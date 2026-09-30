// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * Copyright (c) 2024 NFS-Ganesha Project
 */

#include "config.h"

#ifdef USE_LTTNG
#include "fsal_api.h"
#include "gsh_lttng/gsh_lttng.h"
#if !defined(LTTNG_PARSING)
#include "gsh_lttng/generated_traces/gsh_log.h"
#endif

/**
 * @brief Log facility callback for LTTng.
 *
 * Emits log messages to LTTng userspace tracepoints.
 */
static int log_to_lttng(log_header_t headers, void *private, log_levels_t level,
			struct display_buffer *buffer, char *compstr,
			char *message)
{
	const char *line_to_log =
		(buffer && buffer->b_start)
			? buffer->b_start
			: (compstr ? compstr : (message ? message : ""));

	GSH_AUTO_TRACEPOINT(gsh_log, log_line, TRACE_DEBUG, "{} from lttng",
			    TP_STR(line_to_log));

	return 0;
}

#ifndef LTTNG_PARSING

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <spawn.h>
#include <time.h>

extern char **environ;

#include <lttng/lttng.h>

#include "log.h"
#include "log_lttng.h"
#include "common_utils.h"

#define LTTNG_LIVE_SESSION_NAME "ganesha-live-session"

static pthread_t lttng_reader_thread;
static bool reader_thread_running;
static FILE *babeltrace_pipe;
static const char *log_file_path; /* same path already used by log_to_file */
static bool atfork_registered;

static pid_t owned_sessiond_pid;
static pid_t owned_relayd_pid;

static bool lttng_active; /* true once LTTNG facility is the default */

static void start_lttng_reader_thread(void);
static void stop_lttng_reader_thread(void);

bool lttng_log_is_active(void)
{
	return lttng_active;
}

static void lttng_child_after_fork(void)
{
	/* In the forked child process (e.g. after daemon(0, 0)),
	 * restart reader
	*/
	reader_thread_running = false;
	babeltrace_pipe = NULL;
	lttng_reader_thread = 0;
	start_lttng_reader_thread();
}

/**
 * @brief Background reader thread.
 *
 * Streams live LTTng events via babeltrace2 and writes extracted log lines
 * to the destination log file.
 */
static void *lttng_reader_thread_func(void *arg)
{
	char hostname[128];
	char bt_cmd[512];
	char line[4096];
	FILE *out_fp = NULL;

	if (gethostname(hostname, sizeof(hostname)) != 0)
		strncpy(hostname, "localhost", sizeof(hostname) - 1);

	if (!log_file_path) {
		LogCrit(COMPONENT_LOG,
			"LTTng reader thread: no log_file_path configured");
		return NULL;
	}

	out_fp = fopen(log_file_path, "a");
	if (!out_fp) {
		LogCrit(COMPONENT_LOG,
			"LTTng reader thread failed to open log file %s: %s",
			log_file_path, strerror(errno));
		return NULL;
	}

	/* Flush header/creation to ensure the file
	* exists on disk immediately
	*/
	fflush(out_fp);

	snprintf(bt_cmd, sizeof(bt_cmd),
		 "stdbuf -oL babeltrace2 --input-format=lttng-live "
		 "net://localhost/host/%s/%s 2>/dev/null",
		 hostname, LTTNG_LIVE_SESSION_NAME);

	babeltrace_pipe = popen(bt_cmd, "r");
	if (!babeltrace_pipe) {
		LogCrit(COMPONENT_LOG,
			"LTTng reader thread failed to launch babeltrace2: %s",
			strerror(errno));
		fclose(out_fp);
		return NULL;
	}

	while (reader_thread_running &&
	       fgets(line, sizeof(line), babeltrace_pipe) != NULL) {
		char *match = strstr(line, "arg_3 = \"");

		if (match) {
			match += 9; /* Skip arg_3 = " */
			char *end = strstr(match, "\"");

			if (end) {
				*end = '\0';
				fprintf(out_fp, "[LTTNG] Hurray %s\n", match);
				fflush(out_fp);
			}
		}
	}

	if (babeltrace_pipe) {
		pclose(babeltrace_pipe);
		babeltrace_pipe = NULL;
	}
	if (out_fp) {
		fclose(out_fp);
	}

	return NULL;
}

