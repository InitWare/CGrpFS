#define _KERNTYPES
#include <sys/types.h>

#ifdef CGRPFS_PUFFS
#include <puffs.h>
#endif

#include <sys/event.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cgrpfs.h"

struct cgdir_nodes {
	cg_nodetype_t type;
	const char *name;
};

#ifdef CGRPFS_THREADED
void
_unlock_cgmgr_(int *unused)
{
	(void)unused;
	pthread_mutex_unlock(&cgmgr.lock);
}

static void *
kqueue_thread(void *unused)
{
	struct kevent kev;

	(void)unused;

	while (true) {
		int r;

		r = kevent(cgmgr.kq, NULL, 0, &kev, 1, NULL);

		pthread_mutex_lock(&cgmgr.lock);

		if (r < 0 && errno != EINTR)
			err(EXIT_FAILURE, "kevent failed");
		else if (r < 0)
			;
		else if (r == 0)
			warn("Got 0 from kevent");
		else if (kev.filter == EVFILT_READ &&
			kev.ident == cgmgr.notifyfd)
			cgmgr_accept();
		else if (kev.filter == EVFILT_PROC) {
			if (kev.fflags & NOTE_CHILD) {
				pid_hash_entry_t *entry;
				uintptr_t ppidp = kev.data; /* parent pid */

				/* find parent pid's node */
				HASH_FIND_PTR(cgmgr.pidcg, &ppidp, entry);

				if (!entry)
					warn("Couldn't find containing CGroup of PID %lld",
						(long long)kev.data);
				else
					attachpid(entry->node, kev.ident);
			} else if (kev.fflags & NOTE_EXIT)
				detachpid(kev.ident, kev.data, false);
			else if (kev.fflags & NOTE_TRACKERR)
				warn("NOTE_TRACKERR received from Kernel Queue");
			else if (kev.fflags & NOTE_EXEC)
				warn("NOTE_EXEC was received");
		} else
			assert(!"Unreached");

		pthread_mutex_unlock(&cgmgr.lock);
	}

	exit(EXIT_FAILURE);
	return NULL;
}
#endif /* CGROUPFS_THREADS */

/* add PID to cgmgr hashtable */
static int
addpidhash(pid_t pid, cg_node_t *node, pid_hash_entry_t **entryout)
{
	pid_hash_entry_t *entry;
	uintptr_t pidp = pid;

	HASH_FIND_PTR(cgmgr.pidcg, &pidp, entry);

	if (entry) {
		entry->node = node;
		*entryout = entry;
		return 0;
	}

	entry = malloc(sizeof *entry);

	if (!entry)
		return -ENOMEM;

	entry->pid = pid;
	entry->node = node;
	HASH_ADD_PTR(cgmgr.pidcg, pid, entry);

	*entryout = entry;

	return 1;
}

cg_node_t *
newnode(cg_node_t *parent, const char *name, cg_nodetype_t type)
{
	cg_node_t *node = malloc(sizeof *node);

	if (!node)
		return NULL;

	node->type = type;
	node->name = name != NULL ? strdup(name) : NULL;
	node->agent = NULL;
	node->notify = false;
	node->parent = parent;
	node->pid = 0;
	node->accessed = false;
	node->todel = false;
	LIST_INIT(&node->subnodes);

	bzero(&node->attr, sizeof(node->attr));

	if (parent != NULL) {
		node->attr.st_uid = parent->attr.st_uid;
		node->attr.st_gid = parent->attr.st_gid;
		LIST_INSERT_HEAD(&parent->subnodes, node, entries);
	}

	return node;
}

static void
movepids(cg_node_t *from, cg_node_t *to)
{
	pid_hash_entry_t *entry, *tmp2;

	HASH_ITER(hh, cgmgr.pidcg, entry, tmp2)
	if (entry->node == from) {
		if (to)
			entry->node = to;
		else
			detachpid(entry->pid, 0, true);
	}
}

void
removenode(cg_node_t *node)
{
	cg_node_t *val, *tmp;

	if (!node->accessed)
		return delnode(node);
	if (node->todel)
		return;

	// printf("Marking node %p for deletion\n", node);
	node->todel = true;

	LIST_FOREACH_SAFE (val, &node->subnodes, entries, tmp)
		if (!val->accessed)
			delnode(val);
		else
			/* if accessed, wait for PUFFS to issue a reclaim op */
			removenode(val);

	/* move up all contained PIDs to parent */
	movepids(node, node->parent);

	if (node->parent)
		LIST_REMOVE(node, entries);
}

