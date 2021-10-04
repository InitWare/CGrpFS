#define _KERNTYPES

#include <sys/types.h>
#include <puffs.h>
/* these must come first */

#include <sys/event.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <mntopts.h>
#include <paths.h>
#include <stdlib.h>

#include "cgrpfs.h"

cgmgr_t cgmgr;

static void
usage()
{
	errx(EXIT_FAILURE, "usage: %s [-o mntopt] [-o puffsopt] /mountpoint",
		getprogname());
}

int
main(int argc, char *argv[])
{
	extern char *optarg;
	extern int optind;
	struct puffs_usermount *pu;
	struct puffs_pathobj *po_root;
	struct puffs_ops *pops;
	struct timespec ts;
	const char *typename;
	char *rtstr;
	mntoptparse_t mp;
	int pflags = 0 | PUFFS_FLAG_OPDUMP, detach = 0, mntflags = 0;
	int ch;

	while ((ch = getopt(argc, argv, "o:")) != -1) {
		switch (ch) {
		case 'o':
			mp = getmntopts(optarg, puffsmopts, &mntflags, &pflags);
			if (mp == NULL)
				err(1, "getmntopts");
			freemntopts(mp);
			break;
		default:
			usage();
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 1)
		usage();

	cgmgr_init();

	PUFFSOP_INIT(pops);
	puffs_null_setops(pops);

	PUFFSOP_SETFSNOP(pops, sync);
	PUFFSOP_SETFSNOP(pops, statvfs);

	PUFFSOP_SET(pops, cgrpfs, node, lookup);
	PUFFSOP_SET(pops, cgrpfs, node, open);
	PUFFSOP_SET(pops, cgrpfs, node, open2);
	PUFFSOP_SET(pops, cgrpfs, node, mkdir);
	PUFFSOP_SET(pops, cgrpfs, node, rmdir);
	PUFFSOP_SET(pops, cgrpfs, node, access);
	PUFFSOP_SET(pops, cgrpfs, node, getattr);
	PUFFSOP_SET(pops, cgrpfs, node, setattr);
	PUFFSOP_SET(pops, cgrpfs, node, readdir);
	PUFFSOP_SET(pops, cgrpfs, node, rename);
	PUFFSOP_SET(pops, cgrpfs, node, read);
	PUFFSOP_SET(pops, cgrpfs, node, write);
	PUFFSOP_SET(pops, cgrpfs, node, inactive);
	PUFFSOP_SET(pops, cgrpfs, node, reclaim);

	if ((pu = puffs_init(pops, _PATH_PUFFS, "cgrpfs", NULL, pflags)) ==
		NULL)
		err(1, "init");

		/*
	 * The framebuf interface is useless to us as it tries to add a write
	 * event filter, which doesn't work on kernel queues.
	 * We could maybe have a separate thread wait for the KQ FD to become
	 * ready then sent a byte along a pipe instead, but that can be done
	 * another time.
	 */
#if 0
	puffs_framev_init(pu, kq_fdread_fn, NULL, NULL, NULL, NULL);
	if (puffs_framev_addfd(pu, cgmgr.kq, PUFFS_FBIO_READ) < 0)
		err(1, "framebuf addfd kq");
#endif

	puffs_set_errnotify(pu, puffs_kernerr_abort);
	if (detach)
		if (puffs_daemon(pu, 1, 1) == -1)
			err(1, "puffs_daemon");

	if (puffs_mount(pu, *argv, mntflags, cgmgr.rootnode) == -1)
		err(1, "mount");
	if (puffs_mainloop(pu) == -1)
		err(1, "mainloop");

	return 0;
}