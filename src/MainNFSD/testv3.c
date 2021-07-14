/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * ---------------------------------------
 */

/**
 * @file nfs_main.c
 * @brief The file that contain the 'main' routine for the nfsd.
 *
 */
#include "config.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include "log.h"
#include "gsh_rpc.h"
#include "nfs_init.h"
#include "config_parsing.h"
#include "mount.h"
#include "nfs23.h"
#include "nfs_convert.h"

/**
 * @brief LTTng trace enabling magic
 *
 * Every trace include file must be added here regardless whether it
 * is actually used in this source file.  The file must also be
 * included ONLY ONCE.  Failure to do so will create interesting
 * build time failure messages.  The key bit is the definitions of
 * TRACEPOINT_DEFINE and TRACEPOINT_PROBE_DYNAMIC_LINKAGE that are here
 * to trigger the global definitions as a shared object with the right
 * (weak) symbols to make the module loading optional.
 *
 * If and when this file gets some tracepoints of its own, the include
 * here is necessary and sufficient.
 */

#ifdef USE_LTTNG
#define TRACEPOINT_DEFINE
#define TRACEPOINT_PROBE_DYNAMIC_LINKAGE

#include "gsh_lttng/logger.h"
#include "gsh_lttng/mdcache.h"
#include "gsh_lttng/nfs_rpc.h"
#include "gsh_lttng/nfs4.h"
#include "gsh_lttng/state.h"
#include "gsh_lttng/fsal_mem.h"
#include "gsh_lttng/fsal_gluster.h"
#include "gsh_lttng/fsal_ceph.h"
#endif /* USE_LTTNG */

#if DO_CONFIG_STUFF

/* parameters for NFSd startup and default values */
static nfs_start_info_t my_nfs_start_info = {
	.dump_default_config = false,
	.lw_mark_trigger = false,
	.drop_caps = true
};

config_file_t nfs_config_struct;
#endif

char *tempo_exec_name;
char *stderr_path = "STDERR";
char *exec_name = "nfs-ganesha";
char *default_sever = "localhost";

char *nfs_host_name = "localhost";

char *log_path;

CLIENT *mnt_clnt, *nfs_clnt;
AUTH *mnt_auth, *nfs_auth_null, *nfs_auth_user;
char *server_host;
char *mount_dir;
char *test_dir;
/* retry timeout default to the request and back */
static const struct timespec tout = { 30, 0 };
nfs_fh3 mnt_handle;

uid_t user = 5000;
gid_t group = 5000;
int num_gids = 2;
uid_t gids[] = {5001, 5002};
gid_t group2 = 5003;

bool mnt_connect(void)
{
	mnt_clnt = clnt_ncreate(server_host, MOUNTPROG, MOUNT_V3, "tcp");

	if (CLNT_FAILURE(mnt_clnt)) {
		char *err = rpc_sperror(&mnt_clnt->cl_error, "failed");

		LogEvent(COMPONENT_NFSPROTO, "connect to mountd %s", err);
		gsh_free(err);
		CLNT_DESTROY(mnt_clnt);
		mnt_clnt = NULL;
		return false;
	}

	/* split auth (for authnone, idempotent) */
	mnt_auth = authnone_ncreate();

	return true;
}

bool mount(void)
{
	struct clnt_req *cc;
	mnt3_dirpath path = mount_dir;
	struct mountres3 res;
	enum clnt_stat ret;

	memset(&res, 0, sizeof(res));

	cc = gsh_malloc(sizeof(*cc));
	clnt_req_fill(cc, mnt_clnt, mnt_auth, MOUNTPROC3_MNT,
		      (xdrproc_t) xdr_dirpath, &path,
		      (xdrproc_t) xdr_mountres3, &res);
	ret = clnt_req_setup(cc, tout);
	if (ret == RPC_SUCCESS) {
		ret = CLNT_CALL_WAIT(cc);
	}

	if (ret != RPC_SUCCESS) {
		char *err = rpc_sperror(&cc->cc_error, "failed");

		LogCrit(COMPONENT_NFSPROTO,
			"Mount %s MOUNTPROC3_MNT %s",
			path, err);
		gsh_free(err);

		clnt_req_release(cc);
		return false;
	}
	clnt_req_release(cc);

	if (res.fhs_status != MNT3_OK) {
		LogCrit(COMPONENT_NFSPROTO,
			"Mount failed with %d", res.fhs_status);
		return false;
	}

	mnt_handle.data.data_len =
		res.mountres3_u.mountinfo.fhandle.fhandle3_len;
	mnt_handle.data.data_val =
		res.mountres3_u.mountinfo.fhandle.fhandle3_val;

	/* Make sure we don't free the handle we saved. */
	res.mountres3_u.mountinfo.fhandle.fhandle3_len = 0;
	res.mountres3_u.mountinfo.fhandle.fhandle3_val = NULL;

	/* Free the rest of the response */
	xdr_free((xdrproc_t) xdr_mountres3, &res);

	return true;
}

