#include <sys/types.h>
#include <sys/event.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define FUSE_USE_VERSION 26
#include <fuse.h>

#include "cgrpfs.h"

cgmgr_t cgmgr;

void
_unlock_cgmgr_(int *unused)
{
	(void)unused;
	pthread_mutex_unlock(&cgmgr.lock);
}

void *
loop(void *unused)
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
				detachpid(kev.ident, false);
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

int
main(int argc, char *argv[])
{
	int r;
	pthread_t thrd;

	cgmgr.kq = kqueue();
	if ((cgmgr.kq = kqueue()) < 0)
		errx(EXIT_FAILURE, "Failed to open kernel queue.");

	if (pthread_mutex_init(&cgmgr.lock, NULL) < 0)
		err(EXIT_FAILURE, "Failed to initialise mutex");

	r = pthread_create(&thrd, NULL, loop, NULL);
	if (r != 0)
		errx(EXIT_FAILURE, "pthread_create failed: %s", strerror(r));

	cgmgr.pidcg = NULL;

	cgmgr.rootnode = newcgdir(NULL, NULL, 0755, 0, 0);
	if (!cgmgr.rootnode)
		errx(EXIT_FAILURE, "Failed to allocate root node.");

	cgmgr.metanode = newnode(cgmgr.rootnode, "cgroup.meta",
		CGN_PID_ROOT_DIR);
	if (!cgmgr.metanode)
		errx(EXIT_FAILURE, "Failed to allocate meta node.");

	cgmgr.metanode->attr.st_mode = S_IFDIR | 0755;

	cgmgr.fuse = fuse_setup(argc, argv, &cgops, sizeof(cgops),
		&cgmgr.mountpoint, &cgmgr.mt, &cgmgr);
	if (!cgmgr.fuse)
		errx(EXIT_FAILURE, "Failed to mount filesystem.");

	printf("CGrpFS mounted at %s\n", cgmgr.mountpoint);

	r = fuse_loop(cgmgr.fuse);
	if (r < 0)
		err(EXIT_FAILURE, "fuse_loop failed");

	fuse_teardown(cgmgr.fuse, cgmgr.mountpoint);
}
