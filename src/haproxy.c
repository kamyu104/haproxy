/*
 * HA-Proxy : High Availability-enabled HTTP/TCP proxy
 * Copyright 2000-2021 Willy Tarreau <willy@haproxy.org>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * Please refer to RFC7230 - RFC7235 information about HTTP protocol, and
 * RFC6265 for information about cookies usage. More generally, the IETF HTTP
 * Working Group's web site should be consulted for protocol related changes :
 *
 *     http://ftp.ics.uci.edu/pub/ietf/http/
 *
 * Pending bugs (may be not fixed because never reproduced) :
 *   - solaris only : sometimes, an HTTP proxy with only a dispatch address causes
 *     the proxy to terminate (no core) if the client breaks the connection during
 *     the response. Seen on 1.1.8pre4, but never reproduced. May not be related to
 *     the snprintf() bug since requests were simple (GET / HTTP/1.0), but may be
 *     related to missing setsid() (fixed in 1.1.15)
 *   - a proxy with an invalid config will prevent the startup even if disabled.
 *
 * ChangeLog has moved to the CHANGELOG file.
 *
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <syslog.h>
#include <grp.h>
#ifdef USE_CPU_AFFINITY
#include <sched.h>
#if defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/param.h>
#ifdef __FreeBSD__
#include <sys/cpuset.h>
#endif
#include <pthread_np.h>
#endif
#ifdef __APPLE__
#include <mach/mach_types.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#endif
#endif

#if defined(USE_PRCTL)
#include <sys/prctl.h>
#endif

#ifdef DEBUG_FULL
#include <assert.h>
#endif
#if defined(USE_SYSTEMD)
#include <systemd/sd-daemon.h>
#endif

#include <import/sha1.h>

#include <haproxy/acl.h>
#include <haproxy/activity.h>
#include <haproxy/api.h>
#include <haproxy/arg.h>
#include <haproxy/auth.h>
#include <haproxy/base64.h>
#include <haproxy/capture-t.h>
#include <haproxy/cfgparse.h>
#include <haproxy/chunk.h>
#include <haproxy/cli.h>
#include <haproxy/connection.h>
#include <haproxy/dns.h>
#include <haproxy/dynbuf.h>
#include <haproxy/errors.h>
#include <haproxy/fd.h>
#include <haproxy/filters.h>
#include <haproxy/global.h>
#include <haproxy/hlua.h>
#include <haproxy/http_rules.h>
#include <haproxy/list.h>
#include <haproxy/listener.h>
#include <haproxy/log.h>
#include <haproxy/mworker.h>
#include <haproxy/namespace.h>
#include <haproxy/net_helper.h>
#include <haproxy/openssl-compat.h>
#include <haproxy/pattern.h>
#include <haproxy/peers.h>
#include <haproxy/pool.h>
#include <haproxy/protocol.h>
#include <haproxy/proto_tcp.h>
#include <haproxy/proxy.h>
#include <haproxy/regex.h>
#include <haproxy/sample.h>
#include <haproxy/server.h>
#include <haproxy/session.h>
#include <haproxy/signal.h>
#include <haproxy/sock.h>
#include <haproxy/sock_inet.h>
#include <haproxy/ssl_sock.h>
#include <haproxy/stats-t.h>
#include <haproxy/stream.h>
#include <haproxy/task.h>
#include <haproxy/thread.h>
#include <haproxy/time.h>
#include <haproxy/tools.h>
#include <haproxy/uri_auth-t.h>
#include <haproxy/vars.h>
#include <haproxy/version.h>


/* array of init calls for older platforms */
DECLARE_INIT_STAGES;

/* list of config files */
static struct list cfg_cfgfiles = LIST_HEAD_INIT(cfg_cfgfiles);
int  pid;			/* current process id */
int  relative_pid = 1;		/* process id starting at 1 */
unsigned long pid_bit = 1;      /* bit corresponding to the process id */
unsigned long all_proc_mask = 1; /* mask of all processes */

volatile unsigned long sleeping_thread_mask = 0; /* Threads that are about to sleep in poll() */
volatile unsigned long stopping_thread_mask = 0; /* Threads acknowledged stopping */

/* global options */
struct global global = {
	.hard_stop_after = TICK_ETERNITY,
	.nbproc = 1,
	.nbthread = 0,
	.req_count = 0,
	.logsrvs = LIST_HEAD_INIT(global.logsrvs),
	.maxzlibmem = 0,
	.comp_rate_lim = 0,
	.ssl_server_verify = SSL_SERVER_VERIFY_REQUIRED,
	.unix_bind = {
		 .ux = {
			 .uid = -1,
			 .gid = -1,
			 .mode = 0,
		 }
	},
	.tune = {
		.options = GTUNE_LISTENER_MQ,
		.bufsize = (BUFSIZE + 2*sizeof(void *) - 1) & -(2*sizeof(void *)),
		.maxrewrite = MAXREWRITE,
		.reserved_bufs = RESERVED_BUFS,
		.pattern_cache = DEFAULT_PAT_LRU_SIZE,
		.pool_low_ratio  = 20,
		.pool_high_ratio = 25,
		.max_http_hdr = MAX_HTTP_HDR,
#ifdef USE_OPENSSL
		.sslcachesize = SSLCACHESIZE,
#endif
		.comp_maxlevel = 1,
#ifdef DEFAULT_IDLE_TIMER
		.idle_timer = DEFAULT_IDLE_TIMER,
#else
		.idle_timer = 1000, /* 1 second */
#endif
	},
#ifdef USE_OPENSSL
#ifdef DEFAULT_MAXSSLCONN
	.maxsslconn = DEFAULT_MAXSSLCONN,
#endif
#endif
	/* others NULL OK */
};

/*********************************************************************/

int stopping;	/* non zero means stopping in progress */
int killed;	/* non zero means a hard-stop is triggered */
int jobs = 0;   /* number of active jobs (conns, listeners, active tasks, ...) */
int unstoppable_jobs = 0;  /* number of active jobs that can't be stopped during a soft stop */
int active_peers = 0; /* number of active peers (connection attempts and connected) */
int connected_peers = 0; /* number of connected peers (verified ones) */

/* Here we store information about the pids of the processes we may pause
 * or kill. We will send them a signal every 10 ms until we can bind to all
 * our ports. With 200 retries, that's about 2 seconds.
 */
#define MAX_START_RETRIES	200
static int *oldpids = NULL;
static int oldpids_sig; /* use USR1 or TERM */

/* Path to the unix socket we use to retrieve listener sockets from the old process */
static const char *old_unixsocket;

static char *cur_unixsocket = NULL;

int atexit_flag = 0;

int nb_oldpids = 0;
const int zero = 0;
const int one = 1;
const struct linger nolinger = { .l_onoff = 1, .l_linger = 0 };

char hostname[MAX_HOSTNAME_LEN];
char *localpeer = NULL;

static char **old_argv = NULL; /* previous argv but cleaned up */

struct list proc_list = LIST_HEAD_INIT(proc_list);

int master = 0; /* 1 if in master, 0 if in child */
unsigned int rlim_fd_cur_at_boot = 0;
unsigned int rlim_fd_max_at_boot = 0;

/* per-boot randomness */
unsigned char boot_seed[20];        /* per-boot random seed (160 bits initially) */

struct mworker_proc *proc_self = NULL;

static void *run_thread_poll_loop(void *data);

/* bitfield of a few warnings to emit just once (WARN_*) */
unsigned int warned = 0;

/* master CLI configuration (-S flag) */
struct list mworker_cli_conf = LIST_HEAD_INIT(mworker_cli_conf);

/* These are strings to be reported in the output of "haproxy -vv". They may
 * either be constants (in which case must_free must be zero) or dynamically
 * allocated strings to pass to free() on exit, and in this case must_free
 * must be non-zero.
 */
struct list build_opts_list = LIST_HEAD_INIT(build_opts_list);
struct build_opts_str {
	struct list list;
	const char *str;
	int must_free;
};

/* These functions are called just after the point where the program exits
 * after a config validity check, so they are generally suited for resource
 * allocation and slow initializations that should be skipped during basic
 * config checks. The functions must return 0 on success, or a combination
 * of ERR_* flags (ERR_WARN, ERR_ABORT, ERR_FATAL, ...). The 2 latter cause
 * and immediate exit, so the function must have emitted any useful error.
 */
struct list post_check_list = LIST_HEAD_INIT(post_check_list);
struct post_check_fct {
	struct list list;
	int (*fct)();
};

/* These functions are called for each proxy just after the config validity
 * check. The functions must return 0 on success, or a combination of ERR_*
 * flags (ERR_WARN, ERR_ABORT, ERR_FATAL, ...). The 2 latter cause and immediate
 * exit, so the function must have emitted any useful error.
 */
struct list post_proxy_check_list = LIST_HEAD_INIT(post_proxy_check_list);
struct post_proxy_check_fct {
	struct list list;
	int (*fct)(struct proxy *);
};

/* These functions are called for each server just after the config validity
 * check. The functions must return 0 on success, or a combination of ERR_*
 * flags (ERR_WARN, ERR_ABORT, ERR_FATAL, ...). The 2 latter cause and immediate
 * exit, so the function must have emitted any useful error.
 */
struct list post_server_check_list = LIST_HEAD_INIT(post_server_check_list);
struct post_server_check_fct {
	struct list list;
	int (*fct)(struct server *);
};

/* These functions are called for each thread just after the thread creation
 * and before running the init functions. They should be used to do per-thread
 * (re-)allocations that are needed by subsequent functoins. They must return 0
 * if an error occurred. */
struct list per_thread_alloc_list = LIST_HEAD_INIT(per_thread_alloc_list);
struct per_thread_alloc_fct {
	struct list list;
	int (*fct)();
};

/* These functions are called for each thread just after the thread creation
 * and before running the scheduler. They should be used to do per-thread
 * initializations. They must return 0 if an error occurred. */
struct list per_thread_init_list = LIST_HEAD_INIT(per_thread_init_list);
struct per_thread_init_fct {
	struct list list;
	int (*fct)();
};

/* These functions are called when freeing the global sections at the end of
 * deinit, after everything is stopped. They don't return anything. They should
 * not release shared resources that are possibly used by other deinit
 * functions, only close/release what is private. Use the per_thread_free_list
 * to release shared resources.
 */
struct list post_deinit_list = LIST_HEAD_INIT(post_deinit_list);
struct post_deinit_fct {
	struct list list;
	void (*fct)();
};

/* These functions are called when freeing a proxy during the deinit, after
 * everything isg stopped. They don't return anything. They should not release
 * the proxy itself or any shared resources that are possibly used by other
 * deinit functions, only close/release what is private.
 */
struct list proxy_deinit_list = LIST_HEAD_INIT(proxy_deinit_list);
struct proxy_deinit_fct {
	struct list list;
	void (*fct)(struct proxy *);
};

/* These functions are called when freeing a server during the deinit, after
 * everything isg stopped. They don't return anything. They should not release
 * the proxy itself or any shared resources that are possibly used by other
 * deinit functions, only close/release what is private.
 */
struct list server_deinit_list = LIST_HEAD_INIT(server_deinit_list);
struct server_deinit_fct {
	struct list list;
	void (*fct)(struct server *);
};

/* These functions are called when freeing the global sections at the end of
 * deinit, after the thread deinit functions, to release unneeded memory
 * allocations. They don't return anything, and they work in best effort mode
 * as their sole goal is to make valgrind mostly happy.
 */
struct list per_thread_free_list = LIST_HEAD_INIT(per_thread_free_list);
struct per_thread_free_fct {
	struct list list;
	void (*fct)();
};

/* These functions are called for each thread just after the scheduler loop and
 * before exiting the thread. They don't return anything and, as for post-deinit
 * functions, they work in best effort mode as their sole goal is to make
 * valgrind mostly happy. */
struct list per_thread_deinit_list = LIST_HEAD_INIT(per_thread_deinit_list);
struct per_thread_deinit_fct {
	struct list list;
	void (*fct)();
};

/*********************************************************************/
/*  general purpose functions  ***************************************/
/*********************************************************************/

/* used to register some build option strings at boot. Set must_free to
 * non-zero if the string must be freed upon exit.
 */
void hap_register_build_opts(const char *str, int must_free)
{
	struct build_opts_str *b;

	b = calloc(1, sizeof(*b));
	if (!b) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	b->str = str;
	b->must_free = must_free;
	LIST_ADDQ(&build_opts_list, &b->list);
}

/* used to register some initialization functions to call after the checks. */
void hap_register_post_check(int (*fct)())
{
	struct post_check_fct *b;

	b = calloc(1, sizeof(*b));
	if (!b) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	b->fct = fct;
	LIST_ADDQ(&post_check_list, &b->list);
}

/* used to register some initialization functions to call for each proxy after
 * the checks.
 */
void hap_register_post_proxy_check(int (*fct)(struct proxy *))
{
	struct post_proxy_check_fct *b;

	b = calloc(1, sizeof(*b));
	if (!b) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	b->fct = fct;
	LIST_ADDQ(&post_proxy_check_list, &b->list);
}

/* used to register some initialization functions to call for each server after
 * the checks.
 */
void hap_register_post_server_check(int (*fct)(struct server *))
{
	struct post_server_check_fct *b;

	b = calloc(1, sizeof(*b));
	if (!b) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	b->fct = fct;
	LIST_ADDQ(&post_server_check_list, &b->list);
}

/* used to register some de-initialization functions to call after everything
 * has stopped.
 */
void hap_register_post_deinit(void (*fct)())
{
	struct post_deinit_fct *b;

	b = calloc(1, sizeof(*b));
	if (!b) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	b->fct = fct;
	LIST_ADDQ(&post_deinit_list, &b->list);
}

/* used to register some per proxy de-initialization functions to call after
 * everything has stopped.
 */
void hap_register_proxy_deinit(void (*fct)(struct proxy *))
{
	struct proxy_deinit_fct *b;

	b = calloc(1, sizeof(*b));
	if (!b) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	b->fct = fct;
	LIST_ADDQ(&proxy_deinit_list, &b->list);
}


/* used to register some per server de-initialization functions to call after
 * everything has stopped.
 */
void hap_register_server_deinit(void (*fct)(struct server *))
{
	struct server_deinit_fct *b;

	b = calloc(1, sizeof(*b));
	if (!b) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	b->fct = fct;
	LIST_ADDQ(&server_deinit_list, &b->list);
}

/* used to register some allocation functions to call for each thread. */
void hap_register_per_thread_alloc(int (*fct)())
{
	struct per_thread_alloc_fct *b;

	b = calloc(1, sizeof(*b));
	if (!b) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	b->fct = fct;
	LIST_ADDQ(&per_thread_alloc_list, &b->list);
}

/* used to register some initialization functions to call for each thread. */
void hap_register_per_thread_init(int (*fct)())
{
	struct per_thread_init_fct *b;

	b = calloc(1, sizeof(*b));
	if (!b) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	b->fct = fct;
	LIST_ADDQ(&per_thread_init_list, &b->list);
}

/* used to register some de-initialization functions to call for each thread. */
void hap_register_per_thread_deinit(void (*fct)())
{
	struct per_thread_deinit_fct *b;

	b = calloc(1, sizeof(*b));
	if (!b) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	b->fct = fct;
	LIST_ADDQ(&per_thread_deinit_list, &b->list);
}

/* used to register some free functions to call for each thread. */
void hap_register_per_thread_free(void (*fct)())
{
	struct per_thread_free_fct *b;

	b = calloc(1, sizeof(*b));
	if (!b) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	b->fct = fct;
	LIST_ADDQ(&per_thread_free_list, &b->list);
}