void
delnode(cg_node_t *node)
{
	cg_node_t *val, *tmp;

	// printf("Deleting node %p\n", node);

	LIST_FOREACH_SAFE (val, &node->subnodes, entries, tmp)
		if (!val->accessed)
			delnode(val);
		else
			/* if accessed, wait for PUFFS to issue a reclaim op */
			removenode(val);

	/* move up all contained PIDs to parent */
	movepids(node, node->parent);

	if (node->parent)
		LIST_REMOVE(node, entries);

	free(node->name);
	free(node->agent);
	free(node);
}

/* Add standard pseudofiles to a CGroup directory node */
static int
addcgdirfiles(cg_node_t *node)
{
	struct cgdir_nodes nodes[] = { { CGN_EVENTS, "cgroup.events" },
		{ CGN_PROCS, "cgroup.procs" },
		{ CGN_RELEASE_AGENT, "release_agent" },
		{ CGN_NOTIFY_ON_RELEASE, "notify_on_release" },
		{ CGN_INVALID, NULL } };

	for (int i = 0; nodes[i].type != CGN_INVALID; i++) {
		cg_node_t *subnode = newnode(node, nodes[i].name,
			nodes[i].type);

		if (!subnode)
			return -ENOMEM;

		subnode->attr.st_mode = S_IFREG | 0644;
	}

	return 0;
}

cg_node_t *
newcgdir(cg_node_t *parent, const char *name, mode_t perms, uid_t uid,
	gid_t gid)
{
	cg_node_t *node = newnode(parent, name, CGN_CG_DIR);

	node->type = CGN_CG_DIR;
	node->attr.st_mode = S_IFDIR | perms;
	node->attr.st_uid = uid;
	node->attr.st_gid = gid;

	if (addcgdirfiles(node) < 0) {
		warn("Out of memory");
		delnode(node);
		return NULL;
	}

	return node;
}

/* try to add a PID to our mini procfs. these are strictly auto-synthesised */
static cg_node_t *
synthpiddir(pid_t pid)
{
	char buf[32];
	cg_node_t *node;
	struct cgdir_nodes nodes[] = { { CGN_PID_CGROUP, "cgroup" },
		{ CGN_INVALID, NULL } };
	pid_hash_entry_t *entry;
	uintptr_t pidp = pid;
	int r;

	HASH_FIND_PTR(cgmgr.pidcg, &pidp, entry);
	if (!entry) {
		warnx("Entry absent for %lld, creating one", (long long)pid);
		r = attachpid(cgmgr.rootnode, pid);
		if (r == -ESRCH)
			return NULL;
	}

	sprintf(buf, "%lld", (long long)pid);
	node = newnode(cgmgr.metanode, buf, CGN_PID_DIR);
	if (!node)
		return NULL;

	node->pid = pid;
	node->attr.st_mode = S_IFDIR | 0755;
	for (int i = 0; nodes[i].type != CGN_INVALID; i++) {
		cg_node_t *subnode = newnode(node, nodes[i].name,
			nodes[i].type);

		if (!subnode) {
			delnode(node);
			return NULL;
		}

		subnode->attr.st_mode = S_IFREG | 0644;
	}

	return node;
}

cg_node_t *
lookupfile(cg_node_t *node, const char *filename)
{
	cg_node_t *subnode;
	LIST_FOREACH (subnode, &node->subnodes, entries)
		if (!strcmp(subnode->name, filename))
			return subnode;

	/* try to synth a PID dir */
	if (node->type == CGN_PID_ROOT_DIR) {
		char *endptr;
		pid_t pid;
		cg_node_t *pidnode;

		pid = strtol(filename, &endptr, 10);

		if (*endptr != '\0' && *endptr != '/')
			return NULL;

		return synthpiddir(pid);
	}

	return NULL;
}