bool nfs_connect(void)
{
	nfs_clnt = clnt_ncreate(server_host, NFS_PROGRAM, NFS_V3, "tcp");

	if (CLNT_FAILURE(nfs_clnt)) {
		char *err = rpc_sperror(&nfs_clnt->cl_error, "failed");

		LogEvent(COMPONENT_NFSPROTO, "connect to nfs server %s", err);
		gsh_free(err);
		CLNT_DESTROY(nfs_clnt);
		nfs_clnt = NULL;
		return false;
	}

	/* split auth (for authnone, idempotent) */
	nfs_auth_null = authnone_ncreate();
	nfs_auth_user = authunix_ncreate(nfs_host_name, user, group,
					 num_gids, gids);

	return true;
}

bool create_01(void)
{
	struct clnt_req *cc;
	CREATE3args args;
	CREATE3res res;
	enum clnt_stat ret;

	memset(&args, 0, sizeof(args));
	memset(&res, 0, sizeof(res));

	args.how.mode = GUARDED;
	args.how.createhow3_u.obj_attributes.mode.set_it = true;
	args.how.createhow3_u.obj_attributes.mode.set_mode3_u.mode = 0644;
	args.how.createhow3_u.obj_attributes.uid.set_it = true;
	args.how.createhow3_u.obj_attributes.uid.set_uid3_u.uid = user;
	args.how.createhow3_u.obj_attributes.gid.set_it = true;
	args.how.createhow3_u.obj_attributes.gid.set_gid3_u.gid = group2;
	args.where.dir = mnt_handle;
	args.where.name = "CREATE_01";

	cc = gsh_malloc(sizeof(*cc));
	clnt_req_fill(cc, nfs_clnt, nfs_auth_user, NFSPROC3_CREATE,
		      (xdrproc_t) xdr_CREATE3args, &args,
		      (xdrproc_t) xdr_CREATE3res, &res);
	ret = clnt_req_setup(cc, tout);
	if (ret == RPC_SUCCESS) {
		ret = CLNT_CALL_WAIT(cc);
	}

	if (ret != RPC_SUCCESS) {
		char *err = rpc_sperror(&cc->cc_error, "failed");

		LogCrit(COMPONENT_NFSPROTO,
			"Mount %s NFSPROC3_CREATE %s",
			args.where.name, err);
		gsh_free(err);

		clnt_req_release(cc);
		return false;
	}
	clnt_req_release(cc);

	if (res.status == NFS3ERR_PERM || res.status == NFS3ERR_ACCES) {
		LogEvent(COMPONENT_NFSPROTO,
			 "%s passed with %s",
			 __func__, nfsstat3_to_str(res.status));
	} else if (res.status != NFS3_OK) {
		LogCrit(COMPONENT_NFSPROTO,
			"%s failed with %s",
			__func__, nfsstat3_to_str(res.status));
	} else {
		LogCrit(COMPONENT_NFSPROTO,
			"%s failed because the file was created",
			__func__);
	}

	xdr_free((xdrproc_t) xdr_CREATE3res, &res);

	return true;
}

/* command line syntax */

static const char options[] = "L:N:f:Chs:m:t:";
static const char usage[] =
	"Usage: %s [-hd][-L <logfile>][-N <dbg_lvl>][-f <config_file>]\n";
/*
	"\t[-L <logfile>]      set the default logfile for the daemon\n"
	"\t[-N <dbg_lvl>]      set the verbosity level\n"
	"\t[-f <config_file>]  set the config file to be used\n"
	"\t[-C]                dump trace when segfault\n"
	"\t[-h]                display this help\n"
	"------------- Default Values -------------\n"
	"LogFile    : SYSLOG\n"
	"DebugLevel : NIV_EVENT\n" "ConfigFile : "GANESHA_CONFIG_PATH"\n";
*/

static inline char *main_strdup(const char *var, const char *str)
{
	char *s = strdup(str);

	if (s == NULL) {
		fprintf(stderr, "strdup failed for %s value %s\n", var, str);
		abort();
	}

	return s;
}

static void cleanup(void)
{
	if (tempo_exec_name)
		free(exec_name);
	if (log_path != stderr_path)
		free(log_path);
	if (mount_dir)
		free(mount_dir);
	if (test_dir)
		free(test_dir);
	if (server_host != default_sever)
		free(server_host);

	xdr_free((xdrproc_t) xdr_fhandle3, &mnt_handle);
}

/**
 * main: simply the main function.
 *
 * The 'main' function as in every C program.
 *
 * @param argc number of arguments
 * @param argv array of arguments
 *
 * @return status to calling program by calling the exit(3C) function.
 *
 */