static void display_version()
{
	struct utsname utsname;

	printf("HA-Proxy version %s %s - https://haproxy.org/\n"
	       PRODUCT_STATUS "\n", haproxy_version, haproxy_date);

	if (strlen(PRODUCT_URL_BUGS) > 0) {
		char base_version[20];
		int dots = 0;
		char *del;

		/* only retrieve the base version without distro-specific extensions */
		for (del = haproxy_version; *del; del++) {
			if (*del == '.')
				dots++;
			else if (*del < '0' || *del > '9')
				break;
		}

		strlcpy2(base_version, haproxy_version, del - haproxy_version + 1);
		if (dots < 2)
			printf("Known bugs: https://github.com/haproxy/haproxy/issues?q=is:issue+is:open\n");
		else
			printf("Known bugs: " PRODUCT_URL_BUGS "\n", base_version);
	}

	if (uname(&utsname) == 0) {
		printf("Running on: %s %s %s %s\n", utsname.sysname, utsname.release, utsname.version, utsname.machine);
	}
}

static void display_build_opts()
{
	struct build_opts_str *item;

	printf("Build options :"
#ifdef BUILD_TARGET
	       "\n  TARGET  = " BUILD_TARGET
#endif
#ifdef BUILD_CPU
	       "\n  CPU     = " BUILD_CPU
#endif
#ifdef BUILD_CC
	       "\n  CC      = " BUILD_CC
#endif
#ifdef BUILD_CFLAGS
	       "\n  CFLAGS  = " BUILD_CFLAGS
#endif
#ifdef BUILD_OPTIONS
	       "\n  OPTIONS = " BUILD_OPTIONS
#endif
#ifdef BUILD_DEBUG
	       "\n  DEBUG   = " BUILD_DEBUG
#endif
#ifdef BUILD_FEATURES
	       "\n\nFeature list : " BUILD_FEATURES
#endif
	       "\n\nDefault settings :"
	       "\n  bufsize = %d, maxrewrite = %d, maxpollevents = %d"
	       "\n\n",
	       BUFSIZE, MAXREWRITE, MAX_POLL_EVENTS);

	list_for_each_entry(item, &build_opts_list, list) {
		puts(item->str);
	}

	putchar('\n');

	list_pollers(stdout);
	putchar('\n');
	list_mux_proto(stdout);
	putchar('\n');
	list_services(stdout);
	putchar('\n');
	list_filters(stdout);
	putchar('\n');
}

/*
 * This function prints the command line usage and exits
 */
static void usage(char *name)
{
	display_version();
	fprintf(stderr,
		"Usage : %s [-f <cfgfile|cfgdir>]* [ -vdV"
		"D ] [ -n <maxconn> ] [ -N <maxpconn> ]\n"
		"        [ -p <pidfile> ] [ -m <max megs> ] [ -C <dir> ] [-- <cfgfile>*]\n"
		"        -v displays version ; -vv shows known build options.\n"
		"        -d enters debug mode ; -db only disables background mode.\n"
		"        -dM[<byte>] poisons memory with <byte> (defaults to 0x50)\n"
		"        -V enters verbose mode (disables quiet mode)\n"
		"        -D goes daemon ; -C changes to <dir> before loading files.\n"
		"        -W master-worker mode.\n"
#if defined(USE_SYSTEMD)
		"        -Ws master-worker mode with systemd notify support.\n"
#endif
		"        -q quiet mode : don't display messages\n"
		"        -c check mode : only check config files and exit\n"
		"        -n sets the maximum total # of connections (uses ulimit -n)\n"
		"        -m limits the usable amount of memory (in MB)\n"
		"        -N sets the default, per-proxy maximum # of connections (%d)\n"
		"        -L set local peer name (default to hostname)\n"
		"        -p writes pids of all children to this file\n"
#if defined(USE_EPOLL)
		"        -de disables epoll() usage even when available\n"
#endif
#if defined(USE_KQUEUE)
		"        -dk disables kqueue() usage even when available\n"
#endif
#if defined(USE_EVPORTS)
		"        -dv disables event ports usage even when available\n"
#endif
#if defined(USE_POLL)
		"        -dp disables poll() usage even when available\n"
#endif
#if defined(USE_LINUX_SPLICE)
		"        -dS disables splice usage (broken on old kernels)\n"
#endif
#if defined(USE_GETADDRINFO)
		"        -dG disables getaddrinfo() usage\n"
#endif
#if defined(SO_REUSEPORT)
		"        -dR disables SO_REUSEPORT usage\n"
#endif
		"        -dr ignores server address resolution failures\n"
		"        -dV disables SSL verify on servers side\n"
		"        -dW fails if any warning is emitted\n"
		"        -sf/-st [pid ]* finishes/terminates old pids.\n"
		"        -x <unix_socket> get listening sockets from a unix socket\n"
		"        -S <bind>[,<bind options>...] new master CLI\n"
		"\n",
		name, cfg_maxpconn);
	exit(1);
}



/*********************************************************************/
/*   more specific functions   ***************************************/
/*********************************************************************/

/* sends the signal <sig> to all pids found in <oldpids>. Returns the number of
 * pids the signal was correctly delivered to.
 */
int tell_old_pids(int sig)
{
	int p;
	int ret = 0;
	for (p = 0; p < nb_oldpids; p++)
		if (kill(oldpids[p], sig) == 0)
			ret++;
	return ret;
}

/*
 * remove a pid forom the olpid array and decrease nb_oldpids
 * return 1 pid was found otherwise return 0
 */

int delete_oldpid(int pid)
{
	int i;

	for (i = 0; i < nb_oldpids; i++) {
		if (oldpids[i] == pid) {
			oldpids[i] = oldpids[nb_oldpids - 1];
			oldpids[nb_oldpids - 1] = 0;
			nb_oldpids--;
			return 1;
		}
	}
	return 0;
}


static void get_cur_unixsocket()
{
	/* if -x was used, try to update the stat socket if not available anymore */
	if (global.stats_fe) {
		struct bind_conf *bind_conf;

		/* pass through all stats socket */
		list_for_each_entry(bind_conf, &global.stats_fe->conf.bind, by_fe) {
			struct listener *l;

			list_for_each_entry(l, &bind_conf->listeners, by_bind) {

				if (l->rx.addr.ss_family == AF_UNIX &&
				    (bind_conf->level & ACCESS_FD_LISTENERS)) {
					const struct sockaddr_un *un;

					un = (struct sockaddr_un *)&l->rx.addr;
					/* priority to old_unixsocket */
					if (!cur_unixsocket) {
						cur_unixsocket = strdup(un->sun_path);
					} else {
						if (old_unixsocket && strcmp(un->sun_path, old_unixsocket) == 0) {
							free(cur_unixsocket);
							cur_unixsocket = strdup(old_unixsocket);
							return;
						}
					}
				}
			}
		}
	}
	if (!cur_unixsocket && old_unixsocket)
		cur_unixsocket = strdup(old_unixsocket);
}

/*
 * When called, this function reexec haproxy with -sf followed by current
 * children PIDs and possibly old children PIDs if they didn't leave yet.
 */
void mworker_reload()
{
	char **next_argv = NULL;
	int old_argc = 0; /* previous number of argument */
	int next_argc = 0;
	int i = 0;
	char *msg = NULL;
	struct rlimit limit;
	struct per_thread_deinit_fct *ptdf;

	mworker_block_signals();
#if defined(USE_SYSTEMD)
	if (global.tune.options & GTUNE_USE_SYSTEMD)
		sd_notify(0, "RELOADING=1");
#endif
	setenv("HAPROXY_MWORKER_REEXEC", "1", 1);

	mworker_proc_list_to_env(); /* put the children description in the env */

	/* during the reload we must ensure that every FDs that can't be
	 * reuse (ie those that are not referenced in the proc_list)
	 * are closed or they will leak. */

	/* close the listeners FD */
	mworker_cli_proxy_stop();

	if (getenv("HAPROXY_MWORKER_WAIT_ONLY") == NULL) {
		/* close the poller FD and the thread waker pipe FD */
		list_for_each_entry(ptdf, &per_thread_deinit_list, list)
			ptdf->fct();
		if (fdtab)
			deinit_pollers();
	}
#if defined(USE_OPENSSL) && (HA_OPENSSL_VERSION_NUMBER >= 0x10101000L) && !defined(OPENSSL_IS_BORINGSSL)
	/* close random device FDs */
	RAND_keep_random_devices_open(0);
#endif

	/* restore the initial FD limits */
	limit.rlim_cur = rlim_fd_cur_at_boot;
	limit.rlim_max = rlim_fd_max_at_boot;
	if (setrlimit(RLIMIT_NOFILE, &limit) == -1) {
		getrlimit(RLIMIT_NOFILE, &limit);
		ha_warning("Failed to restore initial FD limits (cur=%u max=%u), using cur=%u max=%u\n",
			   rlim_fd_cur_at_boot, rlim_fd_max_at_boot,
			   (unsigned int)limit.rlim_cur, (unsigned int)limit.rlim_max);
	}

	/* compute length  */
	while (old_argv[old_argc])
		old_argc++;

	/* 1 for haproxy -sf, 2 for -x /socket */
	next_argv = calloc(old_argc + 1 + 2 + mworker_child_nb() + nb_oldpids + 1,
			   sizeof(*next_argv));
	if (next_argv == NULL)
		goto alloc_error;

	/* copy the program name */
	next_argv[next_argc++] = old_argv[0];

	/* insert the new options just after argv[0] in case we have a -- */

	/* add -sf <PID>*  to argv */
	if (mworker_child_nb() > 0) {
		struct mworker_proc *child;

		next_argv[next_argc++] = "-sf";

		list_for_each_entry(child, &proc_list, list) {
			if (!(child->options & (PROC_O_TYPE_WORKER|PROC_O_TYPE_PROG)) || child->pid <= -1 )
				continue;
			if ((next_argv[next_argc++] = memprintf(&msg, "%d", child->pid)) == NULL)
				goto alloc_error;
			msg = NULL;
		}
	}
	/* add the -x option with the stat socket */
	if (cur_unixsocket) {
		next_argv[next_argc++] = "-x";
		next_argv[next_argc++] = (char *)cur_unixsocket;
	}

	/* copy the previous options */
	for (i = 1; i < old_argc; i++)
		next_argv[next_argc++] = old_argv[i];

	ha_warning("Reexecuting Master process\n");
	signal(SIGPROF, SIG_IGN);
	execvp(next_argv[0], next_argv);

	ha_warning("Failed to reexecute the master process [%d]: %s\n", pid, strerror(errno));
	free(next_argv);
	next_argv = NULL;
	return;

alloc_error:
	free(next_argv);
	next_argv = NULL;
	ha_warning("Failed to reexecute the master process [%d]: Cannot allocate memory\n", pid);
	return;
}

static void mworker_loop()
{

#if defined(USE_SYSTEMD)
	if (global.tune.options & GTUNE_USE_SYSTEMD)
		sd_notifyf(0, "READY=1\nMAINPID=%lu", (unsigned long)getpid());
#endif
	/* Busy polling makes no sense in the master :-) */
	global.tune.options &= ~GTUNE_BUSY_POLLING;

	master = 1;

	signal_unregister(SIGTTIN);
	signal_unregister(SIGTTOU);
	signal_unregister(SIGUSR1);
	signal_unregister(SIGHUP);
	signal_unregister(SIGQUIT);

	signal_register_fct(SIGTERM, mworker_catch_sigterm, SIGTERM);
	signal_register_fct(SIGUSR1, mworker_catch_sigterm, SIGUSR1);
	signal_register_fct(SIGTTIN, mworker_broadcast_signal, SIGTTIN);
	signal_register_fct(SIGTTOU, mworker_broadcast_signal, SIGTTOU);
	signal_register_fct(SIGINT, mworker_catch_sigterm, SIGINT);
	signal_register_fct(SIGHUP, mworker_catch_sighup, SIGHUP);
	signal_register_fct(SIGUSR2, mworker_catch_sighup, SIGUSR2);
	signal_register_fct(SIGCHLD, mworker_catch_sigchld, SIGCHLD);

	mworker_unblock_signals();
	mworker_cleanlisteners();
	mworker_cleantasks();

	mworker_catch_sigchld(NULL); /* ensure we clean the children in case
				     some SIGCHLD were lost */

	global.nbthread = 1;
	relative_pid = 1;
	pid_bit = 1;
	all_proc_mask = 1;

#ifdef USE_THREAD
	tid_bit = 1;
	all_threads_mask = 1;
#endif

	jobs++; /* this is the "master" job, we want to take care of the
		signals even if there is no listener so the poll loop don't
		leave */

	fork_poller();
	run_thread_poll_loop(0);
}

/*
 * Reexec the process in failure mode, instead of exiting
 */
void reexec_on_failure()
{
	if (!atexit_flag)
		return;

	setenv("HAPROXY_MWORKER_WAIT_ONLY", "1", 1);

	ha_warning("Reexecuting Master process in waitpid mode\n");
	mworker_reload();
}


/*
 * upon SIGUSR1, let's have a soft stop. Note that soft_stop() broadcasts
 * a signal zero to all subscribers. This means that it's as easy as
 * subscribing to signal 0 to get informed about an imminent shutdown.
 */
static void sig_soft_stop(struct sig_handler *sh)
{
	soft_stop();
	signal_unregister_handler(sh);
	pool_gc(NULL);
}

/*
 * upon SIGTTOU, we pause everything
 */
static void sig_pause(struct sig_handler *sh)
{
	if (protocol_pause_all() & ERR_FATAL) {
		const char *msg = "Some proxies refused to pause, performing soft stop now.\n";
		ha_warning("%s", msg);
		send_log(NULL, LOG_WARNING, "%s", msg);
		soft_stop();
	}
	pool_gc(NULL);
}

/*
 * upon SIGTTIN, let's have a soft stop.
 */
static void sig_listen(struct sig_handler *sh)
{
	if (protocol_resume_all() & ERR_FATAL) {
		const char *msg = "Some proxies refused to resume, probably due to a conflict on a listening port. You may want to try again after the conflicting application is stopped, otherwise a restart might be needed to resume safe operations.\n";
		ha_warning("%s", msg);
		send_log(NULL, LOG_WARNING, "%s", msg);
	}
}

/*
 * this function dumps every server's state when the process receives SIGHUP.
 */
static void sig_dump_state(struct sig_handler *sh)
{
	struct proxy *p = proxies_list;

	ha_warning("SIGHUP received, dumping servers states.\n");
	while (p) {
		struct server *s = p->srv;

		send_log(p, LOG_NOTICE, "SIGHUP received, dumping servers states for proxy %s.\n", p->id);
		while (s) {
			chunk_printf(&trash,
			             "SIGHUP: Server %s/%s is %s. Conn: %d act, %d pend, %lld tot.",
			             p->id, s->id,
			             (s->cur_state != SRV_ST_STOPPED) ? "UP" : "DOWN",
			             s->cur_sess, s->nbpend, s->counters.cum_sess);
			ha_warning("%s\n", trash.area);
			send_log(p, LOG_NOTICE, "%s\n", trash.area);
			s = s->next;
		}

		/* FIXME: those info are a bit outdated. We should be able to distinguish between FE and BE. */
		if (!p->srv) {
			chunk_printf(&trash,
			             "SIGHUP: Proxy %s has no servers. Conn: act(FE+BE): %d+%d, %d pend (%d unass), tot(FE+BE): %lld+%lld.",
			             p->id,
			             p->feconn, p->beconn, p->totpend, p->nbpend, p->fe_counters.cum_conn, p->be_counters.cum_conn);
		} else if (p->srv_act == 0) {
			chunk_printf(&trash,
			             "SIGHUP: Proxy %s %s ! Conn: act(FE+BE): %d+%d, %d pend (%d unass), tot(FE+BE): %lld+%lld.",
			             p->id,
			             (p->srv_bck) ? "is running on backup servers" : "has no server available",
			             p->feconn, p->beconn, p->totpend, p->nbpend, p->fe_counters.cum_conn, p->be_counters.cum_conn);
		} else {
			chunk_printf(&trash,
			             "SIGHUP: Proxy %s has %d active servers and %d backup servers available."
			             " Conn: act(FE+BE): %d+%d, %d pend (%d unass), tot(FE+BE): %lld+%lld.",
			             p->id, p->srv_act, p->srv_bck,
			             p->feconn, p->beconn, p->totpend, p->nbpend, p->fe_counters.cum_conn, p->be_counters.cum_conn);
		}
		ha_warning("%s\n", trash.area);
		send_log(p, LOG_NOTICE, "%s\n", trash.area);

		p = p->next;
	}
}