/*
 * @brief Spawn a background process and wait for it to become ready.
 *
 * The child is spawned WITHOUT POSIX_SPAWN_SETSID, so it stays in ganesha's
 * process group.  Ganesha owns the child's lifecycle: it stores the PID and
 * kills it in lttng_log_facility_shutdown().
 *
 * No --daemonize is passed to the child for the same reason: we need it to
 * stay as a direct child so waitpid() works and SIGTERM reaches it.
 *
 * @param argv        NULL-terminated argument vector; argv[0] is the binary
 *                    name searched via PATH (same semantics as execvp).
 * @param ready_fn    Returns true once the process is ready to serve.
 * @param max_wait_ms Maximum milliseconds to poll for readiness.
 * @param out_pid     Receives the child PID on success.
 *
 * Returns 0 on success, -errno on failure.
 */
static int spawn_and_wait_ready(const char *argv[], bool (*ready_fn)(void),
				int max_wait_ms, pid_t *out_pid)
{
	pid_t pid;
	int ret;

	/*
	 * posix_spawnp searches PATH for argv[0], so bare names like
	 * "lttng-sessiond" work without an absolute path.  No attribute
	 * flags: the child inherits our process group, session, and signal
	 * mask.  It will receive SIGTERM in lttng_log_facility_shutdown().
	 */
	ret = posix_spawnp(&pid, argv[0], NULL, NULL, (char *const *)argv,
			   environ);
	if (ret != 0)
		return -ret;

	/* Poll readiness in 50 ms steps */
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000000L };
	int elapsed = 0;

	while (elapsed < max_wait_ms) {
		nanosleep(&ts, NULL);
		elapsed += 50;
		if (ready_fn()) {
			*out_pid = pid;
			return 0;
		}
	}
	/* End of polling loop */

	/*
	 * Timed out.  Kill the child we spawned rather than leaving it
	 * orphaned, then reap it.
	 */
	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
	return -ETIMEDOUT;
}

/*
 * @brief Probe lttng-sessiond reachability via a raw Unix socket connect.
 *
 * lttng-sessiond exposes a Unix domain socket whose path follows the pattern:
 *
 *   root  : /var/run/lttng/client-lttng-sessiond
 *   user  : $XDG_RUNTIME_DIR/lttng/client-lttng-sessiond
 *           /run/user/<uid>/lttng/client-lttng-sessiond  (common default)
 *           /tmp/lttng-<uid>/client-lttng-sessiond       (fallback)
 *
 * We attempt a non-blocking connect() to the appropriate socket.  A successful
 * connect (or an immediate ECONNREFUSED — meaning the socket exists but the
 * daemon is not yet accepting) both confirm the daemon is present.  ENOENT /
 * EACCES / any other error means it is absent or inaccessible.
 *
 * This replaces lttng_session_daemon_alive() which internally does a sendmsg()
 * and causes liblttng-ctl to print its own PERROR to stderr when the daemon is
 * not running.
 *
 * Returns true if the daemon socket is reachable, false otherwise.
 */
static bool sessiond_socket_reachable(void)
{
	struct sockaddr_un addr;
	char sock_path[sizeof(addr.sun_path)];
	struct stat st;
	int fd, rc;
	uid_t uid = getuid();
	const char *xdg;

	memset(&addr, 0, sizeof(addr));

	if (uid == 0) {
		/* Root session daemon uses a system-wide socket */
		snprintf(sock_path, sizeof(sock_path),
			 "/var/run/lttng/client-lttng-sessiond");
	} else {
		/*
		* Prefer XDG_RUNTIME_DIR (set by
		* systemd/PAM for the session), fall back
		* to the conventional /run/user/<uid> path,
		* and finally to /tmp/lttng-<uid> for
		* systems without a login session.
		*/
		xdg = getenv("XDG_RUNTIME_DIR");
		if (xdg && *xdg)
			snprintf(sock_path, sizeof(sock_path),
				 "%s/lttng/client-lttng-sessiond", xdg);
		else
			snprintf(sock_path, sizeof(sock_path),
				 "/run/user/%d/lttng/client-lttng-sessiond",
				 (int)uid);
	}

	/*
	 * Fast path: stat() the path first.  If it doesn't exist or isn't a
	 * socket we bail immediately without opening a file descriptor.
	 */
	if (stat(sock_path, &st) != 0 || !S_ISSOCK(st.st_mode)) {
		/*
		* Try the /tmp fallback for non-root when XDG_RUNTIME_DIR is set
		* but the lttng sub-directory hasn't been created there.
		*/
		if (uid != 0) {
			snprintf(sock_path, sizeof(sock_path),
				 "/tmp/lttng-%d/client-lttng-sessiond",
				 (int)uid);
			if (stat(sock_path, &st) != 0 || !S_ISSOCK(st.st_mode))
				return false;
		} else {
			return false;
		}
	}

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return false;

	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

	rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
	close(fd);

	/*
	 * connect() == 0            → daemon is up and accepted.
	 * errno == ECONNREFUSED     → socket exists, daemon not
	 *                             yet accepting; treat as
	 *                             present so we don't silently
	 *                             drop tracing on slow start.
	 * Anything else             → daemon absent or not
	 *   (ENOENT, EACCES, …)      accessible.
	 */
	return (rc == 0 || errno == ECONNREFUSED);
}

