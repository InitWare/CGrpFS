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

int
main(int argc, char *argv[])
{
	cgmgr_init();

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
