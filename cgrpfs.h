#ifndef CGRPFS_H_
#define CGRPFS_H_

#ifdef CGRPFS_PUFFS
#define _KERNTYPES

#include <sys/types.h>

#include <puffs.h>
#endif

#include <sys/queue.h>
#include <sys/stat.h>

#ifdef CGRPFS_THREADED
#include <pthread.h>

#define CGMGR_LOCKED                                                           \
	__attribute__((cleanup(_unlock_cgmgr_))) __attribute__((               \
		unused)) int _unused_lock_ = pthread_mutex_lock(&cgmgr.lock)

void _unlock_cgmgr_(int *unused);
#else
#define CGMGR_LOCKED
#endif

#include "uthash.h"

/* an entry in the pid => node hashtable */
typedef struct pid_hash_entry {
	uintptr_t pid;
	struct cg_node *node;
	UT_hash_handle hh;
} pid_hash_entry_t;

typedef struct poll_request {
	LIST_ENTRY(poll_request) pollreqs;

	struct cg_node *node;
} poll_request_t;

/* a listener for emptiness/exit events */
typedef struct listener {
	LIST_ENTRY(listener) listeners;

	int fd;
} listener_t;

/* kind of CGroupFS node */
typedef enum cg_nodetype {
	CGN_INVALID = -1,
	CGN_EVENTS, /* cgroup.events file */
	CGN_PROCS, /* cgroup.procs file */
	CGN_RELEASE_AGENT, /* release_agent file */
	CGN_NOTIFY_ON_RELEASE, /* notify_on_release file */
	CGN_CG_DIR, /* cgroup directory */
	CGN_PID_ROOT_DIR, /* cgroup.meta root dir */
	CGN_PID_DIR, /* cgroup.meta/$pid directory */
	CGN_PID_CGROUP /* cgroup.meta/$pid/cgroup */
} cg_nodetype_t;

/* node for all entries in the CGroupFS */
typedef struct cg_node {
	LIST_ENTRY(cg_node) entries;

	char *name;
	cg_nodetype_t type;
	struct cg_node *parent;
	struct stat attr;
	bool todel; /* is it to be deleted? */
	int accessed; /* how many kernel handles to it? */

	/* for PID dirs */
	pid_t pid;

	/* for all dirs */
	LIST_HEAD(cg_node_list, cg_node) subnodes;

	/* for cgroup dirs */
	bool notify;
	char *agent;
} cg_node_t;

/* the cgfs manager singleton */
typedef struct cgmgr {
	struct fuse *fuse;
	char *mountpoint;
	int mt; /* is it multithreaded? */
	int kq; /* kernel queue fd */
	int notifyfd; /* notification server fd for exit and emptiness events */

	LIST_HEAD(listeners, listener) listeners;

	pid_hash_entry_t *pidcg; /* map pid => node */

	cg_node_t *rootnode, *metanode;

#ifdef CGRPFS_THREADED
	pthread_mutex_t lock;
	/*
	 * TODO: If it turns out adding events to kqueue from another thread is
	 * not allowed, we'll write a byte to the pipe which the kevent thread
	 * will set a read filter on, so that it's not blocked on kevent() at
	 * the time of adding a new event filter from our other thread.
	 */
	int commfd[2];
#endif
} cgmgr_t;

/* an open file description */
typedef struct cgn_filedesc {
	cg_node_t *node;

	char *buf; /* file contents - pre-filled on open() for consistency */
} cg_filedesc_t;

/* set up the cgmgr */
void cgmgr_init(void);
/* accept a connection on the notify passive socket */
void cgmgr_accept(void);

/* Create a new node and initialise it enough to let delnode not fail */
cg_node_t *newnode(cg_node_t *parent, const char *name, cg_nodetype_t type);
/* Create a new CGroup directory node */
cg_node_t *newcgdir(cg_node_t *parent, const char *name, mode_t perms,
	uid_t uid, gid_t gid);
/* Like delnode but dosen't free it. PUFFS will order a reclaim op later. */
void removenode(cg_node_t *node);
/*
 * Delete a node. Any contained PIDs moved to parent. Subnodes either deleted
 * (if accessed = 0) or marked for deletion.
 */
void delnode(cg_node_t *node);

/* Lookup a node by filename within another node. */
cg_node_t *lookupfile(cg_node_t *node, const char *filename);
/* Lookup a node by path, or the second-last node of that path */
cg_node_t *lookupnode(const char *path, bool secondlast);
/* Get full path of node */
char *nodefullpath(cg_node_t *node);

/* Get file contents of node. */
char *nodetxt(cg_node_t *node);
/* Get cgroups.proc file contents for node */
char *procsfiletxt(cg_node_t *node);

/* Attach a PID to a CGroup */
int attachpid(cg_node_t *node, pid_t pid);
/* Detach a PID from its owner CGroup and stop tracking it if untrack set */
int detachpid(pid_t pid, int wstat, bool untrack);

extern cgmgr_t cgmgr;

#ifdef CGRPFS_PUFFS
PUFFSOP_PROTOS(cgrpfs);
#else
extern struct fuse_operations cgops;
#endif

#endif /* CGRPFS_H_ */