/*
 * @brief Probe lttng-relayd reachability via a TCP connect to port 5344.
 *
 * lttng-relayd listens on two well-known TCP ports:
 *   5342  — data port   (used by lttng-sessiond to stream trace data)
 *   5344  — live viewer port (used by babeltrace2/live consumers)
 *
 * A live session created with lttng_create_session_live("net://localhost")
 * requires lttng-relayd to be listening on the data port 5342 at creation
 * time.  We probe port 5344 (the live viewer port) as it is equally present
 * whenever the relay daemon is running and is the one babeltrace2 connects to.
 *
 * We do a non-blocking TCP connect() so the probe never blocks: the kernel
 * either completes the three-way handshake immediately (daemon up) or returns
 * ECONNREFUSED synchronously (port closed).  We close the fd right away —
 * no data is ever sent.
 *
 * Returns true if lttng-relayd is listening, false otherwise.
 */
static bool relayd_tcp_reachable(void)
{
	struct sockaddr_in addr;
	int fd, rc;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return false;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(5344); /* lttng-relayd live viewer port */
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 127.0.0.1 */

	rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
	close(fd);

	/*
	 * rc == 0  → lttng-relayd is up and accepted our probe.
	 */
	return (rc == 0);
}

/**
 * @brief Initialize LTTng session and enable tracepoints via liblttng-ctl
 * C API.
 *
 * Auto-launches lttng-sessiond and lttng-relayd if they are not already
 * running.  Daemons launched here stay in ganesha's process group and are
 * terminated in lttng_log_facility_shutdown().  If they were already running
 * before ganesha started they are left untouched on shutdown.
 */