cg_node_t *
lookupnode(const char *path, bool secondlast)
{
	const char *part = path;
	cg_node_t *node = cgmgr.rootnode;
	bool breaksecondlast = false; /* whether to break on finding 2nd-last */
	bool last = false; /* are we on the last component of the path? */

	while ((part = strstr(part, "/")) != NULL) {
		char *partend;
		size_t partlen;
		bool found = false;
		cg_node_t *subnode;

		if (!*part++) {
			assert(false);
			break; /* reached last part */
		}

		partend = strstr(part, "/");
		if (partend)
			partlen = partend - part;
		else {
			partlen = strlen(part);
			last = true;
		}

		if (secondlast && last && node == cgmgr.rootnode)
			return node;
		else if (last && breaksecondlast)
			return node;
		else if (!strlen(part)) /* root dir */
			return node;

		LIST_FOREACH (subnode, &node->subnodes, entries) {
			if ((strlen(subnode->name) == partlen) &&
				!strncmp(subnode->name, part, partlen)) {
				node = subnode;
				found = true;
				break;
			}
		}

		/* synthesise pid folder under cgroup.meta if absent*/
		if (!found && node->type == CGN_PID_ROOT_DIR) {
			char *endptr;
			pid_t pid;
			cg_node_t *pidnode;

			pid = strtol(part, &endptr, 10);

			if (*endptr != '\0' && *endptr != '/')
				return NULL;

			pidnode = synthpiddir(pid);
			if (pidnode) {
				node = pidnode;
				found = true;
			}
		}

		if (!found && secondlast)
			breaksecondlast = true;
		else if (!found)
			return NULL;
	}

	return node;
}

static char *
nodefullpath_internal(cg_node_t *node)
{
	char *path, *newpath;

	if (node->parent) {
		if ((path = nodefullpath_internal(node->parent)) == NULL)
			return NULL;
	} else
		return strdup(""); /* root node */

	asprintf(&newpath, "%s/%s", path, node->name);
	free(path);
	return newpath;
}

char *
nodefullpath(cg_node_t *node)
{
	if (!node->parent)
		return strdup("/"); /* root node */
	else
		return nodefullpath_internal(node);
}

/* Check if a CGroup node has any PIDs, or if any of its subnodes do. */
static bool
nodepopulated(cg_node_t *node)
{
	bool populated = false;
	pid_hash_entry_t *entry, *tmp2;
	cg_node_t *subnode;

	HASH_ITER(hh, cgmgr.pidcg, entry, tmp2)
	if (entry->node == node)
		return true;

	LIST_FOREACH (subnode, &node->subnodes, entries)
		if (nodepopulated(subnode))
			return true;

	return false;
}

char *
procsfiletxt(cg_node_t *node)
{
	char *txt = NULL;
	char linebuf[33];
	size_t curlen = 0;
	pid_hash_entry_t *entry, *tmp2;

	HASH_ITER(hh, cgmgr.pidcg, entry, tmp2)
	{
		if (entry->node == node->parent) {
			char *newtxt;

			curlen += sprintf(linebuf, "%lld\n",
				(long long)entry->pid);
			newtxt = realloc(txt, curlen + 1);
			if (!newtxt) {
				free(txt);
				warnx("Out of memory");
				return NULL;
			}

			if (!txt) {
				txt = newtxt;
				txt[0] = '\0';
			} else
				txt = newtxt;

			strcat(txt, linebuf);
		}
	}

	return txt ? txt : strdup("");
}

char *
nodetxt(cg_node_t *node)
{
	if (node->type == CGN_PID_CGROUP) {
		pid_hash_entry_t *entry;
		uintptr_t pidp = node->parent->pid;
		char *buf;

		HASH_FIND_PTR(cgmgr.pidcg, &pidp, entry);

		if (!entry)
			/* untracked are in root CGroup by default */
			asprintf(&buf, "1:name=systemd:/\n");
		else {
			char *path = nodefullpath(entry->node);

			if (!path)
				return NULL;

			asprintf(&buf, "1:name=systemd:%s\n", path);
			free(path);
		}

		return buf;

	} else if (node->type == CGN_EVENTS) {
		char *buf;
		return NULL;
	} else if (node->type == CGN_PROCS)
		return procsfiletxt(node);
	else
		return NULL;
}

int
attachpid(cg_node_t *node, pid_t pid)
{
	struct kevent kev;
	int r;
	pid_hash_entry_t *entry;

	assert(node->type == CGN_CG_DIR);
	r = addpidhash(pid, node, &entry);

	if (r < 0)
		warnx("Failed to add PID %lld", (long long)pid);
	else if (r == 0) {
		warnx("Existing entry for %lld\n", (long long)pid);
		return 0;
	}

	/* new PID - must be tracked */
	EV_SET(&kev, pid, EVFILT_PROC, EV_ADD, NOTE_EXIT | NOTE_TRACK, 0, NULL);
	r = kevent(cgmgr.kq, &kev, 1, NULL, 0, NULL);

	if (r < 0) {
		int olderrno = errno;
		/* delete untrackable PID */
		HASH_DEL(cgmgr.pidcg, entry);
		free(entry);
		errno = olderrno;
		warn("Failed to watch PID %lld", (long long)pid);
		return -olderrno;
	} else if (r >= 0)
		return 1;

	return -errno;
}