static void dump(struct sig_handler *sh)
{
	/* dump memory usage then free everything possible */
	dump_pools();
	pool_gc(NULL);
}

/*
 *  This function dup2 the stdio FDs (0,1,2) with <fd>, then closes <fd>
 *  If <fd> < 0, it opens /dev/null and use it to dup
 *
 *  In the case of chrooting, you have to open /dev/null before the chroot, and
 *  pass the <fd> to this function
 */
static void stdio_quiet(int fd)
{
	if (fd < 0)
		fd = open("/dev/null", O_RDWR, 0);

	if (fd > -1) {
		fclose(stdin);
		fclose(stdout);
		fclose(stderr);

		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2)
			close(fd);
		return;
	}

	ha_alert("Cannot open /dev/null\n");
	exit(EXIT_FAILURE);
}


/* This function checks if cfg_cfgfiles contains directories.
 * If it finds one, it adds all the files (and only files) it contains
 * in cfg_cfgfiles in place of the directory (and removes the directory).
 * It adds the files in lexical order.
 * It adds only files with .cfg extension.
 * It doesn't add files with name starting with '.'
 */
static void cfgfiles_expand_directories(void)
{
	struct wordlist *wl, *wlb;
	char *err = NULL;

	list_for_each_entry_safe(wl, wlb, &cfg_cfgfiles, list) {
		struct stat file_stat;
		struct dirent **dir_entries = NULL;
		int dir_entries_nb;
		int dir_entries_it;

		if (stat(wl->s, &file_stat)) {
			ha_alert("Cannot open configuration file/directory %s : %s\n",
				 wl->s,
				 strerror(errno));
			exit(1);
		}

		if (!S_ISDIR(file_stat.st_mode))
			continue;

		/* from this point wl->s is a directory */

		dir_entries_nb = scandir(wl->s, &dir_entries, NULL, alphasort);
		if (dir_entries_nb < 0) {
			ha_alert("Cannot open configuration directory %s : %s\n",
				 wl->s,
				 strerror(errno));
			exit(1);
		}

		/* for each element in the directory wl->s */
		for (dir_entries_it = 0; dir_entries_it < dir_entries_nb; dir_entries_it++) {
			struct dirent *dir_entry = dir_entries[dir_entries_it];
			char *filename = NULL;
			char *d_name_cfgext = strstr(dir_entry->d_name, ".cfg");

			/* don't add filename that begin with .
			 * only add filename with .cfg extension
			 */
			if (dir_entry->d_name[0] == '.' ||
			    !(d_name_cfgext && d_name_cfgext[4] == '\0'))
				goto next_dir_entry;

			if (!memprintf(&filename, "%s/%s", wl->s, dir_entry->d_name)) {
				ha_alert("Cannot load configuration files %s : out of memory.\n",
					 filename);
				exit(1);
			}

			if (stat(filename, &file_stat)) {
				ha_alert("Cannot open configuration file %s : %s\n",
					 wl->s,
					 strerror(errno));
				exit(1);
			}

			/* don't add anything else than regular file in cfg_cfgfiles
			 * this way we avoid loops
			 */
			if (!S_ISREG(file_stat.st_mode))
				goto next_dir_entry;

			if (!list_append_word(&wl->list, filename, &err)) {
				ha_alert("Cannot load configuration files %s : %s\n",
					 filename,
					 err);
				exit(1);
			}

next_dir_entry:
			free(filename);
			free(dir_entry);
		}

		free(dir_entries);

		/* remove the current directory (wl) from cfg_cfgfiles */
		free(wl->s);
		LIST_DEL(&wl->list);
		free(wl);
	}

	free(err);
}

/*
 * copy and cleanup the current argv
 * Remove the -sf /-st / -x parameters
 * Return an allocated copy of argv
 */

static char **copy_argv(int argc, char **argv)
{
	char **newargv, **retargv;

	newargv = calloc(argc + 2, sizeof(*newargv));
	if (newargv == NULL) {
		ha_warning("Cannot allocate memory\n");
		return NULL;
	}
	retargv = newargv;

	/* first copy argv[0] */
	*newargv++ = *argv++;
	argc--;

	while (argc > 0) {
		if (**argv != '-') {
			/* non options are copied but will fail in the argument parser */
			*newargv++ = *argv++;
			argc--;

		} else  {
			char *flag;

			flag = *argv + 1;

			if (flag[0] == '-' && flag[1] == 0) {
				/* "--\0" copy every arguments till the end of argv */
				*newargv++ = *argv++;
				argc--;

				while (argc > 0) {
					*newargv++ = *argv++;
					argc--;
				}
			} else {
				switch (*flag) {
					case 's':
						/* -sf / -st and their parameters are ignored */
						if (flag[1] == 'f' || flag[1] == 't') {
							argc--;
							argv++;
							/* The list can't contain a negative value since the only
							way to know the end of this list is by looking for the
							next option or the end of the options */
							while (argc > 0 && argv[0][0] != '-') {
								argc--;
								argv++;
							}
						} else {
							argc--;
							argv++;

						}
						break;

					case 'x':
						/* this option and its parameter are ignored */
						argc--;
						argv++;
						if (argc > 0) {
							argc--;
							argv++;
						}
						break;

					case 'C':
					case 'n':
					case 'm':
					case 'N':
					case 'L':
					case 'f':
					case 'p':
					case 'S':
						/* these options have only 1 parameter which must be copied and can start with a '-' */
						*newargv++ = *argv++;
						argc--;
						if (argc == 0)
							goto error;
						*newargv++ = *argv++;
						argc--;
						break;
					default:
						/* for other options just copy them without parameters, this is also done
						 * for options like "--foo", but this  will fail in the argument parser.
						 * */
						*newargv++ = *argv++;
						argc--;
						break;
				}
			}
		}
	}

	return retargv;

error:
	free(retargv);
	return NULL;
}


/* Performs basic random seed initialization. The main issue with this is that
 * srandom_r() only takes 32 bits and purposely provides a reproducible sequence,
 * which means that there will only be 4 billion possible random sequences once
 * srandom() is called, regardless of the internal state. Not calling it is
 * even worse as we'll always produce the same randoms sequences. What we do
 * here is to create an initial sequence from various entropy sources, hash it
 * using SHA1 and keep the resulting 160 bits available globally.
 *
 * We initialize the current process with the first 32 bits before starting the
 * polling loop, where all this will be changed to have process specific and
 * thread specific sequences.
 *
 * Before starting threads, it's still possible to call random() as srandom()
 * is initialized from this, but after threads and/or processes are started,
 * only ha_random() is expected to be used to guarantee distinct sequences.
 */
static void ha_random_boot(char *const *argv)
{
	unsigned char message[256];
	unsigned char *m = message;
	struct timeval tv;
	blk_SHA_CTX ctx;
	unsigned long l;
	int fd;
	int i;

	/* start with current time as pseudo-random seed */
	gettimeofday(&tv, NULL);
	write_u32(m, tv.tv_sec);  m += 4;
	write_u32(m, tv.tv_usec); m += 4;

	/* PID and PPID add some OS-based randomness */
	write_u16(m, getpid());   m += 2;
	write_u16(m, getppid());  m += 2;

	/* take up to 160 bits bytes from /dev/urandom if available (non-blocking) */
	fd = open("/dev/urandom", O_RDONLY);
	if (fd >= 0) {
		i = read(fd, m, 20);
		if (i > 0)
			m += i;
		close(fd);
	}

	/* take up to 160 bits bytes from openssl (non-blocking) */
#ifdef USE_OPENSSL
	if (RAND_bytes(m, 20) == 1)
		m += 20;
#endif

	/* take 160 bits from existing random in case it was already initialized */
	for (i = 0; i < 5; i++) {
		write_u32(m, random());
		m += 4;
	}

	/* stack address (benefit form operating system's ASLR) */
	l = (unsigned long)&m;
	memcpy(m, &l, sizeof(l)); m += sizeof(l);

	/* argv address (benefit form operating system's ASLR) */
	l = (unsigned long)&argv;
	memcpy(m, &l, sizeof(l)); m += sizeof(l);

	/* use tv_usec again after all the operations above */
	gettimeofday(&tv, NULL);
	write_u32(m, tv.tv_usec); m += 4;

	/*
	 * At this point, ~84-92 bytes have been used
	 */

	/* finish with the hostname */
	strncpy((char *)m, hostname, message + sizeof(message) - m);
	m += strlen(hostname);

	/* total message length */
	l = m - message;

	memset(&ctx, 0, sizeof(ctx));
	blk_SHA1_Init(&ctx);
	blk_SHA1_Update(&ctx, message, l);
	blk_SHA1_Final(boot_seed, &ctx);

	srandom(read_u32(boot_seed));
	ha_random_seed(boot_seed, sizeof(boot_seed));
}

/* considers splicing proxies' maxconn, computes the ideal global.maxpipes
 * setting, and returns it. It may return -1 meaning "unlimited" if some
 * unlimited proxies have been found and the global.maxconn value is not yet
 * set. It may also return a value greater than maxconn if it's not yet set.
 * Note that a value of zero means there is no need for pipes. -1 is never
 * returned if global.maxconn is valid.
 */
static int compute_ideal_maxpipes()
{
	struct proxy *cur;
	int nbfe = 0, nbbe = 0;
	int unlimited = 0;
	int pipes;
	int max;

	for (cur = proxies_list; cur; cur = cur->next) {
		if (cur->options2 & (PR_O2_SPLIC_ANY)) {
			if (cur->cap & PR_CAP_FE) {
				max = cur->maxconn;
				nbfe += max;
				if (!max) {
					unlimited = 1;
					break;
				}
			}
			if (cur->cap & PR_CAP_BE) {
				max = cur->fullconn ? cur->fullconn : global.maxconn;
				nbbe += max;
				if (!max) {
					unlimited = 1;
					break;
				}
			}
		}
	}

	pipes = MAX(nbfe, nbbe);
	if (global.maxconn) {
		if (pipes > global.maxconn || unlimited)
			pipes = global.maxconn;
	} else if (unlimited) {
		pipes = -1;
	}

	return pipes >= 4 ? pipes / 4 : pipes;
}

/* considers global.maxsocks, global.maxpipes, async engines, SSL frontends and
 * rlimits and computes an ideal maxconn. It's meant to be called only when
 * maxsock contains the sum of listening FDs, before it is updated based on
 * maxconn and pipes. If there are not enough FDs left, DEFAULT_MAXCONN (by
 * default 100) is returned as it is expected that it will even run on tight
 * environments, and will maintain compatibility with previous packages that
 * used to rely on this value as the default one. The system will emit a
 * warning indicating how many FDs are missing anyway if needed.
 */
static int compute_ideal_maxconn()
{
	int ssl_sides = !!global.ssl_used_frontend + !!global.ssl_used_backend;
	int engine_fds = global.ssl_used_async_engines * ssl_sides;
	int pipes = compute_ideal_maxpipes();
	int remain = MAX(rlim_fd_cur_at_boot, rlim_fd_max_at_boot);
	int maxconn;

	/* we have to take into account these elements :
	 *   - number of engine_fds, which inflates the number of FD needed per
	 *     connection by this number.
	 *   - number of pipes per connection on average : for the unlimited
	 *     case, this is 0.5 pipe FDs per connection, otherwise it's a
	 *     fixed value of 2*pipes.
	 *   - two FDs per connection
	 */

	/* subtract listeners and checks */
	remain -= global.maxsock;

	/* one epoll_fd/kqueue_fd per thread */
	remain -= global.nbthread;

	/* one wake-up pipe (2 fd) per thread */
	remain -= 2 * global.nbthread;

	/* Fixed pipes values : we only subtract them if they're not larger
	 * than the remaining FDs because pipes are optional.
	 */
	if (pipes >= 0 && pipes * 2 < remain)
		remain -= pipes * 2;

	if (pipes < 0) {
		/* maxsock = maxconn * 2 + maxconn/4 * 2 + maxconn * engine_fds.
		 *         = maxconn * (2 + 0.5 + engine_fds)
		 *         = maxconn * (4 + 1 + 2*engine_fds) / 2
		 */
		maxconn = 2 * remain / (5 + 2 * engine_fds);
	} else {
		/* maxsock = maxconn * 2 + maxconn * engine_fds.
		 *         = maxconn * (2 + engine_fds)
		 */
		maxconn = remain / (2 + engine_fds);
	}

	return MAX(maxconn, DEFAULT_MAXCONN);
}

/* computes the estimated maxsock value for the given maxconn based on the
 * possibly set global.maxpipes and existing partial global.maxsock. It may
 * temporarily change global.maxconn for the time needed to propagate the
 * computations, and will reset it.
 */
static int compute_ideal_maxsock(int maxconn)
{
	int maxpipes = global.maxpipes;
	int maxsock  = global.maxsock;


	if (!maxpipes) {
		int old_maxconn = global.maxconn;

		global.maxconn = maxconn;
		maxpipes = compute_ideal_maxpipes();
		global.maxconn = old_maxconn;
	}

	maxsock += maxconn * 2;         /* each connection needs two sockets */
	maxsock += maxpipes * 2;        /* each pipe needs two FDs */
	maxsock += global.nbthread;     /* one epoll_fd/kqueue_fd per thread */
	maxsock += 2 * global.nbthread; /* one wake-up pipe (2 fd) per thread */

	/* compute fd used by async engines */
	if (global.ssl_used_async_engines) {
		int sides = !!global.ssl_used_frontend + !!global.ssl_used_backend;

		maxsock += maxconn * sides * global.ssl_used_async_engines;
	}
	return maxsock;
}

/* Tests if it is possible to set the current process's RLIMIT_NOFILE to
 * <maxsock>, then sets it back to the previous value. Returns non-zero if the
 * value is accepted, non-zero otherwise. This is used to determine if an
 * automatic limit may be applied or not. When it is not, the caller knows that
 * the highest we can do is the rlim_max at boot. In case of error, we return
 * that the setting is possible, so that we defer the error processing to the
 * final stage in charge of enforcing this.
 */
static int check_if_maxsock_permitted(int maxsock)
{
	struct rlimit orig_limit, test_limit;
	int ret;

	if (getrlimit(RLIMIT_NOFILE, &orig_limit) != 0)
		return 1;

	/* don't go further if we can't even set to what we have */
	if (setrlimit(RLIMIT_NOFILE, &orig_limit) != 0)
		return 1;

	test_limit.rlim_max = MAX(maxsock, orig_limit.rlim_max);
	test_limit.rlim_cur = test_limit.rlim_max;
	ret = setrlimit(RLIMIT_NOFILE, &test_limit);

	if (setrlimit(RLIMIT_NOFILE, &orig_limit) != 0)
		return 1;

	return ret == 0;
}


/*
 * This function initializes all the necessary variables. It only returns
 * if everything is OK. If something fails, it exits.
 */
