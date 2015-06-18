/*-------------------------------------------------------------------------
 * NAME:        gpfs_ganesha()
 *
 * FUNCTION:    Use ioctl to call into the GPFS kernel module.
 *              If GPFS isn't loaded they receive ENOSYS.
 *
 * Returns:      0      Successful
 *              -1      Failure
 *
 * Errno:       ENOSYS  No quality of service function available
 *              ENOENT  File not found
 *              EINVAL  Not a GPFS file
 *              ESTALE  cached fs information was invalid
 *-------------------------------------------------------------------------*/

#include "config.h"

#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/vfs.h>
#include <sys/stat.h>

#include "include/gpfs_nfs.h"

struct kxArgs {
	signed long arg1;
	signed long arg2;
};

#ifdef _VALGRIND_MEMCHECK
static void valgrind_kganesha(struct kxArgs *args)
{
	int op = (int)args->arg1;

	switch (op) {
	case OPENHANDLE_STATFS_BY_FH:
	{
		struct statfs_arg *arg = (void *)args->arg2;

		memset(arg->buf, 0, sizeof(*arg->buf));
		break;
	}
	case OPENHANDLE_READ_BY_FD:
	{
		struct read_arg *arg = (void *)args->arg2;

		memset(arg->bufP, 0, arg->length);
		break;
	}
	default:
		break;
	}
}
#endif

int gpfs_ganesha(int op, void *oarg)
{
	int rc;
	static int gpfs_fd = -1;
	struct kxArgs args;

	if (gpfs_fd < 0) {
		gpfs_fd = open(GPFS_DEVNAMEX, O_RDONLY);
		if (gpfs_fd < 0) {
			fprintf(stderr,
				"Ganesha call to GPFS failed with ENOSYS\n");
			return ENOSYS;
		}
		(void)fcntl(gpfs_fd, F_SETFD, FD_CLOEXEC);
	}

	args.arg1 = op;
	args.arg2 = (long)oarg;
#ifdef _VALGRIND_MEMCHECK
	valgrind_kganesha(&args);
#endif
	rc = ioctl(gpfs_fd, kGanesha, &args);

	return rc;
}