void
notify_exit(pid_t pid, int wstat)
{
	siginfo_t si;
	listener_t *val, *tmp;

	si.si_pid = pid;
	si.si_signo = SIGCHLD;

	if (WIFEXITED(wstat)) {
		si.si_code = CLD_EXITED;
		si.si_status = WEXITSTATUS(wstat);
	} else if (WIFSIGNALED(wstat)) {
		si.si_code = CLD_KILLED;
		si.si_status = WTERMSIG(wstat);
	}

	LIST_FOREACH_SAFE (val, &cgmgr.listeners, listeners, tmp) {
		ssize_t r = send(val->fd, &si, sizeof si, MSG_NOSIGNAL);
		if (r < 0 && errno == EPIPE) {
			/* remove the listener that disconnected */
			LIST_REMOVE(val, listeners);
			free(val);
		} else if (r < 0)
			warn("Failed to send exit notification");
	}
}

int
detachpid(pid_t pid, int wstat, bool untrack)
{
	struct kevent kev;
	cg_node_t *node;
	pid_hash_entry_t *entry;
	uintptr_t pidp = pid;

	if (untrack) {
		int r;

		EV_SET(&kev, pid, EVFILT_PROC, EV_DELETE, 0, 0, NULL);
		r = kevent(cgmgr.kq, &kev, 1, NULL, 0, NULL);
		if (r < 0)
			warn("Failed to untrack PID %lld", (long long)pid);
	}

	HASH_FIND_PTR(cgmgr.pidcg, &pidp, entry);
	if (!entry)
		warnx("Lost PID without a parent CGroup\n");
	else {
		HASH_DEL(cgmgr.pidcg, entry);
		free(entry);
		if (!untrack)
			notify_exit(pid, wstat);
	}

	return 0;
}

void
cgmgr_init(void)
{
	struct sockaddr_un sun = { .sun_family = AF_UNIX,
		.sun_path = "/var/run/cgrpfs.notify" };
	struct kevent kev;
#ifdef CGRPFS_THREADED
	int r;
	pthread_t thrd;
#endif

	cgmgr.kq = kqueue();
	if ((cgmgr.kq = kqueue()) < 0)
		errx(EXIT_FAILURE, "Failed to open kernel queue.");

#ifdef CGRPFS_THREADED
	if (pthread_mutex_init(&cgmgr.lock, NULL) < 0)
		err(EXIT_FAILURE, "Failed to initialise mutex");

	r = pthread_create(&thrd, NULL, kqueue_thread, NULL);
	if (r != 0)
		errx(EXIT_FAILURE, "pthread_create failed: %s", strerror(r));
#endif

	cgmgr.notifyfd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (cgmgr.notifyfd < 0)
		err(EXIT_FAILURE, "failed to create listener socket");

	unlink(sun.sun_path);

	if (bind(cgmgr.notifyfd, (struct sockaddr *)&sun, SUN_LEN(&sun)) < 0)
		err(EXIT_FAILURE, "failed to bind listener socket");

	if (listen(cgmgr.notifyfd, 10) < 0)
		err(EXIT_FAILURE, "failed to listen on listener socket");

	EV_SET(&kev, cgmgr.notifyfd, EVFILT_READ, EV_ADD, 0, 0, NULL);
	if ((kevent(cgmgr.kq, &kev, 1, NULL, 0, NULL)) < 0)
		err(EXIT_FAILURE,
			"Failed to add event for notify FD to Kernel Queue");

	cgmgr.pidcg = NULL;

	cgmgr.rootnode = newcgdir(NULL, NULL, 0755, 0, 0);
	if (!cgmgr.rootnode)
		errx(EXIT_FAILURE, "Failed to allocate root node.");

	cgmgr.metanode = newnode(cgmgr.rootnode, "cgroup.meta",
		CGN_PID_ROOT_DIR);
	if (!cgmgr.metanode)
		errx(EXIT_FAILURE, "Failed to allocate meta node.");

	cgmgr.metanode->attr.st_mode = S_IFDIR | 0755;

	LIST_INIT(&cgmgr.listeners);
}

void
cgmgr_accept(void)
{
	listener_t *listener = malloc(sizeof *listener);

	if (!listener) {
		warn("Failed to allocate listener");
		return;
	}

	listener->fd = accept(cgmgr.notifyfd, NULL, 0);
	if (listener->fd < 0) {
		warn("Failed to accept listener");
		free(listener);
		return;
	}

	LIST_INSERT_HEAD(&cgmgr.listeners, listener, listeners);
}