static void init(int argc, char **argv)
{
	int arg_mode = 0;	/* MODE_DEBUG, ... */
	char *tmp;
	char *cfg_pidfile = NULL;
	int err_code = 0;
	char *err_msg = NULL;
	struct wordlist *wl;
	char *progname;
	char *change_dir = NULL;
	struct proxy *px;
	struct post_check_fct *pcf;
	int ideal_maxconn;

	global.mode = MODE_STARTING;
	old_argv = copy_argv(argc, argv);
	if (!old_argv) {
		ha_alert("failed to copy argv.\n");
		exit(1);
	}

	if (!init_trash_buffers(1)) {
		ha_alert("failed to initialize trash buffers.\n");
		exit(1);
	}

	/* NB: POSIX does not make it mandatory for gethostname() to NULL-terminate
	 * the string in case of truncation, and at least FreeBSD appears not to do
	 * it.
	 */
	memset(hostname, 0, sizeof(hostname));
	gethostname(hostname, sizeof(hostname) - 1);

	if ((localpeer = strdup(hostname)) == NULL) {
		ha_alert("Cannot allocate memory for local peer.\n");
		exit(EXIT_FAILURE);
	}
	setenv("HAPROXY_LOCALPEER", localpeer, 1);

	/* we were in mworker mode, we should restart in mworker mode */
	if (getenv("HAPROXY_MWORKER_REEXEC") != NULL)
		global.mode |= MODE_MWORKER;

	/*
	 * Initialize the previously static variables.
	 */

	totalconn = actconn = listeners = stopping = 0;
	killed = 0;


#ifdef HAPROXY_MEMMAX
	global.rlimit_memmax_all = HAPROXY_MEMMAX;
#endif

	tzset();
	tv_update_date(-1,-1);
	start_date = now;

	ha_random_boot(argv);

	if (init_acl() != 0)
		exit(1);

	/* Initialise lua. */
	hlua_init();

	/* Initialize process vars */
	vars_init(&global.vars, SCOPE_PROC);

	global.tune.options |= GTUNE_USE_SELECT;  /* select() is always available */
#if defined(USE_POLL)
	global.tune.options |= GTUNE_USE_POLL;
#endif
#if defined(USE_EPOLL)
	global.tune.options |= GTUNE_USE_EPOLL;
#endif
#if defined(USE_KQUEUE)
	global.tune.options |= GTUNE_USE_KQUEUE;
#endif
#if defined(USE_EVPORTS)
	global.tune.options |= GTUNE_USE_EVPORTS;
#endif
#if defined(USE_LINUX_SPLICE)
	global.tune.options |= GTUNE_USE_SPLICE;
#endif
#if defined(USE_GETADDRINFO)
	global.tune.options |= GTUNE_USE_GAI;
#endif
#if defined(SO_REUSEPORT)
	global.tune.options |= GTUNE_USE_REUSEPORT;
#endif
#ifdef USE_THREAD
	global.tune.options |= GTUNE_IDLE_POOL_SHARED;
#endif
	global.tune.options |= GTUNE_STRICT_LIMITS;

	pid = getpid();
	progname = *argv;
	while ((tmp = strchr(progname, '/')) != NULL)
		progname = tmp + 1;

	/* the process name is used for the logs only */
	chunk_initlen(&global.log_tag, strdup(progname), strlen(progname), strlen(progname));
	if (b_orig(&global.log_tag) == NULL) {
		chunk_destroy(&global.log_tag);
		ha_alert("Cannot allocate memory for log_tag.\n");
		exit(EXIT_FAILURE);
	}

	argc--; argv++;
	while (argc > 0) {
		char *flag;

		if (**argv == '-') {
			flag = *argv+1;

			/* 1 arg */
			if (*flag == 'v') {
				display_version();
				if (flag[1] == 'v')  /* -vv */
					display_build_opts();
				exit(0);
			}
#if defined(USE_EPOLL)
			else if (*flag == 'd' && flag[1] == 'e')
				global.tune.options &= ~GTUNE_USE_EPOLL;
#endif
#if defined(USE_POLL)
			else if (*flag == 'd' && flag[1] == 'p')
				global.tune.options &= ~GTUNE_USE_POLL;
#endif
#if defined(USE_KQUEUE)
			else if (*flag == 'd' && flag[1] == 'k')
				global.tune.options &= ~GTUNE_USE_KQUEUE;
#endif
#if defined(USE_EVPORTS)
			else if (*flag == 'd' && flag[1] == 'v')
				global.tune.options &= ~GTUNE_USE_EVPORTS;
#endif
#if defined(USE_LINUX_SPLICE)
			else if (*flag == 'd' && flag[1] == 'S')
				global.tune.options &= ~GTUNE_USE_SPLICE;
#endif
#if defined(USE_GETADDRINFO)
			else if (*flag == 'd' && flag[1] == 'G')
				global.tune.options &= ~GTUNE_USE_GAI;
#endif
#if defined(SO_REUSEPORT)
			else if (*flag == 'd' && flag[1] == 'R')
				global.tune.options &= ~GTUNE_USE_REUSEPORT;
#endif
			else if (*flag == 'd' && flag[1] == 'V')
				global.ssl_server_verify = SSL_SERVER_VERIFY_NONE;
			else if (*flag == 'V')
				arg_mode |= MODE_VERBOSE;
			else if (*flag == 'd' && flag[1] == 'b')
				arg_mode |= MODE_FOREGROUND;
			else if (*flag == 'd' && flag[1] == 'W')
				arg_mode |= MODE_ZERO_WARNING;
			else if (*flag == 'd' && flag[1] == 'M')
				mem_poison_byte = flag[2] ? strtol(flag + 2, NULL, 0) : 'P';
			else if (*flag == 'd' && flag[1] == 'r')
				global.tune.options |= GTUNE_RESOLVE_DONTFAIL;
			else if (*flag == 'd')
				arg_mode |= MODE_DEBUG;
			else if (*flag == 'c')
				arg_mode |= MODE_CHECK;
			else if (*flag == 'D')
				arg_mode |= MODE_DAEMON;
			else if (*flag == 'W' && flag[1] == 's') {
				arg_mode |= MODE_MWORKER | MODE_FOREGROUND;
#if defined(USE_SYSTEMD)
				global.tune.options |= GTUNE_USE_SYSTEMD;
#else
				ha_alert("master-worker mode with systemd support (-Ws) requested, but not compiled. Use master-worker mode (-W) if you are not using Type=notify in your unit file or recompile with USE_SYSTEMD=1.\n\n");
				usage(progname);
#endif
			}
			else if (*flag == 'W')
				arg_mode |= MODE_MWORKER;
			else if (*flag == 'q')
				arg_mode |= MODE_QUIET;
			else if (*flag == 'x') {
				if (argc <= 1) {
					ha_alert("Unix socket path expected with the -x flag\n\n");
					usage(progname);
				}
				if (old_unixsocket)
					ha_warning("-x option already set, overwriting the value\n");
				old_unixsocket = argv[1];

				argv++;
				argc--;
			}
			else if (*flag == 'S') {
				struct wordlist *c;

				if (argc <= 1) {
					ha_alert("Socket and optional bind parameters expected with the -S flag\n");
					usage(progname);
				}
				if ((c = malloc(sizeof(*c))) == NULL || (c->s = strdup(argv[1])) == NULL) {
					ha_alert("Cannot allocate memory\n");
					exit(EXIT_FAILURE);
				}
				LIST_ADD(&mworker_cli_conf, &c->list);

				argv++;
				argc--;
			}
			else if (*flag == 's' && (flag[1] == 'f' || flag[1] == 't')) {
				/* list of pids to finish ('f') or terminate ('t') */

				if (flag[1] == 'f')
					oldpids_sig = SIGUSR1; /* finish then exit */
				else
					oldpids_sig = SIGTERM; /* terminate immediately */
				while (argc > 1 && argv[1][0] != '-') {
					char * endptr = NULL;
					oldpids = realloc(oldpids, (nb_oldpids + 1) * sizeof(int));
					if (!oldpids) {
						ha_alert("Cannot allocate old pid : out of memory.\n");
						exit(1);
					}
					argc--; argv++;
					errno = 0;
					oldpids[nb_oldpids] = strtol(*argv, &endptr, 10);
					if (errno) {
						ha_alert("-%2s option: failed to parse {%s}: %s\n",
							 flag,
							 *argv, strerror(errno));
						exit(1);
					} else if (endptr && strlen(endptr)) {
						while (isspace((unsigned char)*endptr)) endptr++;
						if (*endptr != 0) {
							ha_alert("-%2s option: some bytes unconsumed in PID list {%s}\n",
								 flag, endptr);
							exit(1);
						}
					}
					if (oldpids[nb_oldpids] <= 0)
						usage(progname);
					nb_oldpids++;
				}
			}
			else if (flag[0] == '-' && flag[1] == 0) { /* "--" */
				/* now that's a cfgfile list */
				argv++; argc--;
				while (argc > 0) {
					if (!list_append_word(&cfg_cfgfiles, *argv, &err_msg)) {
						ha_alert("Cannot load configuration file/directory %s : %s\n",
							 *argv,
							 err_msg);
						exit(1);
					}
					argv++; argc--;
				}
				break;
			}
			else { /* >=2 args */
				argv++; argc--;
				if (argc == 0)
					usage(progname);

				switch (*flag) {
				case 'C' : change_dir = *argv; break;
				case 'n' : cfg_maxconn = atol(*argv); break;
				case 'm' : global.rlimit_memmax_all = atol(*argv); break;
				case 'N' : cfg_maxpconn = atol(*argv); break;
				case 'L' :
					free(localpeer);
					if ((localpeer = strdup(*argv)) == NULL) {
						ha_alert("Cannot allocate memory for local peer.\n");
						exit(EXIT_FAILURE);
					}
					setenv("HAPROXY_LOCALPEER", localpeer, 1);
					global.localpeer_cmdline = 1;
					break;
				case 'f' :
					if (!list_append_word(&cfg_cfgfiles, *argv, &err_msg)) {
						ha_alert("Cannot load configuration file/directory %s : %s\n",
							 *argv,
							 err_msg);
						exit(1);
					}
					break;
				case 'p' : cfg_pidfile = *argv; break;
				default: usage(progname);
				}
			}
		}
		else
			usage(progname);
		argv++; argc--;
	}

	global.mode |= (arg_mode & (MODE_DAEMON | MODE_MWORKER | MODE_FOREGROUND | MODE_VERBOSE
				    | MODE_QUIET | MODE_CHECK | MODE_DEBUG | MODE_ZERO_WARNING));

	if (getenv("HAPROXY_MWORKER_WAIT_ONLY")) {
		unsetenv("HAPROXY_MWORKER_WAIT_ONLY");
		global.mode |= MODE_MWORKER_WAIT;
		global.mode &= ~MODE_MWORKER;
	}

	if ((global.mode & MODE_MWORKER) && (getenv("HAPROXY_MWORKER_REEXEC") != NULL)) {
		atexit_flag = 1;
		atexit(reexec_on_failure);
	}

	if (change_dir && chdir(change_dir) < 0) {
		ha_alert("Could not change to directory %s : %s\n", change_dir, strerror(errno));
		exit(1);
	}

	global.maxsock = 10; /* reserve 10 fds ; will be incremented by socket eaters */

	init_default_instance();

	/* in wait mode, we don't try to read the configuration files */
	if (!(global.mode & MODE_MWORKER_WAIT)) {
		struct buffer *trash = get_trash_chunk();

		/* handle cfgfiles that are actually directories */
		cfgfiles_expand_directories();

		if (LIST_ISEMPTY(&cfg_cfgfiles))
			usage(progname);


		list_for_each_entry(wl, &cfg_cfgfiles, list) {
			int ret;

			if (trash->data)
				chunk_appendf(trash, ";");

			chunk_appendf(trash, "%s", wl->s);

			ret = readcfgfile(wl->s);
			if (ret == -1) {
				ha_alert("Could not open configuration file %s : %s\n",
					 wl->s, strerror(errno));
				exit(1);
			}
			if (ret & (ERR_ABORT|ERR_FATAL))
				ha_alert("Error(s) found in configuration file : %s\n", wl->s);
			err_code |= ret;
			if (err_code & ERR_ABORT)
				exit(1);
		}

		/* do not try to resolve arguments nor to spot inconsistencies when
		 * the configuration contains fatal errors caused by files not found
		 * or failed memory allocations.
		 */
		if (err_code & (ERR_ABORT|ERR_FATAL)) {
			ha_alert("Fatal errors found in configuration.\n");
			exit(1);
		}
		if (trash->data)
			setenv("HAPROXY_CFGFILES", trash->area, 1);

	}
	if (global.mode & MODE_MWORKER) {
		int proc;
		struct mworker_proc *tmproc;

		setenv("HAPROXY_MWORKER", "1", 1);

		if (getenv("HAPROXY_MWORKER_REEXEC") == NULL) {

			tmproc = calloc(1, sizeof(*tmproc));
			if (!tmproc) {
				ha_alert("Cannot allocate process structures.\n");
				exit(EXIT_FAILURE);
			}
			tmproc->options |= PROC_O_TYPE_MASTER; /* master */
			tmproc->reloads = 0;
			tmproc->relative_pid = 0;
			tmproc->pid = pid;
			tmproc->timestamp = start_date.tv_sec;
			tmproc->ipc_fd[0] = -1;
			tmproc->ipc_fd[1] = -1;

			proc_self = tmproc;

			LIST_ADDQ(&proc_list, &tmproc->list);
		}

		for (proc = 0; proc < global.nbproc; proc++) {

			tmproc = calloc(1, sizeof(*tmproc));
			if (!tmproc) {
				ha_alert("Cannot allocate process structures.\n");
				exit(EXIT_FAILURE);
			}

			tmproc->options |= PROC_O_TYPE_WORKER; /* worker */
			tmproc->pid = -1;
			tmproc->reloads = 0;
			tmproc->timestamp = -1;
			tmproc->relative_pid = 1 + proc;
			tmproc->ipc_fd[0] = -1;
			tmproc->ipc_fd[1] = -1;

			if (mworker_cli_sockpair_new(tmproc, proc) < 0) {
				exit(EXIT_FAILURE);
			}

			LIST_ADDQ(&proc_list, &tmproc->list);
		}
	}
	if (global.mode & (MODE_MWORKER|MODE_MWORKER_WAIT)) {
		struct wordlist *it, *c;

		mworker_env_to_proc_list(); /* get the info of the children in the env */


		if (!LIST_ISEMPTY(&mworker_cli_conf)) {

			if (mworker_cli_proxy_create() < 0) {
				ha_alert("Can't create the master's CLI.\n");
				exit(EXIT_FAILURE);
			}

			list_for_each_entry_safe(c, it, &mworker_cli_conf, list) {

				if (mworker_cli_proxy_new_listener(c->s) < 0) {
					ha_alert("Can't create the master's CLI.\n");
					exit(EXIT_FAILURE);
				}
				LIST_DEL(&c->list);
				free(c->s);
				free(c);
			}
		}
	}

	if (global.nbproc > 1 && !global.nbthread) {
		ha_warning("nbproc is deprecated!\n"
			   "  | For suffering many limitations, the 'nbproc' directive is now deprecated\n"
			   "  | and scheduled for removal in 2.5. Just comment it out: haproxy will use\n"
			   "  | threads and will run on all allocated processors. You may also switch to\n"
			   "  | 'nbthread %d' to keep the same number of processors. If you absolutely\n"
			   "  | want to run in multi-process mode, you can silence this warning by adding\n"
			   "  | 'nbthread 1', but then please report your use case to developers.\n",
		           global.nbproc);
	}

	err_code |= check_config_validity();
	for (px = proxies_list; px; px = px->next) {
		struct server *srv;
		struct post_proxy_check_fct *ppcf;
		struct post_server_check_fct *pscf;

		if (px->disabled)
			continue;

		list_for_each_entry(pscf, &post_server_check_list, list) {
			for (srv = px->srv; srv; srv = srv->next)
				err_code |= pscf->fct(srv);
		}
		list_for_each_entry(ppcf, &post_proxy_check_list, list)
			err_code |= ppcf->fct(px);
	}
	if (err_code & (ERR_ABORT|ERR_FATAL)) {
		ha_alert("Fatal errors found in configuration.\n");
		exit(1);
	}

	err_code |= pattern_finalize_config();
	if (err_code & (ERR_ABORT|ERR_FATAL)) {
		ha_alert("Failed to finalize pattern config.\n");
		exit(1);
	}

	/* recompute the amount of per-process memory depending on nbproc and
	 * the shared SSL cache size (allowed to exist in all processes).
	 */
	if (global.rlimit_memmax_all) {
#if defined (USE_OPENSSL) && !defined(USE_PRIVATE_CACHE)
		int64_t ssl_cache_bytes = global.tune.sslcachesize * 200LL;

		global.rlimit_memmax =
			((((int64_t)global.rlimit_memmax_all * 1048576LL) -
			  ssl_cache_bytes) / global.nbproc +
			 ssl_cache_bytes + 1048575LL) / 1048576LL;
#else
		global.rlimit_memmax = global.rlimit_memmax_all / global.nbproc;
#endif
	}

#ifdef USE_NS
        err_code |= netns_init();
        if (err_code & (ERR_ABORT|ERR_FATAL)) {
                ha_alert("Failed to initialize namespace support.\n");
                exit(1);
        }
#endif

	/* Apply server states */
	apply_server_state();

	for (px = proxies_list; px; px = px->next)
		srv_compute_all_admin_states(px);

	/* Apply servers' configured address */
	err_code |= srv_init_addr();
	if (err_code & (ERR_ABORT|ERR_FATAL)) {
		ha_alert("Failed to initialize server(s) addr.\n");
		exit(1);
	}

	if (warned & WARN_ANY && global.mode & MODE_ZERO_WARNING) {
		ha_alert("Some warnings were found and 'zero-warning' is set. Aborting.\n");
		exit(1);
	}

	if (global.mode & MODE_CHECK) {
		struct peers *pr;
		struct proxy *px;

		if (warned & WARN_ANY)
			qfprintf(stdout, "Warnings were found.\n");

		for (pr = cfg_peers; pr; pr = pr->next)
			if (pr->peers_fe)
				break;

		for (px = proxies_list; px; px = px->next)
			if (!px->disabled && px->li_all)
				break;

		if (pr || px) {
			/* At least one peer or one listener has been found */
			qfprintf(stdout, "Configuration file is valid\n");
			deinit_and_exit(0);
		}
		qfprintf(stdout, "Configuration file has no error but will not start (no listener) => exit(2).\n");
		exit(2);
	}

	/* now we know the buffer size, we can initialize the channels and buffers */
	init_buffer();

	list_for_each_entry(pcf, &post_check_list, list) {
		err_code |= pcf->fct();
		if (err_code & (ERR_ABORT|ERR_FATAL))
			exit(1);
	}

	if (cfg_maxconn > 0)
		global.maxconn = cfg_maxconn;

	if (global.stats_fe)
		global.maxsock += global.stats_fe->maxconn;

	if (cfg_peers) {
		/* peers also need to bypass global maxconn */
		struct peers *p = cfg_peers;

		for (p = cfg_peers; p; p = p->next)
			if (p->peers_fe)
				global.maxsock += p->peers_fe->maxconn;
	}

	if (cfg_pidfile) {
		free(global.pidfile);
		global.pidfile = strdup(cfg_pidfile);
	}

	/* Now we want to compute the maxconn and possibly maxsslconn values.
	 * It's a bit tricky. Maxconn defaults to the pre-computed value based
	 * on rlim_fd_cur and the number of FDs in use due to the configuration,
	 * and maxsslconn defaults to DEFAULT_MAXSSLCONN. On top of that we can
	 * enforce a lower limit based on memmax.
	 *
	 * If memmax is set, then it depends on which values are set. If
	 * maxsslconn is set, we use memmax to determine how many cleartext
	 * connections may be added, and set maxconn to the sum of the two.
	 * If maxconn is set and not maxsslconn, maxsslconn is computed from
	 * the remaining amount of memory between memmax and the cleartext
	 * connections. If neither are set, then it is considered that all
	 * connections are SSL-capable, and maxconn is computed based on this,
	 * then maxsslconn accordingly. We need to know if SSL is used on the
	 * frontends, backends, or both, because when it's used on both sides,
	 * we need twice the value for maxsslconn, but we only count the
	 * handshake once since it is not performed on the two sides at the
	 * same time (frontend-side is terminated before backend-side begins).
	 * The SSL stack is supposed to have filled ssl_session_cost and
	 * ssl_handshake_cost during its initialization. In any case, if
	 * SYSTEM_MAXCONN is set, we still enforce it as an upper limit for
	 * maxconn in order to protect the system.
	 */
	ideal_maxconn = compute_ideal_maxconn();

	if (!global.rlimit_memmax) {
		if (global.maxconn == 0) {
			global.maxconn = ideal_maxconn;
			if (global.mode & (MODE_VERBOSE|MODE_DEBUG))
				fprintf(stderr, "Note: setting global.maxconn to %d.\n", global.maxconn);
		}
	}
#ifdef USE_OPENSSL
	else if (!global.maxconn && !global.maxsslconn &&
		 (global.ssl_used_frontend || global.ssl_used_backend)) {
		/* memmax is set, compute everything automatically. Here we want
		 * to ensure that all SSL connections will be served. We take
		 * care of the number of sides where SSL is used, and consider
		 * the worst case : SSL used on both sides and doing a handshake
		 * simultaneously. Note that we can't have more than maxconn
		 * handshakes at a time by definition, so for the worst case of
		 * two SSL conns per connection, we count a single handshake.
		 */
		int sides = !!global.ssl_used_frontend + !!global.ssl_used_backend;
		int64_t mem = global.rlimit_memmax * 1048576ULL;
		int retried = 0;

		mem -= global.tune.sslcachesize * 200; // about 200 bytes per SSL cache entry
		mem -= global.maxzlibmem;
		mem = mem * MEM_USABLE_RATIO;

		/* Principle: we test once to set maxconn according to the free
		 * memory. If it results in values the system rejects, we try a
		 * second time by respecting rlim_fd_max. If it fails again, we
		 * go back to the initial value and will let the final code
		 * dealing with rlimit report the error. That's up to 3 attempts.
		 */
		do {
			global.maxconn = mem /
				((STREAM_MAX_COST + 2 * global.tune.bufsize) +    // stream + 2 buffers per stream
				 sides * global.ssl_session_max_cost +            // SSL buffers, one per side
				 global.ssl_handshake_max_cost);                  // 1 handshake per connection max

			if (retried == 1)
				global.maxconn = MIN(global.maxconn, ideal_maxconn);
			global.maxconn = round_2dig(global.maxconn);
#ifdef SYSTEM_MAXCONN
			if (global.maxconn > SYSTEM_MAXCONN)
				global.maxconn = SYSTEM_MAXCONN;
#endif /* SYSTEM_MAXCONN */
			global.maxsslconn = sides * global.maxconn;

			if (check_if_maxsock_permitted(compute_ideal_maxsock(global.maxconn)))
				break;
		} while (retried++ < 2);

		if (global.mode & (MODE_VERBOSE|MODE_DEBUG))
			fprintf(stderr, "Note: setting global.maxconn to %d and global.maxsslconn to %d.\n",
			        global.maxconn, global.maxsslconn);
	}
	else if (!global.maxsslconn &&
		 (global.ssl_used_frontend || global.ssl_used_backend)) {
		/* memmax and maxconn are known, compute maxsslconn automatically.
		 * maxsslconn being forced, we don't know how many of it will be
		 * on each side if both sides are being used. The worst case is
		 * when all connections use only one SSL instance because
		 * handshakes may be on two sides at the same time.
		 */
		int sides = !!global.ssl_used_frontend + !!global.ssl_used_backend;
		int64_t mem = global.rlimit_memmax * 1048576ULL;
		int64_t sslmem;

		mem -= global.tune.sslcachesize * 200; // about 200 bytes per SSL cache entry
		mem -= global.maxzlibmem;
		mem = mem * MEM_USABLE_RATIO;

		sslmem = mem - global.maxconn * (int64_t)(STREAM_MAX_COST + 2 * global.tune.bufsize);
		global.maxsslconn = sslmem / (global.ssl_session_max_cost + global.ssl_handshake_max_cost);
		global.maxsslconn = round_2dig(global.maxsslconn);

		if (sslmem <= 0 || global.maxsslconn < sides) {
			ha_alert("Cannot compute the automatic maxsslconn because global.maxconn is already too "
				 "high for the global.memmax value (%d MB). The absolute maximum possible value "
				 "without SSL is %d, but %d was found and SSL is in use.\n",
				 global.rlimit_memmax,
				 (int)(mem / (STREAM_MAX_COST + 2 * global.tune.bufsize)),
				 global.maxconn);
			exit(1);
		}

		if (global.maxsslconn > sides * global.maxconn)
			global.maxsslconn = sides * global.maxconn;

		if (global.mode & (MODE_VERBOSE|MODE_DEBUG))
			fprintf(stderr, "Note: setting global.maxsslconn to %d\n", global.maxsslconn);
	}
#endif
	else if (!global.maxconn) {
		/* memmax and maxsslconn are known/unused, compute maxconn automatically */
		int sides = !!global.ssl_used_frontend + !!global.ssl_used_backend;
		int64_t mem = global.rlimit_memmax * 1048576ULL;
		int64_t clearmem;
		int retried = 0;

		if (global.ssl_used_frontend || global.ssl_used_backend)
			mem -= global.tune.sslcachesize * 200; // about 200 bytes per SSL cache entry

		mem -= global.maxzlibmem;
		mem = mem * MEM_USABLE_RATIO;

		clearmem = mem;
		if (sides)
			clearmem -= (global.ssl_session_max_cost + global.ssl_handshake_max_cost) * (int64_t)global.maxsslconn;

		/* Principle: we test once to set maxconn according to the free
		 * memory. If it results in values the system rejects, we try a
		 * second time by respecting rlim_fd_max. If it fails again, we
		 * go back to the initial value and will let the final code
		 * dealing with rlimit report the error. That's up to 3 attempts.
		 */
		do {
			global.maxconn = clearmem / (STREAM_MAX_COST + 2 * global.tune.bufsize);
			if (retried == 1)
				global.maxconn = MIN(global.maxconn, ideal_maxconn);
			global.maxconn = round_2dig(global.maxconn);
#ifdef SYSTEM_MAXCONN
			if (global.maxconn > SYSTEM_MAXCONN)
				global.maxconn = SYSTEM_MAXCONN;
#endif /* SYSTEM_MAXCONN */

			if (clearmem <= 0 || !global.maxconn) {
				ha_alert("Cannot compute the automatic maxconn because global.maxsslconn is already too "
					 "high for the global.memmax value (%d MB). The absolute maximum possible value "
					 "is %d, but %d was found.\n",
					 global.rlimit_memmax,
				 (int)(mem / (global.ssl_session_max_cost + global.ssl_handshake_max_cost)),
					 global.maxsslconn);
				exit(1);
			}

			if (check_if_maxsock_permitted(compute_ideal_maxsock(global.maxconn)))
				break;
		} while (retried++ < 2);

		if (global.mode & (MODE_VERBOSE|MODE_DEBUG)) {
			if (sides && global.maxsslconn > sides * global.maxconn) {
				fprintf(stderr, "Note: global.maxsslconn is forced to %d which causes global.maxconn "
				        "to be limited to %d. Better reduce global.maxsslconn to get more "
				        "room for extra connections.\n", global.maxsslconn, global.maxconn);
			}
			fprintf(stderr, "Note: setting global.maxconn to %d\n", global.maxconn);
		}
	}

	global.maxsock = compute_ideal_maxsock(global.maxconn);
	global.hardmaxconn = global.maxconn;
	if (!global.maxpipes)
		global.maxpipes = compute_ideal_maxpipes();

	/* update connection pool thresholds */
	global.tune.pool_low_count  = ((long long)global.maxsock * global.tune.pool_low_ratio  + 99) / 100;
	global.tune.pool_high_count = ((long long)global.maxsock * global.tune.pool_high_ratio + 99) / 100;

	proxy_adjust_all_maxconn();

	if (global.tune.maxpollevents <= 0)
		global.tune.maxpollevents = MAX_POLL_EVENTS;

	if (global.tune.runqueue_depth <= 0)
		global.tune.runqueue_depth = RUNQUEUE_DEPTH;

	if (global.tune.recv_enough == 0)
		global.tune.recv_enough = MIN_RECV_AT_ONCE_ENOUGH;

	if (global.tune.maxrewrite >= global.tune.bufsize / 2)
		global.tune.maxrewrite = global.tune.bufsize / 2;

	if (arg_mode & (MODE_DEBUG | MODE_FOREGROUND)) {
		/* command line debug mode inhibits configuration mode */
		global.mode &= ~(MODE_DAEMON | MODE_QUIET);
		global.mode |= (arg_mode & (MODE_DEBUG | MODE_FOREGROUND));
	}

	if (arg_mode & MODE_DAEMON) {
		/* command line daemon mode inhibits foreground and debug modes mode */
		global.mode &= ~(MODE_DEBUG | MODE_FOREGROUND);
		global.mode |= arg_mode & MODE_DAEMON;
	}

	global.mode |= (arg_mode & (MODE_QUIET | MODE_VERBOSE));

	if ((global.mode & MODE_DEBUG) && (global.mode & (MODE_DAEMON | MODE_QUIET))) {
		ha_warning("<debug> mode incompatible with <quiet> and <daemon>. Keeping <debug> only.\n");
		global.mode &= ~(MODE_DAEMON | MODE_QUIET);
	}

	if ((global.nbproc > 1) && !(global.mode & (MODE_DAEMON | MODE_MWORKER))) {
		if (!(global.mode & (MODE_FOREGROUND | MODE_DEBUG)))
			ha_warning("<nbproc> is only meaningful in daemon mode or master-worker mode. Setting limit to 1 process.\n");
		global.nbproc = 1;
	}

	if (global.nbproc < 1)
		global.nbproc = 1;

	if (global.nbthread < 1)
		global.nbthread = 1;

	/* Realloc trash buffers because global.tune.bufsize may have changed */
	if (!init_trash_buffers(0)) {
		ha_alert("failed to initialize trash buffers.\n");
		exit(1);
	}

	if (!init_log_buffers()) {
		ha_alert("failed to initialize log buffers.\n");
		exit(1);
	}

	/*
	 * Note: we could register external pollers here.
	 * Built-in pollers have been registered before main().
	 */

	if (!(global.tune.options & GTUNE_USE_KQUEUE))
		disable_poller("kqueue");

	if (!(global.tune.options & GTUNE_USE_EVPORTS))
		disable_poller("evports");

	if (!(global.tune.options & GTUNE_USE_EPOLL))
		disable_poller("epoll");

	if (!(global.tune.options & GTUNE_USE_POLL))
		disable_poller("poll");

	if (!(global.tune.options & GTUNE_USE_SELECT))
		disable_poller("select");

	/* Note: we could disable any poller by name here */

	if (global.mode & (MODE_VERBOSE|MODE_DEBUG)) {
		list_pollers(stderr);
		fprintf(stderr, "\n");
		list_filters(stderr);
	}

	if (!init_pollers()) {
		ha_alert("No polling mechanism available.\n"
			 "  It is likely that haproxy was built with TARGET=generic and that FD_SETSIZE\n"
			 "  is too low on this platform to support maxconn and the number of listeners\n"
			 "  and servers. You should rebuild haproxy specifying your system using TARGET=\n"
			 "  in order to support other polling systems (poll, epoll, kqueue) or reduce the\n"
			 "  global maxconn setting to accommodate the system's limitation. For reference,\n"
			 "  FD_SETSIZE=%d on this system, global.maxconn=%d resulting in a maximum of\n"
			 "  %d file descriptors. You should thus reduce global.maxconn by %d. Also,\n"
			 "  check build settings using 'haproxy -vv'.\n\n",
			 FD_SETSIZE, global.maxconn, global.maxsock, (global.maxsock + 1 - FD_SETSIZE) / 2);
		exit(1);
	}
	if (global.mode & (MODE_VERBOSE|MODE_DEBUG)) {
		printf("Using %s() as the polling mechanism.\n", cur_poller.name);
	}

	if (!global.node)
		global.node = strdup(hostname);

	/* stop disabled proxies */
	for (px = proxies_list; px; px = px->next) {
		if (px->disabled)
			stop_proxy(px);
	}

	if (!hlua_post_init())
		exit(1);

	free(err_msg);
}