int main(int argc, char *argv[])
{
	char *tempo_exec_name = NULL;
	char localmachine[MAXHOSTNAMELEN + 1];
	int c;
	int debug_level = -1;
	bool dump_trace = false;
#if DO_CONFIG_STUFF
	struct config_error_type err_type;
#endif

	log_path = stderr_path;

	/* Set the server's boot time and epoch */
	now(&nfs_ServerBootTime);
	nfs_ServerEpoch = (time_t) nfs_ServerBootTime.tv_sec;
	srand(nfs_ServerEpoch);

	tempo_exec_name = strrchr(argv[0], '/');
	if (tempo_exec_name != NULL)
		exec_name = main_strdup("exec_name", tempo_exec_name + 1);

	if (*exec_name == '\0')
		exec_name = argv[0];

	/* get host name */
	if (gethostname(localmachine, sizeof(localmachine)) != 0) {
		fprintf(stderr, "Could not get local host name, exiting...\n");
		exit(1);
	} else {
		nfs_host_name = main_strdup("host_name", localmachine);
	}

	/* now parsing options with getopt */
	while ((c = getopt(argc, argv, options)) != EOF) {
		switch (c) {
		case 'L':
			/* Default Log */
			log_path = main_strdup("log_path", optarg);
			break;

		case 'N':
			/* debug level */
			debug_level = ReturnLevelAscii(optarg);
			if (debug_level == -1) {
				fprintf(stderr,
					"Invalid value for option 'N': NIV_NULL, NIV_MAJ, NIV_CRIT, NIV_EVENT, NIV_DEBUG, NIV_MID_DEBUG or NIV_FULL_DEBUG expected.\n");
				exit(1);
			}
			break;

		case 'f':
			/* config file */

			nfs_config_path = main_strdup("config_path", optarg);
			break;

		case 'C':
			dump_trace = true;
			break;

		case 'h':
			fprintf(stderr, usage, exec_name);
			exit(0);

		case 't':
			test_dir = main_strdup("test_dir", optarg);
			break;

		case 'm':
			mount_dir = main_strdup("mount_dir", optarg);
			break;

		case 's':
			server_host = main_strdup("server_host", optarg);
			break;

		default: /* '?' */
			fprintf(stderr, "Try '%s -h' for usage\n", exec_name);
			exit(1);
		}
	}

	/* initialize memory and logging */
	nfs_prereq_init(exec_name, nfs_host_name, debug_level, log_path,
			dump_trace);

	if (mount_dir == NULL) {
		LogMajor(COMPONENT_INIT, "Must specify a mount directory.");
		exit(1);
	}

	if (server_host == NULL) {
		server_host = default_sever;
		LogInfo(COMPONENT_INIT, "Defaul Server: %s.",
			server_host);
	} else {
		LogInfo(COMPONENT_INIT, "Server: %s.",
			server_host);
	}

	LogInfo(COMPONENT_INIT,
		"Testing will operate on %s:%s%s%s",
		server_host, mount_dir,
		test_dir ? "/" : "",
		test_dir ? test_dir : "");

#if DO_CONFIG_STUFF
	/* Create a memstream for parser+processing error messages */
	if (!init_error_type(&err_type))
		goto fatal_die;

	/* Parse the configuration file so we all know what is going on. */

	if (nfs_config_path == NULL || nfs_config_path[0] == '\0') {
		LogWarn(COMPONENT_INIT,
			"No configuration file named.");
		nfs_config_struct = NULL;
	} else
		nfs_config_struct =
			config_ParseFile(nfs_config_path, &err_type);

	if (!config_error_no_error(&err_type)) {
		char *errstr = err_type_str(&err_type);

		if (!config_error_is_harmless(&err_type)) {
			LogCrit(COMPONENT_INIT,
				 "Error %s while parsing (%s)",
				 errstr != NULL ? errstr : "unknown",
				 nfs_config_path);
			if (errstr != NULL)
				gsh_free(errstr);
			goto fatal_die;
		} else
			LogWarn(COMPONENT_INIT,
				"Error %s while parsing (%s)",
				errstr != NULL ? errstr : "unknown",
				nfs_config_path);
		if (errstr != NULL)
			gsh_free(errstr);
	}

	if (read_log_config(nfs_config_struct, &err_type) < 0) {
		LogCrit(COMPONENT_INIT,
			 "Error while parsing log configuration");
		goto fatal_die;
	}

	/* parse configuration file */

	if (nfs_set_param_from_conf(nfs_config_struct,
				    &my_nfs_start_info,
				    &err_type)) {
		LogCrit(COMPONENT_INIT,
			 "Error setting parameters from configuration file.");
		goto fatal_die;
	}

	report_config_errors(&err_type, NULL, config_errs_to_log);

	/* freeing syntax tree : */

	config_Free(nfs_config_struct);
#endif

	nfs_param.core_param.rpc.max_connections = 1024;
	nfs_param.core_param.rpc.max_send_buffer_size = 1048576*9;
	nfs_param.core_param.rpc.max_recv_buffer_size = 1048576*9;
	nfs_param.core_param.rpc.idle_timeout_s = 300;
	nfs_param.core_param.rpc.ioq_thrd_min = 2;
	nfs_param.core_param.rpc.ioq_thrd_max = 200;

	nfs_Init_svc();

	if (!mnt_connect())
		goto fatal_die;

	if (!mount())
		goto fatal_die;

	if (!nfs_connect())
		goto fatal_die;

	if (!create_01())
		goto fatal_die;

	cleanup();

	return 0;


fatal_die:

#if DO_CONFIG_STUFF
	report_config_errors(&err_type, NULL, config_errs_to_log);
#endif

	cleanup();

	LogCrit(COMPONENT_INIT, "Fatal errors.");

	return 2;
}