static int setup_lttng_session_api(void)
{
	struct lttng_domain dom;
	struct lttng_handle *handle = NULL;
	struct lttng_event ev;
	int ret;

	/*
	 * Step 1: Ensure lttng-sessiond is running.
	 * If not, spawn it ourselves and poll the Unix socket for up to 3 s.
	 * owned_sessiond_pid stays 0 when it was already running so we don't
	 * kill an externally managed daemon on shutdown.
	 */
	if (!sessiond_socket_reachable()) {
		static const char *const argv[] = { "lttng-sessiond", NULL };

		LogInfo(COMPONENT_LOG,
			"lttng-sessiond not running; auto-launching");
		ret = spawn_and_wait_ready(argv, sessiond_socket_reachable,
					   3000, &owned_sessiond_pid);
		if (ret != 0) {
			LogWarn(COMPONENT_LOG,
				"Failed to auto-launch lttng-sessiond (%s); "
				"LTTng live session unavailable",
				strerror(-ret));
			return ret;
		}
		LogInfo(COMPONENT_LOG, "lttng-sessiond ready (pid %d)",
			(int)owned_sessiond_pid);
	}

	/*
	 * Step 2: Ensure lttng-relayd is running.
	 * Same ownership model: only kill on shutdown if we spawned it.
	 */
	if (!relayd_tcp_reachable()) {
		static const char *const argv[] = { "lttng-relayd", NULL };

		LogInfo(COMPONENT_LOG,
			"lttng-relayd not running; auto-launching");
		ret = spawn_and_wait_ready(argv, relayd_tcp_reachable, 3000,
					   &owned_relayd_pid);
		if (ret != 0) {
			/*
			 * relayd failed: kill the sessiond we may have just
			 * spawned to avoid leaving it orphaned.
			 */
			if (owned_sessiond_pid != 0) {
				kill(owned_sessiond_pid, SIGTERM);
				waitpid(owned_sessiond_pid, NULL, 0);
				owned_sessiond_pid = 0;
			}
			LogWarn(COMPONENT_LOG,
				"Failed to auto-launch lttng-relayd (%s); "
				"LTTng live session unavailable",
				strerror(-ret));
			return ret;
		}
		LogInfo(COMPONENT_LOG, "lttng-relayd ready (pid %d)",
			(int)owned_relayd_pid);
	}

	/* If session already exists from a previous run, destroy it first */
	(void)lttng_destroy_session(LTTNG_LIVE_SESSION_NAME);

	/* Create live session with 1-second live timer
	* (1000000 microseconds)
	*/
	ret = lttng_create_session_live(LTTNG_LIVE_SESSION_NAME,
					"net://localhost", 1000000);
	if (ret < 0) {
		LogWarn(COMPONENT_LOG,
			"lttng_create_session_live failed (%d: %s); "
			"LTTng live session unavailable",
			ret, lttng_strerror(ret));
		return ret;
	}

	memset(&dom, 0, sizeof(dom));
	dom.type = LTTNG_DOMAIN_UST;
	dom.buf_type = LTTNG_BUFFER_PER_UID;

	handle = lttng_create_handle(LTTNG_LIVE_SESSION_NAME, &dom);
	if (!handle) {
		LogCrit(COMPONENT_LOG, "Failed to create LTTng handle");
		ret = -EINVAL;
		goto cleanup_session;
	}

	memset(&ev, 0, sizeof(ev));
	ev.type = LTTNG_EVENT_TRACEPOINT;
	ev.loglevel_type = LTTNG_EVENT_LOGLEVEL_ALL;
	ev.loglevel = -1;
	strncpy(ev.name, "gsh_log:*", sizeof(ev.name) - 1);

	ret = lttng_enable_event_with_exclusions(handle, &ev, NULL, NULL, 0,
						 NULL);
	if (ret < 0 && ret != -LTTNG_ERR_UST_EVENT_EXIST) {
		LogWarn(COMPONENT_LOG,
			"Failed to enable gsh_log tracepoint event: %s",
			lttng_strerror(ret));
		goto cleanup_session;
	}

	lttng_destroy_handle(handle);
	handle = NULL;

	ret = lttng_start_tracing(LTTNG_LIVE_SESSION_NAME);
	if (ret < 0 && ret != -LTTNG_ERR_TRACE_ALREADY_STARTED) {
		LogCrit(COMPONENT_LOG, "Failed to start LTTng tracing: %s",
			lttng_strerror(ret));
		goto cleanup_session;
	}

	return 0;

cleanup_session:
	if (handle)
		lttng_destroy_handle(handle);
	(void)lttng_destroy_session(LTTNG_LIVE_SESSION_NAME);
	return ret;
}