static void deinit_acl_cond(struct acl_cond *cond)
{
	struct acl_term_suite *suite, *suiteb;
	struct acl_term *term, *termb;

	if (!cond)
		return;

	list_for_each_entry_safe(suite, suiteb, &cond->suites, list) {
		list_for_each_entry_safe(term, termb, &suite->terms, list) {
			LIST_DEL(&term->list);
			free(term);
		}
		LIST_DEL(&suite->list);
		free(suite);
	}

	free(cond);
}

static void deinit_act_rules(struct list *rules)
{
	struct act_rule *rule, *ruleb;

	list_for_each_entry_safe(rule, ruleb, rules, list) {
		LIST_DEL(&rule->list);
		deinit_acl_cond(rule->cond);
		if (rule->release_ptr)
			rule->release_ptr(rule);
		free(rule);
	}
}

static void deinit_stick_rules(struct list *rules)
{
	struct sticking_rule *rule, *ruleb;

	list_for_each_entry_safe(rule, ruleb, rules, list) {
		LIST_DEL(&rule->list);
		deinit_acl_cond(rule->cond);
		release_sample_expr(rule->expr);
		free(rule);
	}
}

void deinit(void)
{
	struct proxy *p = proxies_list, *p0;
	struct cap_hdr *h,*h_next;
	struct server *s,*s_next;
	struct listener *l,*l_next;
	struct acl_cond *cond, *condb;
	struct acl *acl, *aclb;
	struct switching_rule *rule, *ruleb;
	struct server_rule *srule, *sruleb;
	struct redirect_rule *rdr, *rdrb;
	struct wordlist *wl, *wlb;
	struct uri_auth *uap, *ua = NULL;
	struct logsrv *log, *logb;
	struct logformat_node *lf, *lfb;
	struct bind_conf *bind_conf, *bind_back;
	struct build_opts_str *bol, *bolb;
	struct post_deinit_fct *pdf, *pdfb;
	struct proxy_deinit_fct *pxdf, *pxdfb;
	struct server_deinit_fct *srvdf, *srvdfb;
	struct per_thread_init_fct *tif, *tifb;
	struct per_thread_deinit_fct *tdf, *tdfb;
	struct per_thread_alloc_fct *taf, *tafb;
	struct per_thread_free_fct *tff, *tffb;
	struct post_server_check_fct *pscf, *pscfb;
	struct post_check_fct *pcf, *pcfb;
	struct post_proxy_check_fct *ppcf, *ppcfb;
	int cur_fd;

	/* At this point the listeners state is weird:
	 *  - most listeners are still bound and referenced in their protocol
	 *  - some might be zombies that are not in their proto anymore, but
	 *    still appear in their proxy's listeners with a valid FD.
	 *  - some might be stopped and still appear in their proxy as FD #-1
	 *  - among all of them, some might be inherited hence shared and we're
	 *    not allowed to pause them or whatever, we must just close them.
	 *  - finally some are not listeners (pipes, logs, stdout, etc) and
	 *    must be left intact.
	 *
	 * The safe way to proceed is to unbind (and close) whatever is not yet
	 * unbound so that no more receiver/listener remains alive. Then close
	 * remaining listener FDs, which correspond to zombie listeners (those
	 * belonging to disabled proxies that were in another process).
	 * objt_listener() would be cleaner here but not converted yet.
	 */
	protocol_unbind_all();

	for (cur_fd = 0; cur_fd < global.maxsock; cur_fd++) {
		if (!fdtab || !fdtab[cur_fd].owner)
			continue;

		if (fdtab[cur_fd].iocb == &sock_accept_iocb) {
			struct listener *l = fdtab[cur_fd].owner;

			BUG_ON(l->state != LI_INIT);
			unbind_listener(l);
		}
	}

	deinit_signals();
	while (p) {
		free(p->conf.file);
		free(p->id);
		free(p->cookie_name);
		free(p->cookie_domain);
		free(p->cookie_attrs);
		free(p->lbprm.arg_str);
		free(p->capture_name);
		free(p->monitor_uri);
		free(p->rdp_cookie_name);
		free(p->invalid_rep);
		free(p->invalid_req);
		if (p->conf.logformat_string != default_http_log_format &&
		    p->conf.logformat_string != default_tcp_log_format &&
		    p->conf.logformat_string != clf_http_log_format)
			free(p->conf.logformat_string);

		free(p->conf.lfs_file);
		free(p->conf.uniqueid_format_string);
		istfree(&p->header_unique_id);
		free(p->conf.uif_file);
		if ((p->lbprm.algo & BE_LB_LKUP) == BE_LB_LKUP_MAP)
			free(p->lbprm.map.srv);

		if (p->conf.logformat_sd_string != default_rfc5424_sd_log_format)
			free(p->conf.logformat_sd_string);
		free(p->conf.lfsd_file);

		list_for_each_entry_safe(cond, condb, &p->mon_fail_cond, list) {
			LIST_DEL(&cond->list);
			prune_acl_cond(cond);
			free(cond);
		}

		EXTRA_COUNTERS_FREE(p->extra_counters_fe);
		EXTRA_COUNTERS_FREE(p->extra_counters_be);

		/* build a list of unique uri_auths */
		if (!ua)
			ua = p->uri_auth;
		else {
			/* check if p->uri_auth is unique */
			for (uap = ua; uap; uap=uap->next)
				if (uap == p->uri_auth)
					break;

			if (!uap && p->uri_auth) {
				/* add it, if it is */
				p->uri_auth->next = ua;
				ua = p->uri_auth;
			}
		}

		list_for_each_entry_safe(acl, aclb, &p->acl, list) {
			LIST_DEL(&acl->list);
			prune_acl(acl);
			free(acl);
		}

		list_for_each_entry_safe(srule, sruleb, &p->server_rules, list) {
			LIST_DEL(&srule->list);
			prune_acl_cond(srule->cond);
			list_for_each_entry_safe(lf, lfb, &srule->expr, list) {
				LIST_DEL(&lf->list);
				release_sample_expr(lf->expr);
				free(lf->arg);
				free(lf);
			}
			free(srule->file);
			free(srule->cond);
			free(srule);
		}

		list_for_each_entry_safe(rule, ruleb, &p->switching_rules, list) {
			LIST_DEL(&rule->list);
			if (rule->cond) {
				prune_acl_cond(rule->cond);
				free(rule->cond);
			}
			free(rule->file);
			free(rule);
		}

		list_for_each_entry_safe(rdr, rdrb, &p->redirect_rules, list) {
			LIST_DEL(&rdr->list);
			if (rdr->cond) {
				prune_acl_cond(rdr->cond);
				free(rdr->cond);
			}
			free(rdr->rdr_str);
			list_for_each_entry_safe(lf, lfb, &rdr->rdr_fmt, list) {
				LIST_DEL(&lf->list);
				free(lf);
			}
			free(rdr);
		}

		list_for_each_entry_safe(log, logb, &p->logsrvs, list) {
			LIST_DEL(&log->list);
			free(log);
		}

		list_for_each_entry_safe(lf, lfb, &p->logformat, list) {
			LIST_DEL(&lf->list);
			release_sample_expr(lf->expr);
			free(lf->arg);
			free(lf);
		}

		list_for_each_entry_safe(lf, lfb, &p->logformat_sd, list) {
			LIST_DEL(&lf->list);
			release_sample_expr(lf->expr);
			free(lf->arg);
			free(lf);
		}

		list_for_each_entry_safe(lf, lfb, &p->format_unique_id, list) {
			LIST_DEL(&lf->list);
			release_sample_expr(lf->expr);
			free(lf->arg);
			free(lf);
		}

		deinit_act_rules(&p->tcp_req.inspect_rules);
		deinit_act_rules(&p->tcp_rep.inspect_rules);
		deinit_act_rules(&p->tcp_req.l4_rules);
		deinit_act_rules(&p->tcp_req.l5_rules);
		deinit_act_rules(&p->http_req_rules);
		deinit_act_rules(&p->http_res_rules);
		deinit_act_rules(&p->http_after_res_rules);

		deinit_stick_rules(&p->storersp_rules);
		deinit_stick_rules(&p->sticking_rules);

		h = p->req_cap;
		while (h) {
			h_next = h->next;
			free(h->name);
			pool_destroy(h->pool);
			free(h);
			h = h_next;
		}/* end while(h) */

		h = p->rsp_cap;
		while (h) {
			h_next = h->next;
			free(h->name);
			pool_destroy(h->pool);
			free(h);
			h = h_next;
		}/* end while(h) */

		s = p->srv;
		while (s) {
			s_next = s->next;


			task_destroy(s->warmup);

			free(s->id);
			free(s->cookie);
			free(s->hostname);
			free(s->hostname_dn);
			free((char*)s->conf.file);
			free(s->idle_conns);
			free(s->safe_conns);
			free(s->available_conns);
			free(s->curr_idle_thr);
			free(s->resolvers_id);
			free(s->addr_node.key);

			if (s->use_ssl == 1 || s->check.use_ssl == 1 || (s->proxy->options & PR_O_TCPCHK_SSL)) {
				if (xprt_get(XPRT_SSL) && xprt_get(XPRT_SSL)->destroy_srv)
					xprt_get(XPRT_SSL)->destroy_srv(s);
			}
			HA_SPIN_DESTROY(&s->lock);

			list_for_each_entry(srvdf, &server_deinit_list, list)
				srvdf->fct(s);

			EXTRA_COUNTERS_FREE(s->extra_counters);
			free(s);
			s = s_next;
		}/* end while(s) */

		list_for_each_entry_safe(l, l_next, &p->conf.listeners, by_fe) {
			LIST_DEL(&l->by_fe);
			LIST_DEL(&l->by_bind);
			free(l->name);
			free(l->counters);

			EXTRA_COUNTERS_FREE(l->extra_counters);
			free(l);
		}

		/* Release unused SSL configs. */
		list_for_each_entry_safe(bind_conf, bind_back, &p->conf.bind, by_fe) {
			if (bind_conf->xprt->destroy_bind_conf)
				bind_conf->xprt->destroy_bind_conf(bind_conf);
			free(bind_conf->file);
			free(bind_conf->arg);
			LIST_DEL(&bind_conf->by_fe);
			free(bind_conf);
		}

		flt_deinit(p);

		list_for_each_entry(pxdf, &proxy_deinit_list, list)
			pxdf->fct(p);

		free(p->desc);
		free(p->fwdfor_hdr_name);

		task_destroy(p->task);

		pool_destroy(p->req_cap_pool);
		pool_destroy(p->rsp_cap_pool);
		if (p->table)
			pool_destroy(p->table->pool);

		p0 = p;
		p = p->next;
		HA_RWLOCK_DESTROY(&p0->lbprm.lock);
		HA_RWLOCK_DESTROY(&p0->lock);
		free(p0);
	}/* end while(p) */

	while (ua) {
		struct stat_scope *scope, *scopep;

		uap = ua;
		ua = ua->next;

		free(uap->uri_prefix);
		free(uap->auth_realm);
		free(uap->node);
		free(uap->desc);

		userlist_free(uap->userlist);
		deinit_act_rules(&uap->http_req_rules);

		scope = uap->scope;
		while (scope) {
			scopep = scope;
			scope = scope->next;

			free(scopep->px_id);
			free(scopep);
		}

		free(uap);
	}

	userlist_free(userlist);

	cfg_unregister_sections();

	deinit_log_buffers();

	list_for_each_entry(pdf, &post_deinit_list, list)
		pdf->fct();

	free(global.log_send_hostname); global.log_send_hostname = NULL;
	chunk_destroy(&global.log_tag);
	free(global.chroot);  global.chroot = NULL;
	free(global.pidfile); global.pidfile = NULL;
	free(global.node);    global.node = NULL;
	free(global.desc);    global.desc = NULL;
	free(oldpids);        oldpids = NULL;
	free(old_argv);       old_argv = NULL;
	free(localpeer);      localpeer = NULL;
	task_destroy(idle_conn_task);
	idle_conn_task = NULL;

	list_for_each_entry_safe(log, logb, &global.logsrvs, list) {
			LIST_DEL(&log->list);
			free(log);
		}
	list_for_each_entry_safe(wl, wlb, &cfg_cfgfiles, list) {
		free(wl->s);
		LIST_DEL(&wl->list);
		free(wl);
	}

	list_for_each_entry_safe(bol, bolb, &build_opts_list, list) {
		if (bol->must_free)
			free((void *)bol->str);
		LIST_DEL(&bol->list);
		free(bol);
	}

	list_for_each_entry_safe(pxdf, pxdfb, &proxy_deinit_list, list) {
		LIST_DEL(&pxdf->list);
		free(pxdf);
	}

	list_for_each_entry_safe(pdf, pdfb, &post_deinit_list, list) {
		LIST_DEL(&pdf->list);
		free(pdf);
	}

	list_for_each_entry_safe(srvdf, srvdfb, &server_deinit_list, list) {
		LIST_DEL(&srvdf->list);
		free(srvdf);
	}

	list_for_each_entry_safe(pcf, pcfb, &post_check_list, list) {
		LIST_DEL(&pcf->list);
		free(pcf);
	}

	list_for_each_entry_safe(pscf, pscfb, &post_server_check_list, list) {
		LIST_DEL(&pscf->list);
		free(pscf);
	}

	list_for_each_entry_safe(ppcf, ppcfb, &post_proxy_check_list, list) {
		LIST_DEL(&ppcf->list);
		free(ppcf);
	}

	list_for_each_entry_safe(tif, tifb, &per_thread_init_list, list) {
		LIST_DEL(&tif->list);
		free(tif);
	}

	list_for_each_entry_safe(tdf, tdfb, &per_thread_deinit_list, list) {
		LIST_DEL(&tdf->list);
		free(tdf);
	}

	list_for_each_entry_safe(taf, tafb, &per_thread_alloc_list, list) {
		LIST_DEL(&taf->list);
		free(taf);
	}

	list_for_each_entry_safe(tff, tffb, &per_thread_free_list, list) {
		LIST_DEL(&tff->list);
		free(tff);
	}

	vars_prune(&global.vars, NULL, NULL);
	pool_destroy_all();
	deinit_pollers();
} /* end deinit() */

