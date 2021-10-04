#define _KERNTYPES

#include <sys/types.h>
#include <puffs.h>
/* these must come first */

#include <err.h>
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
	int pflags = 0, detach = 0, mntflags = 0;
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
	PUFFSOP_SET(pops, cgrpfs, node, access);
	PUFFSOP_SET(pops, cgrpfs, node, getattr);

	if ((pu = puffs_init(pops, _PATH_PUFFS, "cgrpfs", NULL, pflags)) ==
		NULL)
		err(1, "init");

	/*
	puffs_setfhsize(pu, sizeof(struct dtfs_fid),
		PUFFS_FHFLAG_NFSV2 | PUFFS_FHFLAG_NFSV3 |
			(dynamicfh ? PUFFS_FHFLAG_DYNAMIC : 0));
*/

	puffs_set_errnotify(pu, puffs_kernerr_abort);
	if (detach)
		if (puffs_daemon(pu, 1, 1) == -1)
			err(1, "puffs_daemon");

	printf("MNTFLAGS: %d: %s\n", mntflags, *argv);

	if (puffs_mount(pu, *argv, mntflags, cgmgr.rootnode) == -1)
		err(1, "mount");
	if (puffs_mainloop(pu) == -1)
		err(1, "mainloop");

	return 0;
}