int lttng_log_facility_init(const char *log_path, const char *trace_dir)
{
	int rc;

	/*
	 * The log file path is whatever init_logging() already configured
	 * before calling us: the -L argument or the destination from the
	 * ganesha.conf LOG { Facility { } } block.  The live reader thread
	 * appends to this same file so all output goes to one place.
	 * log_path is NULL when no -L was given; in that case the default
	 * facility is SYSLOG, and the reader thread will not open a file.
	 */
	log_file_path = log_path;

	/*
	 * 1. Attempt to create the LTTng live session.
	 *    Return -1 on failure so caller knows to fall back to FILE.
	 */
	rc = setup_lttng_session_api();
	if (rc != 0) {
		LogWarn(COMPONENT_LOG,
			"LTTng live session setup failed (%d); "
			"continuing with default logging facility",
			rc);
		return rc;
	}

	/* 2. Live session is up — register the LTTng tracepoint facility. */
	rc = create_log_facility("LTTNG", log_to_lttng, NIV_FULL_DEBUG, LH_ALL,
				 NULL);
	if (rc != 0 && rc != -EEXIST) {
		LogCrit(COMPONENT_LOG,
			"Failed to create LTTNG log facility: %s",
			strerror(-rc));
		return rc;
	}

	rc = enable_log_facility("LTTNG");
	if (rc != 0) {
		LogCrit(COMPONENT_LOG,
			"Failed to enable LTTNG log facility: %s",
			strerror(-rc));
		return rc;
	}

	/*
	 * 3. LTTng is now the active writer to the log file via the
	 *    babeltrace2 reader thread.  Disable the FILE facility so the
	 *    same message is not written to the file a second time by
	 *    log_to_file() directly.  SYSLOG is left untouched — it writes
	 *    to the system journal, not to the file, so there is no overlap.
	 *
	 *    FILE may be the default facility (set by init_logging when -L
	 *    is given).  disable_log_facility() refuses to disable the
	 *    default, so we must first promote LTTNG to default — which
	 *    demotes FILE — before disabling it.
	 */
	if (log_file_path) {
		(void)set_default_log_facility("LTTNG");
		(void)disable_log_facility("FILE");
		(void)disable_log_facility("STDERR");
	}

	lttng_active = true;

	/* 4. Register fork handler so child restarts reader after daemon() */
	if (!atfork_registered) {
		pthread_atfork(NULL, NULL, lttng_child_after_fork);
		atfork_registered = true;
	}

	/* 5. Launch live reader thread */
	start_lttng_reader_thread();

	return 0;
}

static void start_lttng_reader_thread(void)
{
	int rc;

	if (reader_thread_running)
		return;

	reader_thread_running = true;
	rc = pthread_create(&lttng_reader_thread, NULL,
			    lttng_reader_thread_func, NULL);
	if (rc != 0) {
		LogCrit(COMPONENT_LOG,
			"Failed to create LTTng reader thread: %s",
			strerror(rc));
		reader_thread_running = false;
	} else {
		LogInfo(COMPONENT_LOG, "LTTng live logging active, output: %s",
			log_file_path);
	}
}

static void stop_lttng_reader_thread(void)
{
	reader_thread_running = false;

	if (babeltrace_pipe) {
		pclose(babeltrace_pipe);
		babeltrace_pipe = NULL;
	}

	if (lttng_reader_thread) {
		pthread_join(lttng_reader_thread, NULL);
		lttng_reader_thread = 0;
	}
}

void lttng_log_facility_shutdown(void)
{
	stop_lttng_reader_thread();

	lttng_active = false;

	/* Programmatically stop and destroy LTTng session via liblttng-ctl */
	(void)lttng_stop_tracing(LTTNG_LIVE_SESSION_NAME);
	(void)lttng_destroy_session(LTTNG_LIVE_SESSION_NAME);

	/*
	 * Restore FILE as the default before disabling LTTNG, otherwise
	 * disable_log_facility() refuses to remove the default logger.
	 * Re-enable FILE first so it is active when set_default_log_facility
	 * switches the default away from LTTNG.
	 */
	if (log_file_path) {
		(void)enable_log_facility("FILE");
		(void)set_default_log_facility("FILE");
	}

	(void)disable_log_facility("LTTNG");
	release_log_facility("LTTNG");

	/*
	 * Shut down daemons that WE launched.  Daemons that were already
	 * running before ganesha started (owned_*_pid == 0) are left alone.
	 */
	if (owned_relayd_pid != 0) {
		LogInfo(COMPONENT_LOG, "Stopping owned lttng-relayd (pid %d)",
			(int)owned_relayd_pid);
		kill(owned_relayd_pid, SIGTERM);
		waitpid(owned_relayd_pid, NULL, 0);
		owned_relayd_pid = 0;
	}

	if (owned_sessiond_pid != 0) {
		LogInfo(COMPONENT_LOG, "Stopping owned lttng-sessiond (pid %d)",
			(int)owned_sessiond_pid);
		kill(owned_sessiond_pid, SIGTERM);
		waitpid(owned_sessiond_pid, NULL, 0);
		owned_sessiond_pid = 0;
	}
}

#endif /* !LTTNG_PARSING */

#endif /* USE_LTTNG */