__attribute__((noreturn)) void deinit_and_exit(int status)
{
	deinit();
	exit(status);
}

/* Runs the polling loop */
void run_poll_loop()
{
	int next, wake;

	tv_update_date(0,1);
	while (1) {
		wake_expired_tasks();

		/* check if we caught some signals and process them in the
		 first thread */
		if (signal_queue_len && tid == 0) {
			activity[tid].wake_signal++;
			signal_process_queue();
		}

		/* Process a few tasks */
		process_runnable_tasks();

		/* also stop  if we failed to cleanly stop all tasks */
		if (killed > 1)
			break;

		/* expire immediately if events are pending */
		wake = 1;
		if (thread_has_tasks())
			activity[tid].wake_tasks++;
		else {
			_HA_ATOMIC_OR(&sleeping_thread_mask, tid_bit);
			__ha_barrier_atomic_store();
			if (thread_has_tasks()) {
				activity[tid].wake_tasks++;
				_HA_ATOMIC_AND(&sleeping_thread_mask, ~tid_bit);
			} else
				wake = 0;
		}

		if (!wake) {
			int i;

			if (stopping) {
				if (_HA_ATOMIC_OR(&stopping_thread_mask, tid_bit) == tid_bit) {
					/* notify all threads that stopping was just set */
					for (i = 0; i < global.nbthread; i++)
						if (((all_threads_mask & ~stopping_thread_mask) >> i) & 1)
							wake_thread(i);
				}
			}

			/* stop when there's nothing left to do */
			if ((jobs - unstoppable_jobs) == 0 &&
			    (stopping_thread_mask & all_threads_mask) == all_threads_mask) {
				/* wake all threads waiting on jobs==0 */
				for (i = 0; i < global.nbthread; i++)
					if (((all_threads_mask & ~tid_bit) >> i) & 1)
						wake_thread(i);
				break;
			}
		}

		/* If we have to sleep, measure how long */
		next = wake ? TICK_ETERNITY : next_timer_expiry();

		/* The poller will ensure it returns around <next> */
		cur_poller.poll(&cur_poller, next, wake);

		activity[tid].loops++;
	}
}

