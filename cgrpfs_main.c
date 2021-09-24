#include <sys/types.h>
#include <sys/event.h>

#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <fuse_lowlevel.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cgrpfs.h"

cgmgr_t cgmgr;

int
loop()
{
	struct fuse_session *se;
	struct fuse_chan *ch;
	int fd;
	struct kevent kev;
	size_t bufsize;
	char *buf;

	se = fuse_get_session(cgmgr.fuse);
	if (!se)
		return -1;

	ch = fuse_session_next_chan(se, NULL);
	if (!ch)
		return -1;

	bufsize = fuse_chan_bufsize(ch);
	buf = malloc(bufsize);
	if (!buf)
		errx(EXIT_FAILURE, "Failed to allocate buffer");

	fd = fuse_chan_fd(ch);

	EV_SET(&kev, fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
	if ((kevent(cgmgr.kq, &kev, 1, NULL, 0, NULL)) < 0)
		err(EXIT_FAILURE, "Failed to add event to Kernel Queue");

	while (!fuse_session_exited(se)) {
		int r;
		struct fuse_chan *tmpch = ch;
		struct fuse_buf fbuf = {
			.mem = buf,
			.size = bufsize,
		};

		r = kevent(cgmgr.kq, NULL, 0, &kev, 1, NULL);

		if (r < 0 && errno != EINTR)
			err(EXIT_FAILURE, "kevent failed");
		else if (r < 0)
			break;
		else if (r == 0)
			warn("Got 0 from kevent");
		else if (kev.filter == EVFILT_READ) {
			r = fuse_session_receive_buf(se, &fbuf, &tmpch);

			if (r == -EINTR)
				continue;
			if (r <= 0)
				errx(EXIT_FAILURE, "Got <0 from fuse");

			fuse_session_process_buf(se, &fbuf, tmpch);
		} else if (kev.filter == EVFILT_PROC) {
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
	}

	free(buf);
	fuse_session_reset(se);

	return 0;
}

int
main(int argc, char *argv[])
{
	cgmgr.kq = kqueue();
	if ((cgmgr.kq = kqueue()) < 0)
		errx(EXIT_FAILURE, "Failed to open kernel queue.");

	cgmgr.pidcg = NULL;

	cgmgr.rootnode = newcgdir(NULL, NULL, 0755, 0, 0);
	if (!cgmgr.rootnode)
		errx(EXIT_FAILURE, "Failed to allocate root node.");

	cgmgr.metanode =
		newnode(cgmgr.rootnode, "cgroup.meta", CGN_PID_ROOT_DIR);
	if (!cgmgr.metanode)
		errx(EXIT_FAILURE, "Failed to allocate meta node.");

	cgmgr.metanode->attr.st_mode = S_IFDIR | 0755;

	cgmgr.fuse = fuse_setup(argc, argv, &cgops, sizeof(cgops),
		&cgmgr.mountpoint, &cgmgr.mt, &cgmgr);
	if (!cgmgr.fuse)
		errx(EXIT_FAILURE, "Failed to mount filesystem.");

	printf("CGrpFS mounted at %s\n", cgmgr.mountpoint);

	loop();

	fuse_teardown(cgmgr.fuse, cgmgr.mountpoint);
}