static void *run_thread_poll_loop(void *data)
{
	struct per_thread_alloc_fct  *ptaf;
	struct per_thread_init_fct   *ptif;
	struct per_thread_deinit_fct *ptdf;
	struct per_thread_free_fct   *ptff;
	static int init_left = 0;
	__decl_thread(static pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER);
	__decl_thread(static pthread_cond_t  init_cond  = PTHREAD_COND_INITIALIZER);

	ha_set_tid((unsigned long)data);
	sched = &task_per_thread[tid];

#if (_POSIX_TIMERS > 0) && defined(_POSIX_THREAD_CPUTIME)
#ifdef USE_THREAD
	pthread_getcpuclockid(pthread_self(), &ti->clock_id);
#else
	ti->clock_id = CLOCK_THREAD_CPUTIME_ID;
#endif
#endif
	/* Now, initialize one thread init at a time. This is better since
	 * some init code is a bit tricky and may release global resources
	 * after reallocating them locally. This will also ensure there is
	 * no race on file descriptors allocation.
	 */
#ifdef USE_THREAD
	pthread_mutex_lock(&init_mutex);
#endif
	/* The first thread must set the number of threads left */
	if (!init_left)
		init_left = global.nbthread;
	init_left--;

	tv_update_date(-1,-1);

	/* per-thread alloc calls performed here are not allowed to snoop on
	 * other threads, so they are free to initialize at their own rhythm
	 * as long as they act as if they were alone. None of them may rely
	 * on resources initialized by the other ones.
	 */
	list_for_each_entry(ptaf, &per_thread_alloc_list, list) {
		if (!ptaf->fct()) {
			ha_alert("failed to allocate resources for thread %u.\n", tid);
			exit(1);
		}
	}

	/* per-thread init calls performed here are not allowed to snoop on
	 * other threads, so they are free to initialize at their own rhythm
	 * as long as they act as if they were alone.
	 */
	list_for_each_entry(ptif, &per_thread_init_list, list) {
		if (!ptif->fct()) {
			ha_alert("failed to initialize thread %u.\n", tid);
			exit(1);
		}
	}

	/* enabling protocols will result in fd_insert() calls to be performed,
	 * we want all threads to have already allocated their local fd tables
	 * before doing so, thus only the last thread does it.
	 */
	if (init_left == 0)
		protocol_enable_all();

#ifdef USE_THREAD
	pthread_cond_broadcast(&init_cond);
	pthread_mutex_unlock(&init_mutex);

	/* now wait for other threads to finish starting */
	pthread_mutex_lock(&init_mutex);
	while (init_left)
		pthread_cond_wait(&init_cond, &init_mutex);
	pthread_mutex_unlock(&init_mutex);
#endif

#if defined(PR_SET_NO_NEW_PRIVS) && defined(USE_PRCTL)
	/* Let's refrain from using setuid executables. This way the impact of
	 * an eventual vulnerability in a library remains limited. It may
	 * impact external checks but who cares about them anyway ? In the
	 * worst case it's possible to disable the option. Obviously we do this
	 * in workers only. We can't hard-fail on this one as it really is
	 * implementation dependent though we're interested in feedback, hence
	 * the warning.
	 */
	if (!(global.tune.options & GTUNE_INSECURE_SETUID) && !master) {
		static int warn_fail;
		if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == -1 && !_HA_ATOMIC_XADD(&warn_fail, 1)) {
			ha_warning("Failed to disable setuid, please report to developers with detailed "
				   "information about your operating system. You can silence this warning "
				   "by adding 'insecure-setuid-wanted' in the 'global' section.\n");
		}
	}
#endif

#if defined(RLIMIT_NPROC)
	/* all threads have started, it's now time to prevent any new thread
	 * or process from starting. Obviously we do this in workers only. We
	 * can't hard-fail on this one as it really is implementation dependent
	 * though we're interested in feedback, hence the warning.
	 */
	if (!(global.tune.options & GTUNE_INSECURE_FORK) && !master) {
		struct rlimit limit = { .rlim_cur = 0, .rlim_max = 0 };
		static int warn_fail;

		if (setrlimit(RLIMIT_NPROC, &limit) == -1 && !_HA_ATOMIC_XADD(&warn_fail, 1)) {
			ha_warning("Failed to disable forks, please report to developers with detailed "
				   "information about your operating system. You can silence this warning "
				   "by adding 'insecure-fork-wanted' in the 'global' section.\n");
		}
	}
#endif
	run_poll_loop();

	list_for_each_entry(ptdf, &per_thread_deinit_list, list)
		ptdf->fct();

	list_for_each_entry(ptff, &per_thread_free_list, list)
		ptff->fct();

#ifdef USE_THREAD
	_HA_ATOMIC_AND(&all_threads_mask, ~tid_bit);
	if (tid > 0)
		pthread_exit(NULL);
#endif
	return NULL;
}

/* set uid/gid depending on global settings */
static void set_identity(const char *program_name)
{
	if (global.gid) {
		if (getgroups(0, NULL) > 0 && setgroups(0, NULL) == -1)
			ha_warning("[%s.main()] Failed to drop supplementary groups. Using 'gid'/'group'"
				   " without 'uid'/'user' is generally useless.\n", program_name);

		if (setgid(global.gid) == -1) {
			ha_alert("[%s.main()] Cannot set gid %d.\n", program_name, global.gid);
			protocol_unbind_all();
			exit(1);
		}
	}

	if (global.uid && setuid(global.uid) == -1) {
		ha_alert("[%s.main()] Cannot set uid %d.\n", program_name, global.uid);
		protocol_unbind_all();
		exit(1);
	}
}

int main(int argc, char **argv)
{
	int err, retry;
	struct rlimit limit;
	int pidfd = -1;

	setvbuf(stdout, NULL, _IONBF, 0);

	/* this can only safely be done here, though it's optimized away by
	 * the compiler.
	 */
	if (MAX_PROCS < 1 || MAX_PROCS > LONGBITS) {
		ha_alert("MAX_PROCS value must be between 1 and %d inclusive; "
		         "HAProxy was built with value %d, please fix it and rebuild.\n",
			 LONGBITS, MAX_PROCS);
		exit(1);
	}

	/* take a copy of initial limits before we possibly change them */
	getrlimit(RLIMIT_NOFILE, &limit);

	if (limit.rlim_max == RLIM_INFINITY)
		limit.rlim_max = limit.rlim_cur;
	rlim_fd_cur_at_boot = limit.rlim_cur;
	rlim_fd_max_at_boot = limit.rlim_max;

	/* process all initcalls in order of potential dependency */
	RUN_INITCALLS(STG_PREPARE);
	RUN_INITCALLS(STG_LOCK);
	RUN_INITCALLS(STG_ALLOC);
	RUN_INITCALLS(STG_POOL);
	RUN_INITCALLS(STG_REGISTER);
	RUN_INITCALLS(STG_INIT);

	init(argc, argv);
	signal_register_fct(SIGQUIT, dump, SIGQUIT);
	signal_register_fct(SIGUSR1, sig_soft_stop, SIGUSR1);
	signal_register_fct(SIGHUP, sig_dump_state, SIGHUP);
	signal_register_fct(SIGUSR2, NULL, 0);

	/* Always catch SIGPIPE even on platforms which define MSG_NOSIGNAL.
	 * Some recent FreeBSD setups report broken pipes, and MSG_NOSIGNAL
	 * was defined there, so let's stay on the safe side.
	 */
	signal_register_fct(SIGPIPE, NULL, 0);

	/* ulimits */
	if (!global.rlimit_nofile)
		global.rlimit_nofile = global.maxsock;

	if (global.rlimit_nofile) {
		limit.rlim_cur = global.rlimit_nofile;
		limit.rlim_max = MAX(rlim_fd_max_at_boot, limit.rlim_cur);

		if (setrlimit(RLIMIT_NOFILE, &limit) == -1) {
			getrlimit(RLIMIT_NOFILE, &limit);
			if (global.tune.options & GTUNE_STRICT_LIMITS) {
				ha_alert("[%s.main()] Cannot raise FD limit to %d, limit is %d.\n",
					 argv[0], global.rlimit_nofile, (int)limit.rlim_cur);
				if (!(global.mode & MODE_MWORKER))
					exit(1);
			}
			else {
				/* try to set it to the max possible at least */
				limit.rlim_cur = limit.rlim_max;
				if (setrlimit(RLIMIT_NOFILE, &limit) != -1)
					getrlimit(RLIMIT_NOFILE, &limit);

				ha_warning("[%s.main()] Cannot raise FD limit to %d, limit is %d.\n",
					   argv[0], global.rlimit_nofile, (int)limit.rlim_cur);
				global.rlimit_nofile = limit.rlim_cur;
			}
		}
	}

	if (global.rlimit_memmax) {
		limit.rlim_cur = limit.rlim_max =
			global.rlimit_memmax * 1048576ULL;
#ifdef RLIMIT_AS
		if (setrlimit(RLIMIT_AS, &limit) == -1) {
			if (global.tune.options & GTUNE_STRICT_LIMITS) {
				ha_alert("[%s.main()] Cannot fix MEM limit to %d megs.\n",
					 argv[0], global.rlimit_memmax);
				if (!(global.mode & MODE_MWORKER))
					exit(1);
			}
			else
				ha_warning("[%s.main()] Cannot fix MEM limit to %d megs.\n",
					   argv[0], global.rlimit_memmax);
		}
#else
		if (setrlimit(RLIMIT_DATA, &limit) == -1) {
			if (global.tune.options & GTUNE_STRICT_LIMITS) {
				ha_alert("[%s.main()] Cannot fix MEM limit to %d megs.\n",
					 argv[0], global.rlimit_memmax);
				if (!(global.mode & MODE_MWORKER))
					exit(1);
			}
			else
				ha_warning("[%s.main()] Cannot fix MEM limit to %d megs.\n",
					   argv[0], global.rlimit_memmax);
		}
#endif
	}

	if (old_unixsocket) {
		if (strcmp("/dev/null", old_unixsocket) != 0) {
			if (sock_get_old_sockets(old_unixsocket) != 0) {
				ha_alert("Failed to get the sockets from the old process!\n");
				if (!(global.mode & MODE_MWORKER))
					exit(1);
			}
		}
	}
	get_cur_unixsocket();

	/* We will loop at most 100 times with 10 ms delay each time.
	 * That's at most 1 second. We only send a signal to old pids
	 * if we cannot grab at least one port.
	 */
	retry = MAX_START_RETRIES;
	err = ERR_NONE;
	while (retry >= 0) {
		struct timeval w;
		err = protocol_bind_all(retry == 0 || nb_oldpids == 0);
		/* exit the loop on no error or fatal error */
		if ((err & (ERR_RETRYABLE|ERR_FATAL)) != ERR_RETRYABLE)
			break;
		if (nb_oldpids == 0 || retry == 0)
			break;

		/* FIXME-20060514: Solaris and OpenBSD do not support shutdown() on
		 * listening sockets. So on those platforms, it would be wiser to
		 * simply send SIGUSR1, which will not be undoable.
		 */
		if (tell_old_pids(SIGTTOU) == 0) {
			/* no need to wait if we can't contact old pids */
			retry = 0;
			continue;
		}
		/* give some time to old processes to stop listening */
		w.tv_sec = 0;
		w.tv_usec = 10*1000;
		select(0, NULL, NULL, NULL, &w);
		retry--;
	}

	/* Note: protocol_bind_all() sends an alert when it fails. */
	if ((err & ~ERR_WARN) != ERR_NONE) {
		ha_alert("[%s.main()] Some protocols failed to start their listeners! Exiting.\n", argv[0]);
		if (retry != MAX_START_RETRIES && nb_oldpids) {
			protocol_unbind_all(); /* cleanup everything we can */
			tell_old_pids(SIGTTIN);
		}
		exit(1);
	}

	if (!(global.mode & MODE_MWORKER_WAIT) && listeners == 0) {
		ha_alert("[%s.main()] No enabled listener found (check for 'bind' directives) ! Exiting.\n", argv[0]);
		/* Note: we don't have to send anything to the old pids because we
		 * never stopped them. */
		exit(1);
	}

	/* Ok, all listeners should now be bound, close any leftover sockets
	 * the previous process gave us, we don't need them anymore
	 */
	while (xfer_sock_list != NULL) {
		struct xfer_sock_list *tmpxfer = xfer_sock_list->next;
		close(xfer_sock_list->fd);
		free(xfer_sock_list->iface);
		free(xfer_sock_list->namespace);
		free(xfer_sock_list);
		xfer_sock_list = tmpxfer;
	}

	/* prepare pause/play signals */
	signal_register_fct(SIGTTOU, sig_pause, SIGTTOU);
	signal_register_fct(SIGTTIN, sig_listen, SIGTTIN);

	/* MODE_QUIET can inhibit alerts and warnings below this line */

	if (getenv("HAPROXY_MWORKER_REEXEC") != NULL) {
		/* either stdin/out/err are already closed or should stay as they are. */
		if ((global.mode & MODE_DAEMON)) {
			/* daemon mode re-executing, stdin/stdout/stderr are already closed so keep quiet */
			global.mode &= ~MODE_VERBOSE;
			global.mode |= MODE_QUIET; /* ensure that we won't say anything from now */
		}
	} else {
		if ((global.mode & MODE_QUIET) && !(global.mode & MODE_VERBOSE)) {
			/* detach from the tty */
			stdio_quiet(-1);
		}
	}

	/* open log & pid files before the chroot */
	if ((global.mode & MODE_DAEMON || global.mode & MODE_MWORKER) && global.pidfile != NULL) {
		unlink(global.pidfile);
		pidfd = open(global.pidfile, O_CREAT | O_WRONLY | O_TRUNC, 0644);
		if (pidfd < 0) {
			ha_alert("[%s.main()] Cannot create pidfile %s\n", argv[0], global.pidfile);
			if (nb_oldpids)
				tell_old_pids(SIGTTIN);
			protocol_unbind_all();
			exit(1);
		}
	}

	if ((global.last_checks & LSTCHK_NETADM) && global.uid) {
		ha_alert("[%s.main()] Some configuration options require full privileges, so global.uid cannot be changed.\n"
			 "", argv[0]);
		protocol_unbind_all();
		exit(1);
	}

	/* If the user is not root, we'll still let them try the configuration
	 * but we inform them that unexpected behaviour may occur.
	 */
	if ((global.last_checks & LSTCHK_NETADM) && getuid())
		ha_warning("[%s.main()] Some options which require full privileges"
			   " might not work well.\n"
			   "", argv[0]);

	if ((global.mode & (MODE_MWORKER|MODE_DAEMON)) == 0) {

		/* chroot if needed */
		if (global.chroot != NULL) {
			if (chroot(global.chroot) == -1 || chdir("/") == -1) {
				ha_alert("[%s.main()] Cannot chroot(%s).\n", argv[0], global.chroot);
				if (nb_oldpids)
					tell_old_pids(SIGTTIN);
				protocol_unbind_all();
				exit(1);
			}
		}
	}

	if (nb_oldpids && !(global.mode & MODE_MWORKER_WAIT))
		nb_oldpids = tell_old_pids(oldpids_sig);

	/* send a SIGTERM to workers who have a too high reloads number  */
	if ((global.mode & MODE_MWORKER) && !(global.mode & MODE_MWORKER_WAIT))
		mworker_kill_max_reloads(SIGTERM);

	if ((getenv("HAPROXY_MWORKER_REEXEC") == NULL)) {
		nb_oldpids = 0;
		free(oldpids);
		oldpids = NULL;
	}


	/* Note that any error at this stage will be fatal because we will not
	 * be able to restart the old pids.
	 */

	if ((global.mode & (MODE_MWORKER | MODE_DAEMON)) == 0)
		set_identity(argv[0]);

	/* check ulimits */
	limit.rlim_cur = limit.rlim_max = 0;
	getrlimit(RLIMIT_NOFILE, &limit);
	if (limit.rlim_cur < global.maxsock) {
		if (global.tune.options & GTUNE_STRICT_LIMITS) {
			ha_alert("[%s.main()] FD limit (%d) too low for maxconn=%d/maxsock=%d. "
				 "Please raise 'ulimit-n' to %d or more to avoid any trouble.\n",
			         argv[0], (int)limit.rlim_cur, global.maxconn, global.maxsock,
				 global.maxsock);
			if (!(global.mode & MODE_MWORKER))
				exit(1);
		}
		else
			ha_alert("[%s.main()] FD limit (%d) too low for maxconn=%d/maxsock=%d. "
				 "Please raise 'ulimit-n' to %d or more to avoid any trouble.\n",
			         argv[0], (int)limit.rlim_cur, global.maxconn, global.maxsock,
				 global.maxsock);
	}

	if (global.mode & (MODE_DAEMON | MODE_MWORKER | MODE_MWORKER_WAIT)) {
		struct proxy *px;
		struct peers *curpeers;
		int ret = 0;
		int proc;
		int devnullfd = -1;

		/*
		 * if daemon + mworker: must fork here to let a master
		 * process live in background before forking children
		 */

		if ((getenv("HAPROXY_MWORKER_REEXEC") == NULL)
		    && (global.mode & MODE_MWORKER)
		    && (global.mode & MODE_DAEMON)) {
			ret = fork();
			if (ret < 0) {
				ha_alert("[%s.main()] Cannot fork.\n", argv[0]);
				protocol_unbind_all();
				exit(1); /* there has been an error */
			} else if (ret > 0) { /* parent leave to daemonize */
				exit(0);
			} else /* change the process group ID in the child (master process) */
				setsid();
		}


		/* if in master-worker mode, write the PID of the father */
		if (global.mode & MODE_MWORKER) {
			char pidstr[100];
			snprintf(pidstr, sizeof(pidstr), "%d\n", (int)getpid());
			if (pidfd >= 0)
				DISGUISE(write(pidfd, pidstr, strlen(pidstr)));
		}

		/* the father launches the required number of processes */
		if (!(global.mode & MODE_MWORKER_WAIT)) {
			if (global.mode & MODE_MWORKER)
				mworker_ext_launch_all();
			for (proc = 0; proc < global.nbproc; proc++) {
				ret = fork();
				if (ret < 0) {
					ha_alert("[%s.main()] Cannot fork.\n", argv[0]);
					protocol_unbind_all();
					exit(1); /* there has been an error */
				}
				else if (ret == 0) { /* child breaks here */
					ha_random_jump96(relative_pid);
					break;
				}
				if (pidfd >= 0 && !(global.mode & MODE_MWORKER)) {
					char pidstr[100];
					snprintf(pidstr, sizeof(pidstr), "%d\n", ret);
					DISGUISE(write(pidfd, pidstr, strlen(pidstr)));
				}
				if (global.mode & MODE_MWORKER) {
					struct mworker_proc *child;

					ha_notice("New worker #%d (%d) forked\n", relative_pid, ret);
					/* find the right mworker_proc */
					list_for_each_entry(child, &proc_list, list) {
						if (child->relative_pid == relative_pid &&
						    child->reloads == 0 && child->options & PROC_O_TYPE_WORKER) {
							child->timestamp = now.tv_sec;
							child->pid = ret;
							child->version = strdup(haproxy_version);
							break;
						}
					}
				}

				relative_pid++; /* each child will get a different one */
				pid_bit <<= 1;
			}
		} else {
			/* wait mode */
			global.nbproc = 1;
			proc = 1;
		}

#ifdef USE_CPU_AFFINITY
		if (proc < global.nbproc &&  /* child */
		    proc < MAX_PROCS &&       /* only the first 32/64 processes may be pinned */
		    global.cpu_map.proc[proc])    /* only do this if the process has a CPU map */
#ifdef __FreeBSD__
		{
			cpuset_t cpuset;
			int i;
			unsigned long cpu_map = global.cpu_map.proc[proc];

			CPU_ZERO(&cpuset);
			while ((i = ffsl(cpu_map)) > 0) {
				CPU_SET(i - 1, &cpuset);
				cpu_map &= ~(1UL << (i - 1));
			}
			ret = cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, sizeof(cpuset), &cpuset);
		}
#elif defined(__linux__) || defined(__DragonFly__)
			sched_setaffinity(0, sizeof(unsigned long), (void *)&global.cpu_map.proc[proc]);
#endif
#endif
		/* close the pidfile both in children and father */
		if (pidfd >= 0) {
			//lseek(pidfd, 0, SEEK_SET);  /* debug: emulate eglibc bug */
			close(pidfd);
		}

		/* We won't ever use this anymore */
		free(global.pidfile); global.pidfile = NULL;

		if (proc == global.nbproc) {
			if (global.mode & (MODE_MWORKER|MODE_MWORKER_WAIT)) {

				if ((!(global.mode & MODE_QUIET) || (global.mode & MODE_VERBOSE)) &&
					(global.mode & MODE_DAEMON)) {
					/* detach from the tty, this is required to properly daemonize. */
					if ((getenv("HAPROXY_MWORKER_REEXEC") == NULL))
						stdio_quiet(-1);

					global.mode &= ~MODE_VERBOSE;
					global.mode |= MODE_QUIET; /* ensure that we won't say anything from now */
				}

				mworker_loop();
				/* should never get there */
				exit(EXIT_FAILURE);
			}
#if defined(USE_OPENSSL) && !defined(OPENSSL_NO_DH)
			ssl_free_dh();
#endif
			exit(0); /* parent must leave */
		}

		/* child must never use the atexit function */
		atexit_flag = 0;

		/* close useless master sockets */
		if (global.mode & MODE_MWORKER) {
			struct mworker_proc *child, *it;
			master = 0;

			mworker_cli_proxy_stop();

			/* free proc struct of other processes  */
			list_for_each_entry_safe(child, it, &proc_list, list) {
				/* close the FD of the master side for all
				 * workers, we don't need to close the worker
				 * side of other workers since it's done with
				 * the bind_proc */
				if (child->ipc_fd[0] >= 0)
					close(child->ipc_fd[0]);
				if (child->relative_pid == relative_pid &&
				    child->reloads == 0) {
					/* keep this struct if this is our pid */
					proc_self = child;
					continue;
				}
				LIST_DEL(&child->list);
				mworker_free_child(child);
				child = NULL;
			}
		}

		if (!(global.mode & MODE_QUIET) || (global.mode & MODE_VERBOSE)) {
			devnullfd = open("/dev/null", O_RDWR, 0);
			if (devnullfd < 0) {
				ha_alert("Cannot open /dev/null\n");
				exit(EXIT_FAILURE);
			}
		}

		/* Must chroot and setgid/setuid in the children */
		/* chroot if needed */
		if (global.chroot != NULL) {
			if (chroot(global.chroot) == -1 || chdir("/") == -1) {
				ha_alert("[%s.main()] Cannot chroot1(%s).\n", argv[0], global.chroot);
				if (nb_oldpids)
					tell_old_pids(SIGTTIN);
				protocol_unbind_all();
				exit(1);
			}
		}

		free(global.chroot);
		global.chroot = NULL;
		set_identity(argv[0]);

		/* pass through every cli socket, and check if it's bound to
		 * the current process and if it exposes listeners sockets.
		 * Caution: the GTUNE_SOCKET_TRANSFER is now set after the fork.
		 * */

		if (global.stats_fe) {
			struct bind_conf *bind_conf;

			list_for_each_entry(bind_conf, &global.stats_fe->conf.bind, by_fe) {
				if (bind_conf->level & ACCESS_FD_LISTENERS) {
					if (!bind_conf->settings.bind_proc || bind_conf->settings.bind_proc & (1UL << proc)) {
						global.tune.options |= GTUNE_SOCKET_TRANSFER;
						break;
					}
				}
			}
		}

		/* we might have to unbind some proxies from some processes */
		px = proxies_list;
		while (px != NULL) {
			if (px->bind_proc && !px->disabled) {
				if (!(px->bind_proc & (1UL << proc)))
					stop_proxy(px);
			}
			px = px->next;
		}

		/* we might have to unbind some log forward proxies from some processes */
		px = cfg_log_forward;
		while (px != NULL) {
			if (px->bind_proc && !px->disabled) {
				if (!(px->bind_proc & (1UL << proc)))
					stop_proxy(px);
			}
			px = px->next;
		}

		/* we might have to unbind some peers sections from some processes */
		for (curpeers = cfg_peers; curpeers; curpeers = curpeers->next) {
			if (!curpeers->peers_fe)
				continue;

			if (curpeers->peers_fe->bind_proc & (1UL << proc))
				continue;

			stop_proxy(curpeers->peers_fe);
			/* disable this peer section so that it kills itself */
			signal_unregister_handler(curpeers->sighandler);
			task_destroy(curpeers->sync_task);
			curpeers->sync_task = NULL;
			task_destroy(curpeers->peers_fe->task);
			curpeers->peers_fe->task = NULL;
			curpeers->peers_fe = NULL;
		}

		/*
		 * This is only done in daemon mode because we might want the
		 * logs on stdout in mworker mode. If we're NOT in QUIET mode,
		 * we should now close the 3 first FDs to ensure that we can
		 * detach from the TTY. We MUST NOT do it in other cases since
		 * it would have already be done, and 0-2 would have been
		 * affected to listening sockets
		 */
		if ((global.mode & MODE_DAEMON) &&
		    (!(global.mode & MODE_QUIET) || (global.mode & MODE_VERBOSE))) {
			/* detach from the tty */
			stdio_quiet(devnullfd);
			global.mode &= ~MODE_VERBOSE;
			global.mode |= MODE_QUIET; /* ensure that we won't say anything from now */
		}
		pid = getpid(); /* update child's pid */
		if (!(global.mode & MODE_MWORKER)) /* in mworker mode we don't want a new pgid for the children */
			setsid();
		fork_poller();
	}

	/* try our best to re-enable core dumps depending on system capabilities.
	 * What is addressed here :
	 *   - remove file size limits
	 *   - remove core size limits
	 *   - mark the process dumpable again if it lost it due to user/group
	 */
	if (global.tune.options & GTUNE_SET_DUMPABLE) {
		limit.rlim_cur = limit.rlim_max = RLIM_INFINITY;

#if defined(RLIMIT_FSIZE)
		if (setrlimit(RLIMIT_FSIZE, &limit) == -1) {
			if (global.tune.options & GTUNE_STRICT_LIMITS) {
				ha_alert("[%s.main()] Failed to set the raise the maximum "
					 "file size.\n", argv[0]);
				if (!(global.mode & MODE_MWORKER))
					exit(1);
			}
			else
				ha_warning("[%s.main()] Failed to set the raise the maximum "
					   "file size.\n", argv[0]);
		}
#endif

#if defined(RLIMIT_CORE)
		if (setrlimit(RLIMIT_CORE, &limit) == -1) {
			if (global.tune.options & GTUNE_STRICT_LIMITS) {
				ha_alert("[%s.main()] Failed to set the raise the core "
					 "dump size.\n", argv[0]);
				if (!(global.mode & MODE_MWORKER))
					exit(1);
			}
			else
				ha_warning("[%s.main()] Failed to set the raise the core "
					   "dump size.\n", argv[0]);
		}
#endif

#if defined(USE_PRCTL)
		if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) == -1)
			ha_warning("[%s.main()] Failed to set the dumpable flag, "
				   "no core will be dumped.\n", argv[0]);
#endif
	}

	global.mode &= ~MODE_STARTING;
	/*
	 * That's it : the central polling loop. Run until we stop.
	 */
#ifdef USE_THREAD
	{
		sigset_t     blocked_sig, old_sig;
		int          i;

		/* ensure the signals will be blocked in every thread */
		sigfillset(&blocked_sig);
		sigdelset(&blocked_sig, SIGPROF);
		sigdelset(&blocked_sig, SIGBUS);
		sigdelset(&blocked_sig, SIGFPE);
		sigdelset(&blocked_sig, SIGILL);
		sigdelset(&blocked_sig, SIGSEGV);
		pthread_sigmask(SIG_SETMASK, &blocked_sig, &old_sig);

		/* Create nbthread-1 thread. The first thread is the current process */
		ha_thread_info[0].pthread = pthread_self();
		for (i = 1; i < global.nbthread; i++)
			pthread_create(&ha_thread_info[i].pthread, NULL, &run_thread_poll_loop, (void *)(long)i);

#ifdef USE_CPU_AFFINITY
		/* Now the CPU affinity for all threads */
		if (global.cpu_map.proc_t1[relative_pid-1])
			global.cpu_map.thread[0] &= global.cpu_map.proc_t1[relative_pid-1];

		for (i = 0; i < global.nbthread; i++) {
			if (global.cpu_map.proc[relative_pid-1])
				global.cpu_map.thread[i] &= global.cpu_map.proc[relative_pid-1];

			if (i < MAX_THREADS &&       /* only the first 32/64 threads may be pinned */
			    global.cpu_map.thread[i]) {/* only do this if the thread has a THREAD map */
#if defined(__APPLE__)
				int j;
				unsigned long cpu_map = global.cpu_map.thread[i];

				while ((j = ffsl(cpu_map)) > 0) {
					thread_affinity_policy_data_t cpu_set = { j - 1 };
					thread_port_t mthread = pthread_mach_thread_np(ha_thread_info[i].pthread);
					thread_policy_set(mthread, THREAD_AFFINITY_POLICY, (thread_policy_t)&cpu_set, 1);
					cpu_map &= ~(1UL << (j - 1));
				}
#else
#if defined(__FreeBSD__) || defined(__NetBSD__)
				cpuset_t cpuset;
#else
				cpu_set_t cpuset;
#endif
				int j;
				unsigned long cpu_map = global.cpu_map.thread[i];

				CPU_ZERO(&cpuset);

				while ((j = ffsl(cpu_map)) > 0) {
					CPU_SET(j - 1, &cpuset);
					cpu_map &= ~(1UL << (j - 1));
				}
				pthread_setaffinity_np(ha_thread_info[i].pthread,
						       sizeof(cpuset), &cpuset);
#endif
			}
		}
#endif /* !USE_CPU_AFFINITY */

		/* when multithreading we need to let only the thread 0 handle the signals */
		haproxy_unblock_signals();

		/* Finally, start the poll loop for the first thread */
		run_thread_poll_loop(0);

		/* Wait the end of other threads */
		for (i = 1; i < global.nbthread; i++)
			pthread_join(ha_thread_info[i].pthread, NULL);

#if defined(DEBUG_THREAD) || defined(DEBUG_FULL)
		show_lock_stats();
#endif
	}
#else /* ! USE_THREAD */
	haproxy_unblock_signals();
	run_thread_poll_loop(0);
#endif

	deinit_and_exit(0);
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
