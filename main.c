
/*
 * Copyright 2011 Con Kolivas
 * Copyright 2010 Jeff Garzik
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"

#include <curses.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>
#include <assert.h>
#include <signal.h>
#ifndef WIN32
#include <sys/resource.h>
#endif
#include <ccan/opt/opt.h>
#include <jansson.h>
#include <curl/curl.h>
#include <libgen.h>

#include "compat.h"
#include "miner.h"
#include "findnonce.h"
#include "bench_block.h"
#include "ocl.h"
#include "uthash.h"
#include "adl.h"

#if defined(unix)
	#include <errno.h>
	#include <fcntl.h>
	#include <unistd.h>
	#include <sys/wait.h>
	#include <sys/types.h>
#endif

#ifdef __linux /* Linux specific policy and affinity management */
#include <sched.h>
static inline void drop_policy(void)
{
	struct sched_param param;

#ifdef SCHED_BATCH
#ifdef SCHED_IDLE
	if (unlikely(sched_setscheduler(0, SCHED_IDLE, &param) == -1))
#endif
		sched_setscheduler(0, SCHED_BATCH, &param);
#endif
}

static inline void affine_to_cpu(int id, int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	sched_setaffinity(0, sizeof(&set), &set);
	applog(LOG_INFO, "Binding cpu mining thread %d to cpu %d", id, cpu);
}
#else
static inline void drop_policy(void)
{
}

static inline void affine_to_cpu(int id, int cpu)
{
}
#endif
		
enum workio_commands {
	WC_GET_WORK,
	WC_SUBMIT_WORK,
};

struct workio_cmd {
	enum workio_commands	cmd;
	struct thr_info		*thr;
	union {
		struct work	*work;
	} u;
	bool			lagging;
};

enum sha256_algos {
	ALGO_C,			/* plain C */
	ALGO_4WAY,		/* parallel SSE2 */
	ALGO_VIA,		/* VIA padlock */
	ALGO_CRYPTOPP,		/* Crypto++ (C) */
	ALGO_CRYPTOPP_ASM32,	/* Crypto++ 32-bit assembly */
	ALGO_SSE2_32,		/* SSE2 for x86_32 */
	ALGO_SSE2_64,		/* SSE2 for x86_64 */
	ALGO_SSE4_64,		/* SSE4 for x86_64 */
};

enum pool_strategy {
	POOL_FAILOVER,
	POOL_ROUNDROBIN,
	POOL_ROTATE,
	POOL_LOADBALANCE,
};

#define TOP_STRATEGY (POOL_LOADBALANCE)

struct strategies {
	const char *s;
} strategies[] = {
	{ "Failover" },
	{ "Round Robin" },
	{ "Rotate" },
	{ "Load Balance" },
};

static size_t max_name_len = 0;
static char *name_spaces_pad = NULL;
static const char *algo_names[] = {
	[ALGO_C]		= "c",
#ifdef WANT_SSE2_4WAY
	[ALGO_4WAY]		= "4way",
#endif
#ifdef WANT_VIA_PADLOCK
	[ALGO_VIA]		= "via",
#endif
	[ALGO_CRYPTOPP]		= "cryptopp",
#ifdef WANT_CRYPTOPP_ASM32
	[ALGO_CRYPTOPP_ASM32]	= "cryptopp_asm32",
#endif
#ifdef WANT_X8632_SSE2
	[ALGO_SSE2_32]		= "sse2_32",
#endif
#ifdef WANT_X8664_SSE2
	[ALGO_SSE2_64]		= "sse2_64",
#endif
#ifdef WANT_X8664_SSE4
	[ALGO_SSE4_64]		= "sse4_64",
#endif
};

typedef void (*sha256_func)();
static const sha256_func sha256_funcs[] = {
	[ALGO_C]		= (sha256_func)scanhash_c,
#ifdef WANT_SSE2_4WAY
	[ALGO_4WAY]		= (sha256_func)ScanHash_4WaySSE2,
#endif
#ifdef WANT_VIA_PADLOCK
	[ALGO_VIA]		= (sha256_func)scanhash_via,
#endif
	[ALGO_CRYPTOPP]		=  (sha256_func)scanhash_cryptopp,
#ifdef WANT_CRYPTOPP_ASM32
	[ALGO_CRYPTOPP_ASM32]	= (sha256_func)scanhash_asm32,
#endif
#ifdef WANT_X8632_SSE2
	[ALGO_SSE2_32]		= (sha256_func)scanhash_sse2_32,
#endif
#ifdef WANT_X8664_SSE2
	[ALGO_SSE2_64]		= (sha256_func)scanhash_sse2_64,
#endif
#ifdef WANT_X8664_SSE4
	[ALGO_SSE4_64]		= (sha256_func)scanhash_sse4_64
#endif
};

static char packagename[255];

bool opt_debug = false;
bool opt_protocol = false;
static bool want_longpoll = true;
static bool have_longpoll = false;
static bool want_per_device_stats = false;
bool use_syslog = false;
static bool opt_quiet = false;
static bool opt_realquiet = false;
static bool opt_loginput = false;
static int opt_retries = -1;
static int opt_fail_pause = 5;
static int fail_pause = 5;
static int opt_log_interval = 5;
bool opt_log_output = false;
static bool opt_dynamic = true;
static int opt_queue = 1;
int opt_vectors;
int opt_worksize;
int opt_scantime = 60;
int opt_bench_algo = -1;
static const bool opt_time = true;
static bool opt_restart = true;
#if defined(WANT_X8664_SSE2) && defined(__SSE2__)
static enum sha256_algos opt_algo = ALGO_SSE2_64;
#elif defined(WANT_X8632_SSE2) && defined(__SSE2__)
static enum sha256_algos opt_algo = ALGO_SSE2_32;
#else
static enum sha256_algos opt_algo = ALGO_C;
#endif
static int nDevs;
static int opt_g_threads = 2;
static int opt_device;
static int total_devices;
static bool gpu_devices[MAX_GPUDEVICES];
static int gpu_threads;
static bool forced_n_threads;
static int opt_n_threads;
static int mining_threads;
static int num_processors;
static int scan_intensity;
static bool use_curses = true;
static bool opt_submit_stale;
static bool opt_nogpu;
static bool opt_usecpu;
static int opt_shares;
static bool opt_fail_only;
bool opt_autofan;
bool opt_autoengine;
bool opt_noadl;

char *opt_kernel_path;
char *cgminer_path;

#define QUIET	(opt_quiet || opt_realquiet)

struct thr_info *thr_info;
static int work_thr_id;
int longpoll_thr_id;
static int stage_thr_id;
static int watchdog_thr_id;
static int input_thr_id;
static int gpur_thr_id;
static int cpur_thr_id;
static int total_threads;

struct work_restart *work_restart = NULL;

static pthread_mutex_t hash_lock;
static pthread_mutex_t qd_lock;
static pthread_mutex_t *stgd_lock;
static pthread_mutex_t curses_lock;
static pthread_rwlock_t blk_lock;
static double total_mhashes_done;
static struct timeval total_tv_start, total_tv_end;

pthread_mutex_t control_lock;

int hw_errors;
static int total_accepted, total_rejected;
static int total_getworks, total_stale, total_discarded;
static int total_queued;
static unsigned int new_blocks;

enum block_change {
	BLOCK_NONE,
	BLOCK_LP,
	BLOCK_DETECT,
	BLOCK_FIRST,
};

static enum block_change block_changed = BLOCK_FIRST;
static unsigned int local_work;
static unsigned int total_go, total_ro;

#define MAX_POOLS (32)

static struct pool *pools[MAX_POOLS];
static struct pool *currentpool = NULL;
static int total_pools;
static enum pool_strategy pool_strategy = POOL_FAILOVER;
static int opt_rotate_period;
static int total_urls, total_users, total_passes, total_userpasses;

static bool curses_active = false;

static char current_block[37];
static char *current_hash;
static char datestamp[40];
static char blocktime[30];

struct block {
	char hash[37];
	UT_hash_handle hh;
};

static struct block *blocks = NULL;

static char *opt_kernel = NULL;

#if defined(unix)
	static char *opt_stderr_cmd = NULL;
#endif // defined(unix)

enum cl_kernel chosen_kernel;

static bool ping = true;

struct sigaction termhandler, inthandler;

struct thread_q *getq;

static int total_work;
struct work *staged_work = NULL;
static int staged_clones;

struct schedtime {
	bool enable;
	struct tm tm;
};

struct schedtime schedstart;
struct schedtime schedstop;
bool sched_paused;

static bool time_before(struct tm *tm1, struct tm *tm2)
{
	if (tm1->tm_hour < tm2->tm_hour)
		return true;
	if (tm1->tm_hour == tm2->tm_hour && tm1->tm_min < tm2->tm_min)
		return true;
	return false;
}

static bool should_run(void)
{
	struct timeval tv;
	struct tm tm;

	if (!schedstart.enable && !schedstop.enable)
		return true;

	gettimeofday(&tv, NULL);
	localtime_r(&tv.tv_sec, &tm);
	if (schedstart.enable) {
		if (!schedstop.enable) {
			if (time_before(&tm, &schedstart.tm))
				return false;

			/* This is a once off event with no stop time set */
			schedstart.enable = false;
			return true;
		}
		if (time_before(&schedstart.tm, &schedstop.tm)) {
			if (time_before(&tm, &schedstop.tm) && !time_before(&tm, &schedstart.tm))
				return true;
			return false;
		} /* Times are reversed */
		if (time_before(&tm, &schedstart.tm)) {
			if (time_before(&tm, &schedstop.tm))
				return true;
			return false;
		}
		return true;
	}
	/* only schedstop.enable == true */
	if (!time_before(&tm, &schedstop.tm))
		return false;
	return true;
}

void get_datestamp(char *f, struct timeval *tv)
{
	struct tm tm;

	localtime_r(&tv->tv_sec, &tm);
	sprintf(f, "[%d-%02d-%02d %02d:%02d:%02d]",
		tm.tm_year + 1900,
		tm.tm_mon + 1,
		tm.tm_mday,
		tm.tm_hour,
		tm.tm_min,
		tm.tm_sec);
}

void get_timestamp(char *f, struct timeval *tv)
{
	struct tm tm;

	localtime_r(&tv->tv_sec, &tm);
	sprintf(f, "[%02d:%02d:%02d]",
		tm.tm_hour,
		tm.tm_min,
		tm.tm_sec);
}

static void applog_and_exit(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vapplog(LOG_ERR, fmt, ap);
	va_end(ap);
	exit(1);
}

static void add_pool(void)
{
	struct pool *pool;

	pool = calloc(sizeof(struct pool), 1);
	if (!pool) {
		applog(LOG_ERR, "Failed to malloc pool in add_pool");
		exit (1);
	}
	pool->pool_no = pool->prio = total_pools;
	pools[total_pools++] = pool;
	if (unlikely(pthread_mutex_init(&pool->pool_lock, NULL))) {
		applog(LOG_ERR, "Failed to pthread_mutex_init in add_pool");
		exit (1);
	}
	/* Make sure the pool doesn't think we've been idle since time 0 */
	pool->tv_idle.tv_sec = ~0UL;
}

/* Pool variant of test and set */
static bool pool_tset(struct pool *pool, bool *var)
{
	bool ret;

	mutex_lock(&pool->pool_lock);
	ret = *var;
	*var = true;
	mutex_unlock(&pool->pool_lock);
	return ret;
}

static bool pool_tclear(struct pool *pool, bool *var)
{
	bool ret;

	mutex_lock(&pool->pool_lock);
	ret = *var;
	*var = false;
	mutex_unlock(&pool->pool_lock);
	return ret;
}

static struct pool *current_pool(void)
{
	struct pool *pool;

	mutex_lock(&control_lock);
	pool = currentpool;
	mutex_unlock(&control_lock);
	return pool;
}

// Algo benchmark, crash-prone, system independent stage
static double bench_algo_stage3(
	enum sha256_algos algo
)
{
	// Use a random work block pulled from a pool
	static uint8_t bench_block[] = { CGMINER_BENCHMARK_BLOCK };
	struct work work __attribute__((aligned(128)));

	size_t bench_size = sizeof(work);
	size_t work_size = sizeof(bench_block);
	size_t min_size = (work_size < bench_size ? work_size : bench_size);
	memset(&work, 0, sizeof(work));
	memcpy(&work, &bench_block, min_size);

	struct work_restart dummy;
	work_restart = &dummy;

	struct timeval end;
	struct timeval start;
	uint32_t max_nonce = (1<<22);
	unsigned long hashes_done = 0;

	gettimeofday(&start, 0);
		#if defined(WANT_VIA_PADLOCK)

			// For some reason, the VIA padlock hasher has a different API ...
			if (ALGO_VIA==algo) {
				(void)scanhash_via(
					0,
					work.data,
					work.target,
					max_nonce,
					&hashes_done,
					work.blk.nonce
				);
			} else
		#endif
			{
				sha256_func func = sha256_funcs[algo];
				(*func)(
					0,
					work.midstate,
					work.data + 64,
					work.hash1,
					work.hash,
					work.target,
					max_nonce,
					&hashes_done,
					work.blk.nonce
				);
			}
	gettimeofday(&end, 0);
	work_restart = NULL;

	uint64_t usec_end = ((uint64_t)end.tv_sec)*1000*1000 + end.tv_usec;
	uint64_t usec_start = ((uint64_t)start.tv_sec)*1000*1000 + start.tv_usec;
	uint64_t usec_elapsed = usec_end - usec_start;

	double rate = -1.0;
	if (0<usec_elapsed) {
		rate = (1.0*hashes_done)/usec_elapsed;
	}
	return rate;
}

#if defined(unix)

	// Change non-blocking status on a file descriptor
	static void set_non_blocking(
		int fd,
		int yes
	)
	{
		int flags = fcntl(fd, F_GETFL, 0);
		if (flags<0) {
			perror("fcntl(GET) failed");
			exit(1);
		}
		flags = yes ? (flags|O_NONBLOCK) : (flags&~O_NONBLOCK);

		int r = fcntl(fd, F_SETFL, flags);
		if (r<0) {
			perror("fcntl(SET) failed");
			exit(1);
		}
	}

#endif // defined(unix)

// Algo benchmark, crash-safe, system-dependent stage
static double bench_algo_stage2(
	enum sha256_algos algo
)
{
	// Here, the gig is to safely run a piece of code that potentially
	// crashes. Unfortunately, the Right Way (tm) to do this is rather
	// heavily platform dependent :(

	double rate = -1.23457;

	#if defined(unix)

		// Make a pipe: [readFD, writeFD]
		int pfd[2];
		int r = pipe(pfd);
		if (r<0) {
			perror("pipe - failed to create pipe for --algo auto");
			exit(1);
		}

		// Make pipe non blocking
		set_non_blocking(pfd[0], 1);
		set_non_blocking(pfd[1], 1);

		// Don't allow a crashing child to kill the main process
		sighandler_t sr0 = signal(SIGPIPE, SIG_IGN);
		sighandler_t sr1 = signal(SIGPIPE, SIG_IGN);
		if (SIG_ERR==sr0 || SIG_ERR==sr1) {
			perror("signal - failed to edit signal mask for --algo auto");
			exit(1);
		}

		// Fork a child to do the actual benchmarking
		pid_t child_pid = fork();
		if (child_pid<0) {
			perror("fork - failed to create a child process for --algo auto");
			exit(1);
		}

		// Do the dangerous work in the child, knowing we might crash
		if (0==child_pid) {

			// TODO: some umask trickery to prevent coredumps

			// Benchmark this algorithm
			double r = bench_algo_stage3(algo);

			// We survived, send result to parent and bail
			int loop_count = 0;
			while (1) {
				ssize_t bytes_written = write(pfd[1], &r, sizeof(r));
				int try_again = (0==bytes_written || (bytes_written<0 && EAGAIN==errno));
				int success = (sizeof(r)==(size_t)bytes_written);

				if (success)
					break;

				if (!try_again) {
					perror("write - child failed to write benchmark result to pipe");
					exit(1);
				}

				if (5<loop_count) {
					applog(LOG_ERR, "child tried %d times to communicate with parent, giving up", loop_count);
					exit(1);
				}
				++loop_count;
				sleep(1);
			}
			exit(0);
		}

		// Parent waits for a result from child
		int loop_count = 0;
		while (1) {

			// Wait for child to die
			int status;
			int r = waitpid(child_pid, &status, WNOHANG);
			if ((child_pid==r) || (r<0 && ECHILD==errno)) {

				// Child died somehow. Grab result and bail
				double tmp;
				ssize_t bytes_read = read(pfd[0], &tmp, sizeof(tmp));
				if (sizeof(tmp)==(size_t)bytes_read)
					rate = tmp;
				break;

			} else if (r<0) {
				perror("bench_algo: waitpid failed. giving up.");
				exit(1);
			}

			// Give up on child after a ~60s
			if (60<loop_count) {
				kill(child_pid, SIGKILL);
				waitpid(child_pid, &status, 0);
				break;
			}

			// Wait a bit longer
			++loop_count;
			sleep(1);
		}

		// Close pipe
		r = close(pfd[0]);
		if (r<0) {
			perror("close - failed to close read end of pipe for --algo auto");
			exit(1);
		}
		r = close(pfd[1]);
		if (r<0) {
			perror("close - failed to close read end of pipe for --algo auto");
			exit(1);
		}

	#elif defined(WIN32)

		// Get handle to current exe
		HINSTANCE module = GetModuleHandle(0);
		if (!module) {
			applog(LOG_ERR, "failed to retrieve module handle");
			exit(1);
		}

		// Create a unique name
		char unique_name[32];
		snprintf(
			unique_name,
			sizeof(unique_name)-1,
			"cgminer-%p",
			(void*)module
		);

		// Create and init a chunked of shared memory
		HANDLE map_handle = CreateFileMapping( 
			INVALID_HANDLE_VALUE,   // use paging file
			NULL,                   // default security attributes
			PAGE_READWRITE,         // read/write access
			0,                      // size: high 32-bits
			4096,			// size: low 32-bits
			unique_name		// name of map object
		);
		if (NULL==map_handle) {
			applog(LOG_ERR, "could not create shared memory");
			exit(1);
		}

		void *shared_mem = MapViewOfFile( 
			map_handle,	// object to map view of
			FILE_MAP_WRITE, // read/write access
			0,              // high offset:  map from
			0,              // low offset:   beginning
			0		// default: map entire file
		);
		if (NULL==shared_mem) {
			applog(LOG_ERR, "could not map shared memory");
			exit(1);
		}
		SetEnvironmentVariable("CGMINER_SHARED_MEM", unique_name);
		CopyMemory(shared_mem, &rate, sizeof(rate));

		// Get path to current exe
		char cmd_line[256 + MAX_PATH];
		const size_t n = sizeof(cmd_line)-200;
		DWORD size = GetModuleFileName(module, cmd_line, n);
		if (0==size) {
			applog(LOG_ERR, "failed to retrieve module path");
			exit(1);
		}

		// Construct new command line based on that
		char *p = strlen(cmd_line) + cmd_line;
		sprintf(p, " --bench-algo %d", algo);
		SetEnvironmentVariable("CGMINER_BENCH_ALGO", "1");

		// Launch a debug copy of cgminer
		STARTUPINFO startup_info;
		PROCESS_INFORMATION process_info;
		ZeroMemory(&startup_info, sizeof(startup_info));
		ZeroMemory(&process_info, sizeof(process_info));
		startup_info.cb = sizeof(startup_info);

		BOOL ok = CreateProcess(
			NULL,			// No module name (use command line)
			cmd_line,		// Command line
			NULL,			// Process handle not inheritable
			NULL,			// Thread handle not inheritable
			FALSE,			// Set handle inheritance to FALSE
			DEBUG_ONLY_THIS_PROCESS,// We're going to debug the child
			NULL,			// Use parent's environment block
			NULL,			// Use parent's starting directory 
			&startup_info,		// Pointer to STARTUPINFO structure
			&process_info		// Pointer to PROCESS_INFORMATION structure
		);
		if (!ok) {
			applog(LOG_ERR, "CreateProcess failed with error %d\n", GetLastError() );
			exit(1);
		}

		// Debug the child (only clean way to catch exceptions)
		while (1) {

			// Wait for child to do something
			DEBUG_EVENT debug_event;
			ZeroMemory(&debug_event, sizeof(debug_event));

			BOOL ok = WaitForDebugEvent(&debug_event, 60 * 1000);
			if (!ok)
				break;

			// Decide if event is "normal"
			int go_on =
				CREATE_PROCESS_DEBUG_EVENT== debug_event.dwDebugEventCode	||
				CREATE_THREAD_DEBUG_EVENT == debug_event.dwDebugEventCode	||
				EXIT_THREAD_DEBUG_EVENT   == debug_event.dwDebugEventCode	||
				EXCEPTION_DEBUG_EVENT     == debug_event.dwDebugEventCode	||
				LOAD_DLL_DEBUG_EVENT      == debug_event.dwDebugEventCode	||
				OUTPUT_DEBUG_STRING_EVENT == debug_event.dwDebugEventCode	||
				UNLOAD_DLL_DEBUG_EVENT    == debug_event.dwDebugEventCode;
			if (!go_on)
				break;

			// Some exceptions are also "normal", apparently.
			if (EXCEPTION_DEBUG_EVENT== debug_event.dwDebugEventCode) {

				int go_on =
					EXCEPTION_BREAKPOINT== debug_event.u.Exception.ExceptionRecord.ExceptionCode;
				if (!go_on)
					break;
			}

			// If nothing unexpected happened, let child proceed
			ContinueDebugEvent(
				debug_event.dwProcessId,
				debug_event.dwThreadId,
				DBG_CONTINUE
			);
		}

		// Clean up child process
		TerminateProcess(process_info.hProcess, 1);
		CloseHandle(process_info.hProcess);
		CloseHandle(process_info.hThread);

		// Reap return value and cleanup
		CopyMemory(&rate, shared_mem, sizeof(rate));
		(void)UnmapViewOfFile(shared_mem); 
		(void)CloseHandle(map_handle); 

	#else

		// Not linux, not unix, not WIN32 ... do our best
		rate = bench_algo_stage3(algo);

	#endif // defined(unix)

	// Done
	return rate;
}

static void bench_algo(
	double            *best_rate,
	enum sha256_algos *best_algo,
	enum sha256_algos algo
)
{
	size_t n = max_name_len - strlen(algo_names[algo]);
	memset(name_spaces_pad, ' ', n);
	name_spaces_pad[n] = 0;

	applog(
		LOG_ERR,
		"\"%s\"%s : benchmarking algorithm ...",
		algo_names[algo],
		name_spaces_pad
	);

	double rate = bench_algo_stage2(algo);
	if (rate<0.0) {
		applog(
			LOG_ERR,
			"\"%s\"%s : algorithm fails on this platform",
			algo_names[algo],
			name_spaces_pad
		);
	} else {
		applog(
			LOG_ERR,
			"\"%s\"%s : algorithm runs at %.5f MH/s",
			algo_names[algo],
			name_spaces_pad,
			rate
		);
		if (*best_rate<rate) {
			*best_rate = rate;
			*best_algo = algo;
		}
	}
}

// Figure out the longest algorithm name
static void init_max_name_len()
{
	size_t i;
	size_t nb_names = sizeof(algo_names)/sizeof(algo_names[0]);
	for (i=0; i<nb_names; ++i) {
		const char *p = algo_names[i];
		size_t name_len = p ? strlen(p) : 0;
		if (max_name_len<name_len)
			max_name_len = name_len;
	}

	name_spaces_pad = (char*) malloc(max_name_len+16);
	if (0==name_spaces_pad) {
		perror("malloc failed");
		exit(1);
	}
}

// Pick the fastest CPU hasher
static enum sha256_algos pick_fastest_algo()
{
	double best_rate = -1.0;
	enum sha256_algos best_algo = 0;
	applog(LOG_ERR, "benchmarking all sha256 algorithms ...");

	bench_algo(&best_rate, &best_algo, ALGO_C);

	#if defined(WANT_SSE2_4WAY)
		bench_algo(&best_rate, &best_algo, ALGO_4WAY);
	#endif

	#if defined(WANT_VIA_PADLOCK)
		bench_algo(&best_rate, &best_algo, ALGO_VIA);
	#endif

	bench_algo(&best_rate, &best_algo, ALGO_CRYPTOPP);

	#if defined(WANT_CRYPTOPP_ASM32)
		bench_algo(&best_rate, &best_algo, ALGO_CRYPTOPP_ASM32);
	#endif

	#if defined(WANT_X8632_SSE2)
		bench_algo(&best_rate, &best_algo, ALGO_SSE2_32);
	#endif

	#if defined(WANT_X8664_SSE2)
		bench_algo(&best_rate, &best_algo, ALGO_SSE2_64);
	#endif

	#if defined(WANT_X8664_SSE4)
		bench_algo(&best_rate, &best_algo, ALGO_SSE4_64);
	#endif

	size_t n = max_name_len - strlen(algo_names[best_algo]);
	memset(name_spaces_pad, ' ', n);
	name_spaces_pad[n] = 0;
	applog(
		LOG_ERR,
		"\"%s\"%s : is fastest algorithm at %.5f MH/s",
		algo_names[best_algo],
		name_spaces_pad,
		best_rate
	);
	return best_algo;
}

/* FIXME: Use asprintf for better errors. */
static char *set_algo(const char *arg, enum sha256_algos *algo)
{
	enum sha256_algos i;

	if (!strcmp(arg, "auto")) {
		*algo = pick_fastest_algo();
		return NULL;
	}

	for (i = 0; i < ARRAY_SIZE(algo_names); i++) {
		if (algo_names[i] && !strcmp(arg, algo_names[i])) {
			*algo = i;
			return NULL;
		}
	}
	return "Unknown algorithm";
}

static void show_algo(char buf[OPT_SHOW_LEN], const enum sha256_algos *algo)
{
	strncpy(buf, algo_names[*algo], OPT_SHOW_LEN);
}

static char *set_int_range(const char *arg, int *i, int min, int max)
{
	char *err = opt_set_intval(arg, i);
	if (err)
		return err;

	if (*i < min || *i > max)
		return "Value out of range";

	return NULL;
}

static char *set_int_0_to_9999(const char *arg, int *i)
{
	return set_int_range(arg, i, 0, 9999);
}

static char *forced_int_1010(const char *arg, int *i)
{
	opt_dynamic = false;
	return set_int_range(arg, i, -10, 10);
}

static char *force_nthreads_int(const char *arg, int *i)
{
	forced_n_threads = true;
	return set_int_range(arg, i, 0, 9999);
}

static char *set_int_0_to_10(const char *arg, int *i)
{
	return set_int_range(arg, i, 0, 10);
}

static char *set_int_1_to_10(const char *arg, int *i)
{
	return set_int_range(arg, i, 1, 10);
}

static char *set_devices(const char *arg, int *i)
{
	char *err = opt_set_intval(arg, i);

	if (err)
		return err;

	if (*i < 0 || *i > 15)
		return "Invalid GPU device number";
	total_devices++;
	gpu_devices[*i] = true;
	return NULL;
}

static char *set_loadbalance(enum pool_strategy *strategy)
{
	*strategy = POOL_LOADBALANCE;
	return NULL;
}

static char *set_rotate(const char *arg, int *i)
{
	pool_strategy = POOL_ROTATE;
	return set_int_range(arg, i, 0, 9999);
}

static char *set_rr(enum pool_strategy *strategy)
{
	*strategy = POOL_ROUNDROBIN;
	return NULL;
}

static char *set_url(char *arg)
{
	struct pool *pool;

	total_urls++;
	if (total_urls > total_pools)
		add_pool();
	pool = pools[total_urls - 1];

	opt_set_charp(arg, &pool->rpc_url);
	if (strncmp(arg, "http://", 7) &&
	    strncmp(arg, "https://", 8)) {
		char *httpinput;

		httpinput = malloc(255);
		if (!httpinput)
			quit(1, "Failed to malloc httpinput");
		strcpy(httpinput, "http://");
		strncat(httpinput, arg, 248);
		pool->rpc_url = httpinput;
	}

	return NULL;
}

static char *set_user(const char *arg)
{
	struct pool *pool;

	if (total_userpasses)
		return "Use only user + pass or userpass, but not both";
	total_users++;
	if (total_users > total_pools)
		add_pool();

	pool = pools[total_users - 1];
	opt_set_charp(arg, &pool->rpc_user);

	return NULL;
}

static char *set_pass(const char *arg)
{
	struct pool *pool;

	if (total_userpasses)
		return "Use only user + pass or userpass, but not both";
	total_passes++;
	if (total_passes > total_pools)
		add_pool();

	pool = pools[total_passes - 1];
	opt_set_charp(arg, &pool->rpc_pass);

	return NULL;
}

static char *set_userpass(const char *arg)
{
	struct pool *pool;

	if (total_users || total_passes)
		return "Use only user + pass or userpass, but not both";
	total_userpasses++;
	if (total_userpasses > total_pools)
		add_pool();

	pool = pools[total_userpasses - 1];
	opt_set_charp(arg, &pool->rpc_userpass);

	return NULL;
}

static char *set_vector(const char *arg, int *i)
{
	char *err = opt_set_intval(arg, i);
	if (err)
		return err;

	if (*i != 1 && *i != 2 && *i != 4)
		return "Valid vectors are 1, 2 or 4";
	return NULL;
}

static char *enable_debug(bool *flag)
{
	*flag = true;
	/* Turn out verbose output, too. */
	opt_log_output = true;
	return NULL;
}

static char *set_schedtime(const char *arg, struct schedtime *st)
{
	if (sscanf(arg, "%d:%d", &st->tm.tm_hour, &st->tm.tm_min) != 2)
		return "Invalid time set, should be HH:MM";
	if (st->tm.tm_hour > 23 || st->tm.tm_min > 59 || st->tm.tm_hour < 0 || st->tm.tm_min < 0)
		return "Invalid time set.";
	st->enable = true;
	return NULL;
}

#ifdef HAVE_ADL
static void get_intrange(char *arg, int *val1, int *val2)
{
	if (sscanf(arg, "%d-%d", val1, val2) == 1) {
		*val2 = *val1;
		*val1 = 0;
	}
}

static char *set_gpu_engine(char *arg)
{
	int i, val1 = 0, val2 = 0, device = 0;
	char *nextptr;

	nextptr = strtok(arg, ",");
	if (nextptr == NULL)
		return "Invalid parameters for set gpu engine";
	get_intrange(nextptr, &val1, &val2);
	if (val1 < 0 || val1 > 9999 || val2 <= 0 || val2 > 9999)
		return "Invalid value passed to set_gpu_engine";

	gpus[device].min_engine = val1;
	gpus[device].gpu_engine = val2;
	device++;

	while ((nextptr = strtok(NULL, ",")) != NULL) {
		get_intrange(nextptr, &val1, &val2);
		if (val1 < 0 || val1 > 9999 || val2 <= 0 || val2 > 9999)
			return "Invalid value passed to set_gpu_engine";
		gpus[device].min_engine = val1;
		gpus[device].gpu_engine = val2;
		device++;
	}

	if (device == 1) {
		for (i = 1; i < MAX_GPUDEVICES; i++) {
			gpus[i].min_engine = gpus[0].min_engine;
			gpus[i].gpu_engine = gpus[0].gpu_engine;
		}
	}

	return NULL;
}

static char *set_gpu_fan(char *arg)
{
	int i, val1 = 0, val2 = 0, device = 0;
	char *nextptr;

	nextptr = strtok(arg, ",");
	if (nextptr == NULL)
		return "Invalid parameters for set gpu fan";
	get_intrange(nextptr, &val1, &val2);
	if (val1 < 0 || val1 > 100 || val2 < 0 || val2 > 100)
		return "Invalid value passed to set_gpu_fan";

	gpus[device].min_fan = val1;
	gpus[device].gpu_fan = val2;
	device++;

	while ((nextptr = strtok(NULL, ",")) != NULL) {
		get_intrange(nextptr, &val1, &val2);
		if (val1 < 0 || val1 > 100 || val2 < 0 || val2 > 100)
			return "Invalid value passed to set_gpu_fan";

		gpus[device].min_fan = val1;
		gpus[device].gpu_fan = val2;
		device++;
	}

	if (device == 1) {
		for (i = 1; i < MAX_GPUDEVICES; i++) {
			gpus[i].min_fan = gpus[0].min_fan;
			gpus[i].gpu_fan = gpus[0].gpu_fan;
		}
	}

	return NULL;
}

static char *set_gpu_memclock(char *arg)
{
	int i, val = 0, device = 0;
	char *nextptr;

	nextptr = strtok(arg, ",");
	if (nextptr == NULL)
		return "Invalid parameters for set gpu memclock";
	val = atoi(nextptr);
	if (val <= 0 || val >= 9999)
		return "Invalid value passed to set_gpu_memclock";

	gpus[device++].gpu_memclock = val;

	while ((nextptr = strtok(NULL, ",")) != NULL) {
		val = atoi(nextptr);
		if (val <= 0 || val >= 9999)
			return "Invalid value passed to set_gpu_memclock";

		gpus[device++].gpu_memclock = val;
	}
	if (device == 1) {
		for (i = device; i < MAX_GPUDEVICES; i++)
			gpus[i].gpu_memclock = gpus[0].gpu_memclock;
	}

	return NULL;
}

static char *set_gpu_powertune(char *arg)
{
	int i, val = 0, device = 0;
	char *nextptr;

	nextptr = strtok(arg, ",");
	if (nextptr == NULL)
		return "Invalid parameters for set gpu powertune";
	val = atoi(nextptr);
	if (val < -99 || val > 99)
		return "Invalid value passed to set_gpu_powertune";

	gpus[device++].gpu_powertune = val;

	while ((nextptr = strtok(NULL, ",")) != NULL) {
		val = atoi(nextptr);
		if (val < -99 || val > 99)
			return "Invalid value passed to set_gpu_powertune";

		gpus[device++].gpu_powertune = val;
	}
	if (device == 1) {
		for (i = device; i < MAX_GPUDEVICES; i++)
			gpus[i].gpu_powertune = gpus[0].gpu_powertune;
	}

	return NULL;
}

static char *set_gpu_vddc(char *arg)
{
	int i, device = 0;
	float val = 0;
	char *nextptr;

	nextptr = strtok(arg, ",");
	if (nextptr == NULL)
		return "Invalid parameters for set gpu vddc";
	val = atof(nextptr);
	if (val <= 0 || val >= 9999)
		return "Invalid value passed to set_gpu_vddc";

	gpus[device++].gpu_vddc = val;

	while ((nextptr = strtok(NULL, ",")) != NULL) {
		val = atof(nextptr);
		if (val <= 0 || val >= 9999)
			return "Invalid value passed to set_gpu_vddc";

		gpus[device++].gpu_vddc = val;
	}
	if (device == 1) {
		for (i = device; i < MAX_GPUDEVICES; i++)
			gpus[i].gpu_vddc = gpus[0].gpu_vddc;
	}

	return NULL;
}

static char *set_temp_cutoff(char *arg)
{
	int i, val = 0, device = 0, *tco;
	char *nextptr;

	nextptr = strtok(arg, ",");
	if (nextptr == NULL)
		return "Invalid parameters for set temp cutoff";
	val = atoi(nextptr);
	if (val < 0 || val > 200)
		return "Invalid value passed to set temp cutoff";

	tco = &gpus[device++].adl.cutofftemp;
	*tco = val;

	while ((nextptr = strtok(NULL, ",")) != NULL) {
		val = atoi(nextptr);
		if (val < 0 || val > 200)
			return "Invalid value passed to set temp cutoff";

		tco = &gpus[device++].adl.cutofftemp;
		*tco = val;
	}
	if (device == 1) {
		for (i = device; i < MAX_GPUDEVICES; i++) {
			tco = &gpus[i].adl.cutofftemp;
			*tco = val;
		}
	}

	return NULL;
}

static char *set_temp_overheat(char *arg)
{
	int i, val = 0, device = 0, *to;
	char *nextptr;

	nextptr = strtok(arg, ",");
	if (nextptr == NULL)
		return "Invalid parameters for set temp overheat";
	val = atoi(nextptr);
	if (val < 0 || val > 200)
		return "Invalid value passed to set temp overheat";

	to = &gpus[device++].adl.overtemp;
	*to = val;

	while ((nextptr = strtok(NULL, ",")) != NULL) {
		val = atoi(nextptr);
		if (val < 0 || val > 200)
			return "Invalid value passed to set temp overheat";

		to = &gpus[device++].adl.overtemp;
		*to = val;
	}
	if (device == 1) {
		for (i = device; i < MAX_GPUDEVICES; i++) {
			to = &gpus[i].adl.overtemp;
			*to = val;
		}
	}

	return NULL;
}

static char *set_temp_target(char *arg)
{
	int i, val = 0, device = 0, *tt;
	char *nextptr;

	nextptr = strtok(arg, ",");
	if (nextptr == NULL)
		return "Invalid parameters for set temp target";
	val = atoi(nextptr);
	if (val < 0 || val > 200)
		return "Invalid value passed to set temp target";

	tt = &gpus[device++].adl.targettemp;
	*tt = val;

	while ((nextptr = strtok(NULL, ",")) != NULL) {
		val = atoi(nextptr);
		if (val < 0 || val > 200)
			return "Invalid value passed to set temp target";

		tt = &gpus[device++].adl.targettemp;
		*tt = val;
	}
	if (device == 1) {
		for (i = device; i < MAX_GPUDEVICES; i++) {
			tt = &gpus[i].adl.targettemp;
			*tt = val;
		}
	}

	return NULL;
}
#endif

/* These options are available from config file or commandline */
static struct opt_table opt_config_table[] = {
	OPT_WITH_ARG("--algo|-a",
		     set_algo, show_algo, &opt_algo,
		     "Specify sha256 implementation for CPU mining:\n"
		     "\tauto\t\tBenchmark at startup and pick fastest algorithm"
		     "\n\tc\t\tLinux kernel sha256, implemented in C"
#ifdef WANT_SSE2_4WAY
		     "\n\t4way\t\ttcatm's 4-way SSE2 implementation"
#endif
#ifdef WANT_VIA_PADLOCK
		     "\n\tvia\t\tVIA padlock implementation"
#endif
		     "\n\tcryptopp\tCrypto++ C/C++ implementation"
#ifdef WANT_CRYPTOPP_ASM32
		     "\n\tcryptopp_asm32\tCrypto++ 32-bit assembler implementation"
#endif
#ifdef WANT_X8632_SSE2
		     "\n\tsse2_32\t\tSSE2 32 bit implementation for i386 machines"
#endif
#ifdef WANT_X8664_SSE2
		     "\n\tsse2_64\t\tSSE2 64 bit implementation for x86_64 machines"
#endif
#ifdef WANT_X8664_SSE4
		     "\n\tsse4_64\t\tSSE4.1 64 bit implementation for x86_64 machines"
#endif
		),
#ifdef HAVE_ADL
	OPT_WITHOUT_ARG("--auto-fan",
			opt_set_bool, &opt_autofan,
			"Automatically adjust all GPU fan speeds to maintain a target temperature"),
	OPT_WITHOUT_ARG("--auto-gpu",
			opt_set_bool, &opt_autoengine,
			"Automatically adjust all GPU engine clock speeds to maintain a target temperature"),
#endif
	OPT_WITH_ARG("--bench-algo|-b",
		     set_int_0_to_9999, opt_show_intval, &opt_bench_algo,
		     opt_hidden),
	OPT_WITH_ARG("--cpu-threads|-t",
		     force_nthreads_int, opt_show_intval, &opt_n_threads,
		     "Number of miner CPU threads"),
	OPT_WITHOUT_ARG("--debug|-D",
		     enable_debug, &opt_debug,
		     "Enable debug output"),
#ifdef HAVE_OPENCL
	OPT_WITH_ARG("--device|-d",
		     set_devices, NULL, &opt_device,
	             "Select device to use, (Use repeat -d for multiple devices, default: all)"),
	OPT_WITHOUT_ARG("--disable-gpu|-G",
			opt_set_bool, &opt_nogpu,
			"Disable GPU mining even if suitable devices exist"),
	OPT_WITHOUT_ARG("--enable-cpu|-C",
			opt_set_bool, &opt_usecpu,
			"Enable CPU mining with GPU mining (default: no CPU mining if suitable GPUs exist)"),
	OPT_WITHOUT_ARG("--failover-only",
			opt_set_bool, &opt_fail_only,
			"Don't leak work to backup pools when primary pool is lagging"),
	OPT_WITH_ARG("--gpu-threads|-g",
		     set_int_1_to_10, opt_show_intval, &opt_g_threads,
		     "Number of threads per GPU (1 - 10)"),
#ifdef HAVE_ADL
	OPT_WITH_ARG("--gpu-engine",
		     set_gpu_engine, NULL, NULL,
		     "GPU engine (over)clock range in Mhz - one value, range and/or comma separated list (e.g. 850-900,900,750-850)"),
	OPT_WITH_ARG("--gpu-fan",
		     set_gpu_fan, NULL, NULL,
		     "GPU fan percentage range - one value, range and/or comma separated list (e.g. 0-85,85,65)"),
	OPT_WITH_ARG("--gpu-memclock",
		     set_gpu_memclock, NULL, NULL,
		     "Set the GPU memory (over)clock in Mhz - one value for all or separate by commas for per card"),
	OPT_WITH_ARG("--gpu-powertune",
		     set_gpu_powertune, NULL, NULL,
		     "Set the GPU powertune percentage - one value for all or separate by commas for per card"),
	OPT_WITH_ARG("--gpu-vddc",
		     set_gpu_vddc, NULL, NULL,
		     "Set the GPU voltage in Volts - one value for all or separate by commas for per card"),
#endif
	OPT_WITH_ARG("--intensity|-I",
		     forced_int_1010, NULL, &scan_intensity,
		     "Intensity of GPU scanning (-10 -> 10, default: dynamic to maintain desktop interactivity)"),
	OPT_WITH_ARG("--kernel-path|-K",
		     opt_set_charp, opt_show_charp, &opt_kernel_path,
	             "Specify a path to where the kernel .cl files are"),
	OPT_WITH_ARG("--kernel|-k",
		     opt_set_charp, NULL, &opt_kernel,
		     "Select kernel to use (poclbm or phatk - default: auto)"),
#endif
	OPT_WITHOUT_ARG("--load-balance",
		     set_loadbalance, &pool_strategy,
		     "Change multipool strategy from failover to even load balance"),
	OPT_WITH_ARG("--log|-l",
		     set_int_0_to_9999, opt_show_intval, &opt_log_interval,
		     "Interval in seconds between log output"),
#if defined(unix)
	OPT_WITH_ARG("--monitor|-m",
		     opt_set_charp, NULL, &opt_stderr_cmd,
		     "Use custom pipe cmd for output messages"),
#endif // defined(unix)
#ifdef HAVE_ADL
	OPT_WITHOUT_ARG("--no-adl",
			opt_set_bool, &opt_noadl,
			"Disable the ATI display library used for monitoring and setting GPU parameters"),
#endif
	OPT_WITHOUT_ARG("--no-longpoll",
			opt_set_invbool, &want_longpoll,
			"Disable X-Long-Polling support"),
#ifdef HAVE_OPENCL
	OPT_WITHOUT_ARG("--no-restart",
			opt_set_invbool, &opt_restart,
			"Do not attempt to restart GPUs that hang"),
#endif
	OPT_WITH_ARG("--pass|-p",
		     set_pass, NULL, NULL,
		     "Password for bitcoin JSON-RPC server"),
	OPT_WITHOUT_ARG("--per-device-stats",
			opt_set_bool, &want_per_device_stats,
			"Force verbose mode and output per-device statistics"),
	OPT_WITHOUT_ARG("--protocol-dump|-P",
			opt_set_bool, &opt_protocol,
			"Verbose dump of protocol-level activities"),
	OPT_WITH_ARG("--queue|-Q",
		     set_int_0_to_10, opt_show_intval, &opt_queue,
		     "Minimum number of work items to have queued (0 - 10)"),
	OPT_WITHOUT_ARG("--quiet|-q",
			opt_set_bool, &opt_quiet,
			"Disable logging output, display status and errors"),
	OPT_WITHOUT_ARG("--real-quiet",
			opt_set_bool, &opt_realquiet,
			"Disable all output"),
	OPT_WITH_ARG("--retries|-r",
		     opt_set_intval, opt_show_intval, &opt_retries,
		     "Number of times to retry before giving up, if JSON-RPC call fails (-1 means never)"),
	OPT_WITH_ARG("--retry-pause|-R",
		     set_int_0_to_9999, opt_show_intval, &opt_fail_pause,
		     "Number of seconds to pause, between retries"),
	OPT_WITH_ARG("--rotate",
		     set_rotate, opt_show_intval, &opt_rotate_period,
		     "Change multipool strategy from failover to regularly rotate at N minutes"),
	OPT_WITHOUT_ARG("--round-robin",
		     set_rr, &pool_strategy,
		     "Change multipool strategy from failover to round robin on failure"),
	OPT_WITH_ARG("--scan-time|-s",
		     set_int_0_to_9999, opt_show_intval, &opt_scantime,
		     "Upper bound on time spent scanning current work, in seconds"),
	OPT_WITH_ARG("--sched-start",
		     set_schedtime, NULL, &schedstart,
		     "Set a time of day in HH:MM to start mining (a once off without a stop time)"),
	OPT_WITH_ARG("--sched-stop",
		     set_schedtime, NULL, &schedstop,
		     "Set a time of day in HH:MM to stop mining (will quit without a start time)"),
	OPT_WITH_ARG("--shares",
		     opt_set_intval, NULL, &opt_shares,
		     "Quit after mining N shares (default: unlimited)"),
	OPT_WITHOUT_ARG("--submit-stale",
			opt_set_bool, &opt_submit_stale,
		        "Submit shares even if they would normally be considered stale"),
#ifdef HAVE_SYSLOG_H
	OPT_WITHOUT_ARG("--syslog",
			opt_set_bool, &use_syslog,
			"Use system log for output messages (default: standard error)"),
#endif
#ifdef HAVE_ADL
	OPT_WITH_ARG("--temp-cutoff",
		     set_temp_cutoff, opt_show_intval, &opt_cutofftemp,
		     "Temperature where a GPU device will be automatically disabled, one value or comma separated list"),
	OPT_WITH_ARG("--temp-hysteresis",
		     set_int_1_to_10, opt_show_intval, &opt_hysteresis,
		     "Set how much the temperature can fluctuate outside limits when automanaging speeds"),
	OPT_WITH_ARG("--temp-overheat",
		     set_temp_overheat, opt_show_intval, &opt_overheattemp,
		     "Overheat temperature when automatically managing fan and GPU speeds, one value or comma separated list"),
	OPT_WITH_ARG("--temp-target",
		     set_temp_target, opt_show_intval, &opt_targettemp,
		     "Target temperature when automatically managing fan and GPU speeds, one value or comma separated list"),
#endif
	OPT_WITHOUT_ARG("--text-only|-T",
			opt_set_invbool, &use_curses,
			"Disable ncurses formatted screen output"),
	OPT_WITH_ARG("--url|-o",
		     set_url, NULL, NULL,
		     "URL for bitcoin JSON-RPC server"),
	OPT_WITH_ARG("--user|-u",
		     set_user, NULL, NULL,
		     "Username for bitcoin JSON-RPC server"),
#ifdef HAVE_OPENCL
	OPT_WITH_ARG("--vectors|-v",
		     set_vector, NULL, &opt_vectors,
		     "Override detected optimal vector width (1, 2 or 4)"),
#endif
	OPT_WITHOUT_ARG("--verbose",
			opt_set_bool, &opt_log_output,
			"Log verbose output to stderr as well as status output"),
#ifdef HAVE_OPENCL
	OPT_WITH_ARG("--worksize|-w",
		     set_int_0_to_9999, opt_show_intval, &opt_worksize,
		     "Override detected optimal worksize"),
#endif
	OPT_WITH_ARG("--userpass|-O",
		     set_userpass, NULL, NULL,
		     "Username:Password pair for bitcoin JSON-RPC server"),
	OPT_ENDTABLE
};

static char *parse_config(json_t *config)
{
	static char err_buf[200];
	json_t *val;
	struct opt_table *opt;

	for (opt = opt_config_table; opt->type != OPT_END; opt++) {
		char *p, *name;

		/* We don't handle subtables. */
		assert(!(opt->type & OPT_SUBTABLE));

		/* Pull apart the option name(s). */
		name = strdup(opt->names);
		for (p = strtok(name, "|"); p; p = strtok(NULL, "|")) {
			char *err;
			/* Ignore short options. */
			if (p[1] != '-')
				continue;

			val = json_object_get(config, p+2);
			if (!val)
				continue;

			if ((opt->type & OPT_HASARG) && json_is_string(val)) {
				err = opt->cb_arg(json_string_value(val),
						  opt->u.arg);
			} else if ((opt->type&OPT_NOARG) && json_is_true(val)) {
				err = opt->cb(opt->u.arg);
			} else {
				err = "Invalid value";
			}
			if (err) {
				sprintf(err_buf, "Parsing JSON option %s: %s",
					p, err);
				return err_buf;
			}
		}
		free(name);
	}
	return NULL;
}

static char *load_config(const char *arg, void *unused)
{
	json_error_t err;
	json_t *config;

	config = json_load_file(arg, 0, &err);
	if (!json_is_object(config))
		return "JSON decode of file failed";

	/* Parse the config now, so we can override it.  That can keep pointers
	 * so don't free config object. */
	return parse_config(config);
}

#ifdef HAVE_OPENCL
static char *print_ndevs_and_exit(int *ndevs)
{
	printf("%i GPU devices detected\n", *ndevs);
	fflush(stdout);
	exit(*ndevs);
}
#endif

extern const char *opt_argv0;

static char *opt_verusage_and_exit(const char *extra)
{
	printf("%s\n"
#ifdef HAVE_OPENCL
		"Built with CPU and GPU mining support.\n"
#else
		"Built with CPU mining support only.\n"
#endif
		, packagename);
	printf("%s", opt_usage(opt_argv0, extra));
	fflush(stdout);
	exit(0);
}

/* These options are available from commandline only */
static struct opt_table opt_cmdline_table[] = {
	OPT_WITH_ARG("--config|-c",
		     load_config, NULL, NULL,
		     "Load a JSON-format configuration file\n"
		     "See example-cfg.json for an example configuration."),
	OPT_WITHOUT_ARG("--help|-h",
			opt_verusage_and_exit, NULL,
			"Print this message"),
#ifdef HAVE_OPENCL
	OPT_WITHOUT_ARG("--ndevs|-n",
			print_ndevs_and_exit, &nDevs,
			"Enumerate number of detected GPUs and exit"),
#endif
	OPT_WITHOUT_ARG("--version|-V",
			opt_version_and_exit, packagename,
			"Display version and exit"),
	OPT_ENDTABLE
};

static bool jobj_binary(const json_t *obj, const char *key,
			void *buf, size_t buflen)
{
	const char *hexstr;
	json_t *tmp;

	tmp = json_object_get(obj, key);
	if (unlikely(!tmp)) {
		applog(LOG_ERR, "JSON key '%s' not found", key);
		return false;
	}
	hexstr = json_string_value(tmp);
	if (unlikely(!hexstr)) {
		applog(LOG_ERR, "JSON key '%s' is not a string", key);
		return false;
	}
	if (!hex2bin(buf, hexstr, buflen))
		return false;

	return true;
}

static bool work_decode(const json_t *val, struct work *work)
{
	if (unlikely(!jobj_binary(val, "midstate",
			 work->midstate, sizeof(work->midstate)))) {
		applog(LOG_ERR, "JSON inval midstate");
		goto err_out;
	}

	if (unlikely(!jobj_binary(val, "data", work->data, sizeof(work->data)))) {
		applog(LOG_ERR, "JSON inval data");
		goto err_out;
	}

	if (unlikely(!jobj_binary(val, "hash1", work->hash1, sizeof(work->hash1)))) {
		applog(LOG_ERR, "JSON inval hash1");
		goto err_out;
	}

	if (unlikely(!jobj_binary(val, "target", work->target, sizeof(work->target)))) {
		applog(LOG_ERR, "JSON inval target");
		goto err_out;
	}

	memset(work->hash, 0, sizeof(work->hash));
	gettimeofday(&work->tv_staged, NULL);

	return true;

err_out:
	return false;
}

static inline int dev_from_id(int thr_id)
{
	return thr_info[thr_id].cgpu->cpu_gpu;
}

/* Make the change in the recent value adjust dynamically when the difference
 * is large, but damp it when the values are closer together. This allows the
 * value to change quickly, but not fluctuate too dramatically when it has
 * stabilised. */
static void decay_time(double *f, double fadd)
{
	double ratio = 0;

	if (likely(*f > 0)) {
		ratio = fadd / *f;
		if (ratio > 1)
			ratio = 1 / ratio;
	}

	if (ratio > 0.9)
		*f = (fadd * 0.1 + *f) / 1.1;
	else
		*f = (fadd + *f * 0.1) / 1.1;
}

static int requests_staged(void)
{
	int ret;

	mutex_lock(stgd_lock);
	ret = HASH_COUNT(staged_work);
	mutex_unlock(stgd_lock);
	return ret;
}

static WINDOW *mainwin, *statuswin, *logwin;
static double total_secs = 0.1;
static char statusline[256];
static int cpucursor, gpucursor, logstart, logcursor;
struct cgpu_info gpus[MAX_GPUDEVICES]; /* Maximum number apparently possible */
static struct cgpu_info *cpus;

static inline void unlock_curses(void)
{
	mutex_unlock(&curses_lock);
}

static inline void lock_curses(void)
{
	mutex_lock(&curses_lock);
}

static bool curses_active_locked(void)
{
	bool ret;

	lock_curses();
	ret = curses_active;
	if (!ret)
		unlock_curses();
	return ret;
}

static void tailsprintf(char *f, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsprintf(f + strlen(f), fmt, ap);
	va_end(ap);
}

static void get_statline(char *buf, struct cgpu_info *cgpu)
{
	sprintf(buf, "%sPU%d ", cgpu->is_gpu ? "G" : "C", cgpu->cpu_gpu);
#ifdef HAVE_ADL
	if (cgpu->has_adl) {
		int gpu = cgpu->cpu_gpu;
		float gt = gpu_temp(gpu);
		int gf = gpu_fanspeed(gpu);
		int gp = gpu_fanpercent(gpu);

		if (gt != -1)
			tailsprintf(buf, "%.1fC ", gt);
		if (gf != -1)
			tailsprintf(buf, "%dRPM ", gf);
		else if (gp != -1)
			tailsprintf(buf, "%d%% ", gp);
		if (gt > -1 || gf > -1 || gp > -1)
			tailsprintf(buf, "| ");
	}
#endif
	tailsprintf(buf, "(%ds):%.1f (avg):%.1f Mh/s | A:%d R:%d HW:%d U:%.2f/m",
		opt_log_interval,
		cgpu->rolling,
		cgpu->total_mhashes / total_secs,
		cgpu->accepted,
		cgpu->rejected,
		cgpu->hw_errors,
		cgpu->utility);
}

static void text_print_status(int thr_id)
{
	struct cgpu_info *cgpu = thr_info[thr_id].cgpu;
	char logline[255];

	get_statline(logline, cgpu);
	printf("%s\n", logline);
}

/* Must be called with curses mutex lock held and curses_active */
static void curses_print_status(void)
{
	struct pool *pool = current_pool();

	wattron(statuswin, A_BOLD);
	mvwprintw(statuswin, 0, 0, " " PACKAGE " version " VERSION " - Started: %s", datestamp);
	if (opt_n_threads)
		wprintw(statuswin, " CPU Algo: %s", algo_names[opt_algo]);
	wattroff(statuswin, A_BOLD);
	mvwhline(statuswin, 1, 0, '-', 80);
	mvwprintw(statuswin, 2, 0, " %s", statusline);
	wclrtoeol(statuswin);
	mvwprintw(statuswin, 3, 0, " TQ: %d  ST: %d  SS: %d  DW: %d  NB: %d  LW: %d  GF: %d  RF: %d  I: %d",
		total_queued, requests_staged(), total_stale, total_discarded, new_blocks,
		local_work, total_go, total_ro, scan_intensity);
	wclrtoeol(statuswin);
	if (pool_strategy == POOL_LOADBALANCE && total_pools > 1)
		mvwprintw(statuswin, 4, 0, " Connected to multiple pools with%s LP",
			have_longpoll ? "": "out");
	else
		mvwprintw(statuswin, 4, 0, " Connected to %s with%s LP as user %s",
			pool->rpc_url, have_longpoll ? "": "out", pool->rpc_user);
	wclrtoeol(statuswin);
	mvwprintw(statuswin, 5, 0, " Block: %s...  Started: %s", current_hash, blocktime);
	mvwhline(statuswin, 6, 0, '-', 80);
	mvwhline(statuswin, logstart - 1, 0, '-', 80);
	mvwprintw(statuswin, gpucursor - 1, 1, "[P]ool management %s[S]ettings [D]isplay options [Q]uit",
		opt_g_threads ? "[G]PU management " : "");
	/* The window will be updated once we're done with all the devices */
	wnoutrefresh(statuswin);
}

static void curses_print_devstatus(int thr_id)
{
	if (thr_id >= 0 && thr_id < gpu_threads) {
		int gpu = dev_from_id(thr_id);
		struct cgpu_info *cgpu = &gpus[gpu];

		cgpu->utility = cgpu->accepted / ( total_secs ? total_secs : 1 ) * 60;

		mvwprintw(statuswin, gpucursor + gpu, 0, " GPU %d: ", gpu);
#ifdef HAVE_ADL
		if (cgpu->has_adl) {
			float gt = gpu_temp(gpu);
			int gf = gpu_fanspeed(gpu);
			int gp = gpu_fanpercent(gpu);

			if (gt != -1)
				wprintw(statuswin, "%.1fC ", gt);
			if (gf != -1)
				wprintw(statuswin, "%4dRPM ", gf);
			else if (gp != -1)
				wprintw(statuswin, "%2d%% ", gp);
			if (gt > -1 || gf > -1 || gp > -1)
				wprintw(statuswin, "| ");
		}
#endif
		if (cgpu->status == LIFE_DEAD)
			wprintw(statuswin, "DEAD ");
		else if (cgpu->status == LIFE_SICK)
			wprintw(statuswin, "SICK ");
		else  if (!gpu_devices[gpu])
			wprintw(statuswin, "DISABLED ");
		else
			wprintw(statuswin, "%.1f", cgpu->rolling);
		wprintw(statuswin, "/%.1fMh/s | A:%d R:%d HW:%d U:%.2f/m",
			cgpu->total_mhashes / total_secs,
			cgpu->accepted, cgpu->rejected, cgpu->hw_errors,
			cgpu->utility);
		wclrtoeol(statuswin);
	} else if (thr_id >= gpu_threads) {
		int cpu = dev_from_id(thr_id);
		struct cgpu_info *cgpu = &cpus[cpu];

		cgpu->utility = cgpu->accepted / ( total_secs ? total_secs : 1 ) * 60;

		mvwprintw(statuswin, cpucursor + cpu, 0, " CPU %d: %.2f/%.2fMh/s | A:%d R:%d U:%.2f/m",
			cpu, cgpu->rolling, cgpu->total_mhashes / total_secs,
			cgpu->accepted, cgpu->rejected,
			cgpu->utility);
		wclrtoeol(statuswin);
	}
	wnoutrefresh(statuswin);
}

static void print_status(int thr_id)
{
	if (!curses_active)
		text_print_status(thr_id);
}

/* Check for window resize. Called with curses mutex locked */
static inline bool change_logwinsize(void)
{
	int x, y, logx, logy;

	getmaxyx(mainwin, y, x);
	getmaxyx(logwin, logy, logx);
	y -= logcursor;
	/* Detect screen size change */
	if ((x != logx || y != logy) && x >= 80 && y >= 25) {
		wresize(logwin, y, x);
		return true;
	}
	return false;
}

/* For mandatory printing when mutex is already locked */
static void wlog(const char *f, ...)
{
	va_list ap;

	va_start(ap, f);
	vw_printw(logwin, f, ap);
	va_end(ap);
}

/* Mandatory printing */
void wlogprint(const char *f, ...)
{
	va_list ap;

	if (curses_active_locked()) {
		va_start(ap, f);
		vw_printw(logwin, f, ap);
		va_end(ap);
		wrefresh(logwin);
		unlock_curses();
	}
}

void log_curses(int prio, const char *f, va_list ap)
{
	if (opt_quiet && prio != LOG_ERR)
		return;

	if (curses_active_locked()) {
		if (!opt_loginput) {
			vw_printw(logwin, f, ap);
			wrefresh(logwin);
		}
		unlock_curses();
	} else
		vprintf(f, ap);
}

static void clear_logwin(void)
{
	if (curses_active_locked()) {
		wclear(logwin);
		wrefresh(logwin);
		unlock_curses();
	}
}

static bool submit_upstream_work(const struct work *work)
{
	char *hexstr = NULL;
	json_t *val, *res;
	char s[345], sd[345];
	bool rc = false;
	int thr_id = work->thr_id;
	struct cgpu_info *cgpu = thr_info[thr_id].cgpu;
	CURL *curl = curl_easy_init();
	struct pool *pool = work->pool;
	bool rolltime;

	if (unlikely(!curl)) {
		applog(LOG_ERR, "CURL initialisation failed");
		return rc;
	}

	/* build hex string */
	hexstr = bin2hex(work->data, sizeof(work->data));
	if (unlikely(!hexstr)) {
		applog(LOG_ERR, "submit_upstream_work OOM");
		goto out_nofree;
	}

	/* build JSON-RPC request */
	sprintf(s,
	      "{\"method\": \"getwork\", \"params\": [ \"%s\" ], \"id\":1}\r\n",
		hexstr);
	sprintf(sd,
	      "{\"method\": \"getwork\", \"params\": [ \"%s\" ], \"id\":1}",
		hexstr);

	if (opt_debug)
		applog(LOG_DEBUG, "DBG: sending %s submit RPC call: %s", pool->rpc_url, sd);

	/* issue JSON-RPC request */
	val = json_rpc_call(curl, pool->rpc_url, pool->rpc_userpass, s, false, false, &rolltime, pool);
	if (unlikely(!val)) {
		applog(LOG_INFO, "submit_upstream_work json_rpc_call failed");
		if (!pool_tset(pool, &pool->submit_fail)) {
			total_ro++;
			pool->remotefail_occasions++;
			applog(LOG_WARNING, "Pool %d communication failure, caching submissions", pool->pool_no);
		}
		goto out;
	} else if (pool_tclear(pool, &pool->submit_fail))
		applog(LOG_WARNING, "Pool %d communication resumed, submitting work", pool->pool_no);

	res = json_object_get(val, "result");

	/* Theoretically threads could race when modifying accepted and
	 * rejected values but the chance of two submits completing at the
	 * same time is zero so there is no point adding extra locking */
	if (json_is_true(res)) {
		cgpu->accepted++;
		total_accepted++;
		pool->accepted++;
		if (opt_debug)
			applog(LOG_DEBUG, "PROOF OF WORK RESULT: true (yay!!!)");
		if (!QUIET) {
			if (total_pools > 1)
				applog(LOG_WARNING, "Accepted %.8s %sPU %d thread %d pool %d",
				       hexstr + 152, cgpu->is_gpu? "G" : "C", cgpu->cpu_gpu, thr_id, work->pool->pool_no);
			else
				applog(LOG_WARNING, "Accepted %.8s %sPU %d thread %d",
				       hexstr + 152, cgpu->is_gpu? "G" : "C", cgpu->cpu_gpu, thr_id);
		}
		if (opt_shares && total_accepted >= opt_shares) {
			applog(LOG_WARNING, "Successfully mined %d accepted shares as requested and exiting.", opt_shares);
			kill_work();
			goto out;
		}
	} else {
		cgpu->rejected++;
		total_rejected++;
		pool->rejected++;
		if (opt_debug)
			applog(LOG_DEBUG, "PROOF OF WORK RESULT: false (booooo)");
		if (!QUIET) {
			if (total_pools > 1)
				applog(LOG_WARNING, "Rejected %.8s %sPU %d thread %d pool %d",
				       hexstr + 152, cgpu->is_gpu? "G" : "C", cgpu->cpu_gpu, thr_id, work->pool->pool_no);
			else
				applog(LOG_WARNING, "Rejected %.8s %sPU %d thread %d",
				       hexstr + 152, cgpu->is_gpu? "G" : "C", cgpu->cpu_gpu, thr_id);
		}
	}

	cgpu->utility = cgpu->accepted / ( total_secs ? total_secs : 1 ) * 60;

	if (!opt_realquiet)
		print_status(thr_id);
	if (!want_per_device_stats) {
		char logline[255];

		get_statline(logline, cgpu);
		applog(LOG_INFO, "%s", logline);
	}

	json_decref(val);

	rc = true;
out:
	free(hexstr);
out_nofree:
	curl_easy_cleanup(curl);
	return rc;
}

static const char *rpc_req =
	"{\"method\": \"getwork\", \"params\": [], \"id\":0}\r\n";

/* Select any active pool in a rotating fashion when loadbalance is chosen */
static inline struct pool *select_pool(bool lagging)
{
	static int rotating_pool = 0;
	struct pool *pool, *cp;

	cp = current_pool();

	if (pool_strategy != POOL_LOADBALANCE && !lagging)
		pool = cp;
	else
		pool = NULL;

	while (!pool) {
		if (++rotating_pool >= total_pools)
			rotating_pool = 0;
		pool = pools[rotating_pool];
		if ((!pool->idle && pool->enabled) || pool == cp)
			break;
		pool = NULL;
	}

	return pool;
}

static bool get_upstream_work(struct work *work, bool lagging)
{
	struct pool *pool;
	json_t *val = NULL;
	bool rc = false;
	int retries = 0;
	CURL *curl;

	curl = curl_easy_init();
	if (unlikely(!curl)) {
		applog(LOG_ERR, "CURL initialisation failed");
		return rc;
	}

	pool = select_pool(lagging);
	if (opt_debug)
		applog(LOG_DEBUG, "DBG: sending %s get RPC call: %s", pool->rpc_url, rpc_req);

retry:
	/* A single failure response here might be reported as a dead pool and
	 * there may be temporary denied messages etc. falsely reporting
	 * failure so retry a few times before giving up */
	while (!val && retries++ < 3)
		val = json_rpc_call(curl, pool->rpc_url, pool->rpc_userpass, rpc_req,
			    false, false, &work->rolltime, pool);
	if (unlikely(!val)) {
		applog(LOG_DEBUG, "Failed json_rpc_call in get_upstream_work");
		goto out;
	}

	rc = work_decode(json_object_get(val, "result"), work);
	if (!rc && retries < 3)
		goto retry;
	work->pool = pool;
	total_getworks++;
	pool->getwork_requested++;

	json_decref(val);
out:
	curl_easy_cleanup(curl);

	return rc;
}

static struct work *make_work(void)
{
	struct work *work = calloc(1, sizeof(struct work));

	if (unlikely(!work))
		quit(1, "Failed to calloc work in make_work");
	work->id = total_work++;
	return work;
}

static void free_work(struct work *work)
{
	free(work);
}

static void workio_cmd_free(struct workio_cmd *wc)
{
	if (!wc)
		return;

	switch (wc->cmd) {
	case WC_SUBMIT_WORK:
		free_work(wc->u.work);
		break;
	default: /* do nothing */
		break;
	}

	memset(wc, 0, sizeof(*wc));	/* poison */
	free(wc);
}

static void disable_curses(void)
{
	if (curses_active_locked()) {
		curses_active = false;
		leaveok(logwin, false);
		leaveok(statuswin, false);
		leaveok(mainwin, false);
		nocbreak();
		echo();
		delwin(logwin);
		delwin(statuswin);
		delwin(mainwin);
		endwin();
		refresh();
#ifdef WIN32
		// Move the cursor to after curses output.
		HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
		CONSOLE_SCREEN_BUFFER_INFO csbi;
		COORD coord;
		
		if (GetConsoleScreenBufferInfo(hout, &csbi)) {
			coord.X = 0;
			coord.Y = csbi.dwSize.Y - 1;
			SetConsoleCursorPosition(hout, coord);
		}
#endif
		unlock_curses();
	}
}

static void print_summary(void);

void kill_work(void)
{
	struct thr_info *thr;
	unsigned int i;

	disable_curses();
	applog(LOG_INFO, "Received kill message");

	if (opt_debug)
		applog(LOG_DEBUG, "Killing off watchdog thread");
	/* Kill the watchdog thread */
	thr = &thr_info[watchdog_thr_id];
	thr_info_cancel(thr);

	if (opt_debug)
		applog(LOG_DEBUG, "Killing off mining threads");
	/* Stop the mining threads*/
	for (i = 0; i < mining_threads; i++) {
		thr = &thr_info[i];
		thr_info_cancel(thr);
	}

	if (opt_debug)
		applog(LOG_DEBUG, "Killing off stage thread");
	/* Stop the others */
	thr = &thr_info[stage_thr_id];
	thr_info_cancel(thr);

	if (opt_debug)
		applog(LOG_DEBUG, "Killing off longpoll thread");
	thr = &thr_info[longpoll_thr_id];
	thr_info_cancel(thr);

	if (opt_debug)
		applog(LOG_DEBUG, "Killing off work thread");
	thr = &thr_info[work_thr_id];
	thr_info_cancel(thr);
}

void quit(int status, const char *format, ...);

static void sighandler(int sig)
{
	/* Restore signal handlers so we can still quit if kill_work fails */
	sigaction(SIGTERM, &termhandler, NULL);
	sigaction(SIGINT, &inthandler, NULL);
	kill_work();

	quit(sig, "Received interrupt signal.");
}

static void *get_work_thread(void *userdata)
{
	struct workio_cmd *wc = (struct workio_cmd *)userdata;
	struct work *ret_work;
	int failures = 0;

	pthread_detach(pthread_self());
	ret_work = make_work();

	if (wc->thr)
		ret_work->thr = wc->thr;
	else
		ret_work->thr = NULL;

	/* obtain new work from bitcoin via JSON-RPC */
	while (!get_upstream_work(ret_work, wc->lagging)) {
		if (unlikely((opt_retries >= 0) && (++failures > opt_retries))) {
			applog(LOG_ERR, "json_rpc_call failed, terminating workio thread");
			free_work(ret_work);
			kill_work();
			goto out;
		}

		/* pause, then restart work-request loop */
		applog(LOG_DEBUG, "json_rpc_call failed on get work, retry after %d seconds",
			fail_pause);
		sleep(fail_pause);
		fail_pause += opt_fail_pause;
	}
	fail_pause = opt_fail_pause;

	if (opt_debug)
		applog(LOG_DEBUG, "Pushing work to requesting thread");

	/* send work to requesting thread */
	if (unlikely(!tq_push(thr_info[stage_thr_id].q, ret_work))) {
		applog(LOG_ERR, "Failed to tq_push work in workio_get_work");
		kill_work();
		free_work(ret_work);
	}

out:
	workio_cmd_free(wc);
	return NULL;
}

static bool workio_get_work(struct workio_cmd *wc)
{
	pthread_t get_thread;

	if (unlikely(pthread_create(&get_thread, NULL, get_work_thread, (void *)wc))) {
		applog(LOG_ERR, "Failed to create get_work_thread");
		return false;
	}
	return true;
}

static bool stale_work(struct work *work)
{
	struct timeval now;
	bool ret = false;
	char *hexstr;

	gettimeofday(&now, NULL);
	if ((now.tv_sec - work->tv_staged.tv_sec) >= opt_scantime)
		return true;

	hexstr = bin2hex(work->data, 18);
	if (unlikely(!hexstr)) {
		applog(LOG_ERR, "submit_work_thread OOM");
		return ret;
	}

	if (strcmp(hexstr, current_block))
		ret = true;

	free(hexstr);
	return ret;
}

static void *submit_work_thread(void *userdata)
{
	struct workio_cmd *wc = (struct workio_cmd *)userdata;
	struct work *work = wc->u.work;
	struct pool *pool = work->pool;
	int failures = 0;

	pthread_detach(pthread_self());

	if (!opt_submit_stale && stale_work(work)) {
		applog(LOG_WARNING, "Stale share detected, discarding");
		total_stale++;
		pool->stale_shares++;
		goto out;
	}

	/* submit solution to bitcoin via JSON-RPC */
	while (!submit_upstream_work(work)) {
		if (!opt_submit_stale && stale_work(work)) {
			applog(LOG_WARNING, "Stale share detected, discarding");
			total_stale++;
			pool->stale_shares++;
			break;
		}
		if (unlikely((opt_retries >= 0) && (++failures > opt_retries))) {
			applog(LOG_ERR, "Failed %d retries ...terminating workio thread", opt_retries);
			kill_work();
			break;
		}

		/* pause, then restart work-request loop */
		applog(LOG_INFO, "json_rpc_call failed on submit_work, retry after %d seconds",
			fail_pause);
		sleep(fail_pause);
		fail_pause += opt_fail_pause;
	}
	fail_pause = opt_fail_pause;
out:
	workio_cmd_free(wc);
	return NULL;
}

static bool workio_submit_work(struct workio_cmd *wc)
{
	pthread_t submit_thread;

	if (unlikely(pthread_create(&submit_thread, NULL, submit_work_thread, (void *)wc))) {
		applog(LOG_ERR, "Failed to create submit_work_thread");
		return false;
	}
	return true;
}

/* Find the pool that currently has the highest priority */
static struct pool *priority_pool(int choice)
{
	struct pool *ret = NULL;
	int i;

	for (i = 0; i < total_pools; i++) {
		struct pool *pool = pools[i];

		if (pool->prio == choice) {
			ret = pool;
			break;
		}
	}

	if (unlikely(!ret)) {
		applog(LOG_ERR, "WTF No pool %d found!", choice);
		return pools[choice];
	}
	return ret;
}

static void restart_longpoll(void);

static void switch_pools(struct pool *selected)
{
	struct pool *pool, *last_pool;
	int i, pool_no;

	mutex_lock(&control_lock);
	last_pool = currentpool;
	pool_no = currentpool->pool_no;

	/* Switch selected to pool number 0 and move the rest down */
	if (selected) {
		if (selected->prio != 0) {
			for (i = 0; i < total_pools; i++) {
				pool = pools[i];
				if (pool->prio < selected->prio)
					pool->prio++;
			}
			selected->prio = 0;
		}
	}

	switch (pool_strategy) {
		/* Both of these set to the master pool */
		case POOL_FAILOVER:
		case POOL_LOADBALANCE:
			for (i = 0; i < total_pools; i++) {
				pool = priority_pool(i);
				if (!pool->idle && pool->enabled) {
					pool_no = pool->pool_no;
					break;
				}
			}
			break;
		/* Both of these simply increment and cycle */
		case POOL_ROUNDROBIN:
		case POOL_ROTATE:
			if (selected) {
				pool_no = selected->pool_no;
				break;
			}
			pool_no++;
			if (pool_no >= total_pools)
				pool_no = 0;
			break;
		default:
			break;
	}

	currentpool = pools[pool_no];
	pool = currentpool;
	mutex_unlock(&control_lock);

	if (pool != last_pool) {
		applog(LOG_WARNING, "Switching to %s", pool->rpc_url);
		/* Only switch longpoll if the new pool also supports LP */
		if (pool->hdr_path)
			restart_longpoll();
	}

	/* Reset the queued amount to allow more to be queued for the new pool */
	mutex_lock(&qd_lock);
	total_queued = 0;
	mutex_unlock(&qd_lock);
}

static void discard_work(struct work *work)
{
	if (!work->clone && !work->rolls && !work->mined) {
		if (work->pool)
			work->pool->discarded_work++;
		total_discarded++;
		if (opt_debug)
			applog(LOG_DEBUG, "Discarded work");
	} else if (opt_debug)
		applog(LOG_DEBUG, "Discarded cloned or rolled work");
	free_work(work);
}

/* This is overkill, but at least we'll know accurately how much work is
 * queued to prevent ever being left without work */
static void inc_queued(void)
{
	mutex_lock(&qd_lock);
	total_queued++;
	mutex_unlock(&qd_lock);
}

static void dec_queued(void)
{
	mutex_lock(&qd_lock);
	if (total_queued > 0)
		total_queued--;
	mutex_unlock(&qd_lock);
}

static int requests_queued(void)
{
	int ret;

	mutex_lock(&qd_lock);
	ret = total_queued;
	mutex_unlock(&qd_lock);
	return ret;
}

static int discard_stale(void)
{
	struct work *work, *tmp;
	int i, stale = 0;

	mutex_lock(stgd_lock);
	HASH_ITER(hh, staged_work, work, tmp) {
		if (stale_work(work)) {
			HASH_DEL(staged_work, work);
			if (work->clone)
				--staged_clones;
			discard_work(work);
			stale++;
		}
	}
	mutex_unlock(stgd_lock);

	if (opt_debug)
		applog(LOG_DEBUG, "Discarded %d stales that didn't match current hash", stale);

	/* Dec queued outside the loop to not have recursive locks */
	for (i = 0; i < stale; i++)
		dec_queued();

	return stale;
}

static bool queue_request(struct thr_info *thr, bool needed);

static void restart_threads(void)
{
	int i, stale;

	/* Discard staged work that is now stale */
	stale = discard_stale();

	for (i = 0; i < stale; i++)
		queue_request(NULL, true);

	for (i = 0; i < mining_threads; i++)
		work_restart[i].restart = 1;
}

static void set_curblock(char *hexstr, unsigned char *hash)
{
	unsigned char hash_swap[32];
	char *old_hash = NULL;
	struct timeval tv_now;

	/* Don't free current_hash directly to avoid dereferencing it when
	 * we might be accessing its data elsewhere */
	if (current_hash)
		old_hash = current_hash;
	strcpy(current_block, hexstr);
	gettimeofday(&tv_now, NULL);
	get_timestamp(blocktime, &tv_now);
	swap256(hash_swap, hash);
	current_hash = bin2hex(hash_swap, 16);
	if (unlikely(!current_hash))
		quit (1, "set_curblock OOM");
	if (old_hash)
		free(old_hash);
}

static void test_work_current(struct work *work)
{
	struct block *s;
	char *hexstr;

	hexstr = bin2hex(work->data, 18);
	if (unlikely(!hexstr)) {
		applog(LOG_ERR, "stage_thread OOM");
		return;
	}

	/* Search to see if this block exists yet and if not, consider it a
	 * new block and set the current block details to this one */
	rd_lock(&blk_lock);
	HASH_FIND_STR(blocks, hexstr, s);
	rd_unlock(&blk_lock);
	if (!s) {
		s = calloc(sizeof(struct block), 1);
		if (unlikely(!s))
			quit (1, "test_work_current OOM");
		strcpy(s->hash, hexstr);
		wr_lock(&blk_lock);
		HASH_ADD_STR(blocks, hash, s);
		wr_unlock(&blk_lock);
		set_curblock(hexstr, work->data);

		new_blocks++;
		if (block_changed != BLOCK_LP && block_changed != BLOCK_FIRST) {
			block_changed = BLOCK_DETECT;
			if (have_longpoll)
				applog(LOG_WARNING, "New block detected on network before longpoll, waiting on fresh work");
			else
				applog(LOG_WARNING, "New block detected on network, waiting on fresh work");
		} else
			block_changed = BLOCK_NONE;
		restart_threads();
	}

	free(hexstr);
}

static int tv_sort(struct work *worka, struct work *workb)
{
	return worka->tv_staged.tv_sec - workb->tv_staged.tv_sec;
}

static bool hash_push(struct work *work)
{
	bool rc = true;

	mutex_lock(stgd_lock);
	if (likely(!getq->frozen)) {
		HASH_ADD_INT(staged_work, id, work);
		HASH_SORT(staged_work, tv_sort);
		if (work->clone)
			++staged_clones;
	} else
		rc = false;
	pthread_cond_signal(&getq->cond);
	mutex_unlock(stgd_lock);
	return rc;
}

static void *stage_thread(void *userdata)
{
	struct thr_info *mythr = userdata;
	bool ok = true;

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	while (ok) {
		struct work *work = NULL;

		if (opt_debug)
			applog(LOG_DEBUG, "Popping work to stage thread");

		work = tq_pop(mythr->q, NULL);
		if (unlikely(!work)) {
			applog(LOG_ERR, "Failed to tq_pop in stage_thread");
			ok = false;
			break;
		}

		test_work_current(work);

		if (opt_debug)
			applog(LOG_DEBUG, "Pushing work to getwork queue");

		if (unlikely(!hash_push(work))) {
			applog(LOG_WARNING, "Failed to hash_push in stage_thread");
			continue;
		}
	}

	tq_freeze(mythr->q);
	return NULL;
}

int curses_int(const char *query)
{
	int ret;
	char *cvar;

	cvar = curses_input(query);
	ret = atoi(cvar);
	free(cvar);
	return ret;
}

static bool input_pool(bool live);

static int active_pools(void)
{
	int ret = 0;
	int i;

	for (i = 0; i < total_pools; i++) {
		if ((pools[i])->enabled)
			ret++;
	}
	return ret;
}

static void display_pool_summary(struct pool *pool)
{
	double efficiency = 0.0;

	if (curses_active_locked()) {
		wlog("Pool: %s\n", pool->rpc_url);
		wlog("%s long-poll support\n", pool->hdr_path ? "Has" : "Does not have");
		wlog(" Queued work requests: %d\n", pool->getwork_requested);
		wlog(" Share submissions: %d\n", pool->accepted + pool->rejected);
		wlog(" Accepted shares: %d\n", pool->accepted);
		wlog(" Rejected shares: %d\n", pool->rejected);
		if (pool->accepted || pool->rejected)
			wlog(" Reject ratio: %.1f\n", (double)(pool->rejected * 100) / (double)(pool->accepted + pool->rejected));
		efficiency = pool->getwork_requested ? pool->accepted * 100.0 / pool->getwork_requested : 0.0;
		wlog(" Efficiency (accepted / queued): %.0f%%\n", efficiency);

		wlog(" Discarded work due to new blocks: %d\n", pool->discarded_work);
		wlog(" Stale submissions discarded due to new blocks: %d\n", pool->stale_shares);
		wlog(" Unable to get work from server occasions: %d\n", pool->getfail_occasions);
		wlog(" Submitting work remotely delay occasions: %d\n\n", pool->remotefail_occasions);
		wrefresh(logwin);
		unlock_curses();
	}
}

/* We can't remove the memory used for this struct pool because there may
 * still be work referencing it. We just remove it from the pools list */
static void remove_pool(struct pool *pool)
{
	int i, last_pool = total_pools - 1;
	struct pool *other;

	/* Boost priority of any lower prio than this one */
	for (i = 0; i < total_pools; i++) {
		other = pools[i];
		if (other->prio > pool->prio)
			other->prio--;
	}

	if (pool->pool_no < last_pool) {
		/* Swap the last pool for this one */
		(pools[last_pool])->pool_no = pool->pool_no;
		pools[pool->pool_no] = pools[last_pool];
	}
	/* Give it an invalid number */
	pool->pool_no = total_pools;
	total_pools--;
}

static void display_pools(void)
{
	struct pool *pool;
	int selected, i;
	char input;

	opt_loginput = true;
	immedok(logwin, true);
updated:
	clear_logwin();
	for (i = 0; i < total_pools; i++) {
		pool = pools[i];

		if (pool == current_pool())
			wattron(logwin, A_BOLD);
		if (!pool->enabled)
			wattron(logwin, A_DIM);
		wlogprint("%d: %s %s Priority %d: %s  User:%s\n",
			pool->pool_no,
			pool->enabled? "Enabled" : "Disabled",
			pool->idle? "Dead" : "Alive",
			pool->prio,
			pool->rpc_url, pool->rpc_user);
		wattroff(logwin, A_BOLD | A_DIM);
	}
retry:
	wlogprint("\nCurrent pool management strategy: %s\n",
		strategies[pool_strategy]);
	if (pool_strategy == POOL_ROTATE)
		wlogprint("Set to rotate every %d minutes\n", opt_rotate_period);
	wlogprint("[A]dd pool [R]emove pool [D]isable pool [E]nable pool\n");
	wlogprint("[C]hange management strategy [S]witch pool [I]nformation\n");
	wlogprint("Or press any other key to continue\n");
	input = getch();

	if (!strncasecmp(&input, "a", 1)) {
		input_pool(true);
		goto updated;
	} else if (!strncasecmp(&input, "r", 1)) {
		if (total_pools <= 1) {
			wlogprint("Cannot remove last pool");
			goto retry;
		}
		selected = curses_int("Select pool number");
		if (selected < 0 || selected >= total_pools) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		pool = pools[selected];
		if (pool == current_pool())
			switch_pools(NULL);
		if (pool == current_pool()) {
			wlogprint("Unable to remove pool due to activity\n");
			goto retry;
		}
		pool->enabled = false;
		remove_pool(pool);
		goto updated;
	} else if (!strncasecmp(&input, "s", 1)) {
		selected = curses_int("Select pool number");
		if (selected < 0 || selected >= total_pools) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		pool = pools[selected];
		pool->enabled = true;
		switch_pools(pool);
		goto updated;
	} else if (!strncasecmp(&input, "d", 1)) {
		if (active_pools() <= 1) {
			wlogprint("Cannot disable last pool");
			goto retry;
		}
		selected = curses_int("Select pool number");
		if (selected < 0 || selected >= total_pools) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		pool = pools[selected];
		pool->enabled = false;
		if (pool == current_pool())
			switch_pools(NULL);
		goto updated;
	} else if (!strncasecmp(&input, "e", 1)) {
		selected = curses_int("Select pool number");
		if (selected < 0 || selected >= total_pools) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		pool = pools[selected];
		pool->enabled = true;
		if (pool->prio < current_pool()->prio)
			switch_pools(pool);
		goto updated;
	} else if (!strncasecmp(&input, "c", 1)) {
		for (i = 0; i <= TOP_STRATEGY; i++)
			wlogprint("%d: %s\n", i, strategies[i]);
		selected = curses_int("Select strategy number type");
		if (selected < 0 || selected > TOP_STRATEGY) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		if (selected == POOL_ROTATE) {
			opt_rotate_period = curses_int("Select interval in minutes");

			if (opt_rotate_period < 0 || opt_rotate_period > 9999) {
				opt_rotate_period = 0;
				wlogprint("Invalid selection\n");
				goto retry;
			}
		}
		pool_strategy = selected;
		switch_pools(NULL);
		goto updated;
	} else if (!strncasecmp(&input, "i", 1)) {
		selected = curses_int("Select pool number");
		if (selected < 0 || selected >= total_pools) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		pool = pools[selected];
		display_pool_summary(pool);
		goto retry;
	}

	clear_logwin();
	immedok(logwin, false);
	opt_loginput = false;
}

static void display_options(void)
{
	int selected;
	char input;

	opt_loginput = true;
	immedok(logwin, true);
retry:
	clear_logwin();
	wlogprint("[N]ormal [C]lear [S]ilent mode (disable all output)\n");
	wlogprint("[D]ebug:%s\n[P]er-device:%s\n[Q]uiet:%s\n[V]erbose:%s\n[R]PC debug:%s\n[L]og interval:%d\n",
		opt_debug ? "on" : "off",
	        want_per_device_stats? "on" : "off",
		opt_quiet ? "on" : "off",
		opt_log_output ? "on" : "off",
		opt_protocol ? "on" : "off",
		opt_log_interval);
	wlogprint("Select an option or any other key to return\n");
	input = getch();
	if (!strncasecmp(&input, "q", 1)) {
		opt_quiet ^= true;
		clear_logwin();
		wlogprint("Quiet mode %s\n", opt_quiet ? "enabled" : "disabled");
	} else if (!strncasecmp(&input, "v", 1)) {
		opt_log_output ^= true;
		if (opt_log_output)
			opt_quiet = false;
		clear_logwin();
		wlogprint("Verbose mode %s\n", opt_log_output ? "enabled" : "disabled");
	} else if (!strncasecmp(&input, "n", 1)) {
		opt_log_output = false;
		opt_debug = false;
		opt_quiet = false;
		opt_protocol = false;
		want_per_device_stats = false;
		clear_logwin();
		wlogprint("Output mode reset to normal\n");
	} else if (!strncasecmp(&input, "d", 1)) {
		opt_debug ^= true;
		opt_log_output = opt_debug;
		if (opt_debug)
			opt_quiet = false;
		clear_logwin();
		wlogprint("Debug mode %s\n", opt_debug ? "enabled" : "disabled");
	} else if (!strncasecmp(&input, "p", 1)) {
		want_per_device_stats ^= true;
		opt_log_output = want_per_device_stats;
		clear_logwin();
		wlogprint("Per-device stats %s\n", want_per_device_stats ? "enabled" : "disabled");
	} else if (!strncasecmp(&input, "r", 1)) {
		opt_protocol ^= true;
		if (opt_protocol)
			opt_quiet = false;
		clear_logwin();
		wlogprint("RPC protocol debugging %s\n", opt_protocol ? "enabled" : "disabled");
	} else if (!strncasecmp(&input, "c", 1))
		clear_logwin();
	else if (!strncasecmp(&input, "l", 1)) {
		selected = curses_int("Interval in seconds");
		if (selected < 0 || selected > 9999) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		opt_log_interval = selected;
		clear_logwin();
		wlogprint("Log interval set to %d seconds\n", opt_log_interval);
	} else if (!strncasecmp(&input, "s", 1)) {
		opt_realquiet = true;
		clear_logwin();
	} else clear_logwin();

	immedok(logwin, false);
	opt_loginput = false;
}

static void set_options(void)
{
	int selected;
	char input;

	opt_loginput = true;
	immedok(logwin, true);
retry:
	clear_logwin();
	wlogprint("\n[D]ynamic mode: %s\n[L]ongpoll: %s\n",
		opt_dynamic ? "On" : "Off", want_longpoll ? "On" : "Off");
	if (opt_dynamic)
		wlogprint("[I]ntensity: Dynamic\n");
	else
		wlogprint("[I]ntensity: %d\n", scan_intensity);
	wlogprint("[Q]ueue: %d\n[S]cantime: %d\n[R]etries: %d\n[P]ause: %d\n",
		opt_queue, opt_scantime, opt_retries, opt_fail_pause);
	wlogprint("Select an option or any other key to return\n");
	input = getch();

	if (!strncasecmp(&input, "q", 1)) {
		selected = curses_int("Extra work items to queue");
		if (selected < 0 || selected > 9999) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		opt_queue = selected;
		goto retry;
	} else if (!strncasecmp(&input, "d", 1)) {
		opt_dynamic ^= true;
		goto retry;
	} else if (!strncasecmp(&input, "l", 1)) {
		want_longpoll ^= true;
		applog(LOG_WARNING, "Longpoll %s", want_longpoll ? "enabled" : "disabled");
		restart_longpoll();
		goto retry;
	} else if (!strncasecmp(&input, "i", 1)) {
		selected = curses_int("Set GPU scan intensity (-10 -> 10)");
		if (selected < -10 || selected > 10) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		opt_dynamic = false;
		scan_intensity = selected;
		goto retry;
	} else if  (!strncasecmp(&input, "s", 1)) {
		selected = curses_int("Set scantime in seconds");
		if (selected < 0 || selected > 9999) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		opt_scantime = selected;
		goto retry;
	} else if  (!strncasecmp(&input, "r", 1)) {
		selected = curses_int("Retries before failing (-1 infinite)");
		if (selected < -1 || selected > 9999) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		opt_retries = selected;
		goto retry;
	} else if  (!strncasecmp(&input, "p", 1)) {
		selected = curses_int("Seconds to pause before network retries");
		if (selected < 1 || selected > 9999) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		opt_fail_pause = selected;
		goto retry;
	}

	clear_logwin();
	immedok(logwin, false);
	opt_loginput = false;
}

#ifdef HAVE_OPENCL
static void reinit_device(struct cgpu_info *cgpu);

static void manage_gpu(void)
{
	struct thr_info *thr;
	int selected, gpu, i;
	char checkin[40];
	char input;

	if (!opt_g_threads)
		return;

	opt_loginput = true;
	immedok(logwin, true);
	clear_logwin();
retry:

	for (gpu = 0; gpu < nDevs; gpu++) {
		struct cgpu_info *cgpu = &gpus[gpu];

		wlog("GPU %d: %.1f / %.1f Mh/s | A:%d  R:%d  HW:%d  U:%.2f/m\n",
			gpu, cgpu->rolling, cgpu->total_mhashes / total_secs,
			cgpu->accepted, cgpu->rejected, cgpu->hw_errors,
			cgpu->utility);
#ifdef HAVE_ADL
		if (gpus[gpu].has_adl) {
			int engineclock = 0, memclock = 0, activity = 0, fanspeed = 0, fanpercent = 0, powertune = 0;
			float temp = 0, vddc = 0;

			if (gpu_stats(gpu, &temp, &engineclock, &memclock, &vddc, &activity, &fanspeed, &fanpercent, &powertune)) {
				char logline[255];

				strcpy(logline, ""); // In case it has no data
				if (temp != -1)
					sprintf(logline, "%.1f C  ", temp);
				if (fanspeed != -1 || fanpercent != -1) {
					tailsprintf(logline, "F: ");
					if (fanpercent != -1)
						tailsprintf(logline, "%d%% ", fanpercent);
					if (fanspeed != -1)
						tailsprintf(logline, "(%d RPM) ", fanspeed);
					tailsprintf(logline, " ");
				}
				if (engineclock != -1)
					tailsprintf(logline, "E: %d MHz  ", engineclock);
				if (memclock != -1)
					tailsprintf(logline, "M: %d Mhz  ", memclock);
				if (vddc != -1)
					tailsprintf(logline, "V: %.3fV  ", vddc);
				if (activity != -1)
					tailsprintf(logline, "A: %d%%  ", activity);
				if (powertune != -1)
					tailsprintf(logline, "P: %d%%", powertune);
				tailsprintf(logline, "\n");
				wlog(logline);
			}
		}
#endif
		wlog("Last initialised: %s\n", cgpu->init);
		for (i = 0; i < mining_threads; i++) {
			thr = &thr_info[i];
			if (thr->cgpu != cgpu)
				continue;
			get_datestamp(checkin, &thr->last);
			wlog("Thread %d: %.1f Mh/s %s ", i, thr->rolling, gpu_devices[gpu] ? "Enabled" : "Disabled");
			switch (cgpu->status) {
				default:
				case LIFE_WELL:
					wlog("ALIVE");
					break;
				case LIFE_SICK:
					wlog("SICK reported in %s", checkin);
					break;
				case LIFE_DEAD:
					wlog("DEAD reported in %s", checkin);
					break;
				case LIFE_NOSTART:
					wlog("Never started");
					break;
			}
			wlog("\n");
		}
		wlog("\n");
	}

	wlogprint("[E]nable [D]isable [R]estart GPU %s\n",adl_active ? "[C]hange settings" : "");

	wlogprint("Or press any other key to continue\n");
	input = getch();

	if (!strncasecmp(&input, "e", 1)) {
		selected = curses_int("Select GPU to enable");
		if (selected < 0 || selected >= nDevs) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		if (gpu_devices[selected]) {
			wlogprint("Device already enabled\n");
			goto retry;
		}
		gpu_devices[selected] = true;
		for (i = 0; i < gpu_threads; i++) {
			if (dev_from_id(i) != selected)
				continue;
			thr = &thr_info[i];
			if (thr->cgpu->status != LIFE_WELL) {
				wlogprint("Must restart device before enabling it");
				gpu_devices[selected] = false;
				goto retry;
			}
			if (opt_debug)
				applog(LOG_DEBUG, "Pushing ping to thread %d", thr->id);

			tq_push(thr->q, &ping);
		}
	} if (!strncasecmp(&input, "d", 1)) {
		selected = curses_int("Select GPU to disable");
		if (selected < 0 || selected >= nDevs) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		if (!gpu_devices[selected]) {
			wlogprint("Device already disabled\n");
			goto retry;
		}
		gpu_devices[selected] = false;
	} else if (!strncasecmp(&input, "r", 1)) {
		selected = curses_int("Select GPU to attempt to restart");
		if (selected < 0 || selected >= nDevs) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		wlogprint("Attempting to restart threads of GPU %d\n", selected);
		reinit_device(&gpus[selected]);
	} else if (adl_active && (!strncasecmp(&input, "c", 1))) {
		selected = curses_int("Select GPU to change settings on");
		if (selected < 0 || selected >= nDevs) {
			wlogprint("Invalid selection\n");
			goto retry;
		}
		change_gpusettings(selected);
	}

	clear_logwin();
	immedok(logwin, false);
	opt_loginput = false;
}
#else
static void manage_gpu(void)
{
}
#endif

static void *input_thread(void *userdata)
{
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	if (!curses_active)
		return NULL;

	while (1) {
		char input;

		input = getch();
		if (!strncasecmp(&input, "q", 1)) {
			kill_work();
			return NULL;
		} else if (!strncasecmp(&input, "d", 1))
			display_options();
		else if (!strncasecmp(&input, "p", 1))
			display_pools();
		else if (!strncasecmp(&input, "s", 1))
			set_options();
		else if (!strncasecmp(&input, "g", 1))
			manage_gpu();
		if (opt_realquiet) {
			disable_curses();
			break;
		}
	}

	return NULL;
}

static void *workio_thread(void *userdata)
{
	struct thr_info *mythr = userdata;
	bool ok = true;

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	while (ok) {
		struct workio_cmd *wc;

		if (opt_debug)
			applog(LOG_DEBUG, "Popping work to work thread");

		/* wait for workio_cmd sent to us, on our queue */
		wc = tq_pop(mythr->q, NULL);
		if (unlikely(!wc)) {
			applog(LOG_ERR, "Failed to tq_pop in workio_thread");
			ok = false;
			break;
		}

		/* process workio_cmd */
		switch (wc->cmd) {
		case WC_GET_WORK:
			ok = workio_get_work(wc);
			break;
		case WC_SUBMIT_WORK:
			ok = workio_submit_work(wc);
			break;
		default:
			ok = false;
			break;
		}
	}

	tq_freeze(mythr->q);

	return NULL;
}

static void thread_reportin(struct thr_info *thr)
{
	gettimeofday(&thr->last, NULL);
	thr->cgpu->status = LIFE_WELL;
	thr->getwork = false;
}

static inline void thread_reportout(struct thr_info *thr)
{
	thr->getwork = true;
}

static void hashmeter(int thr_id, struct timeval *diff,
		      unsigned long hashes_done)
{
	struct timeval temp_tv_end, total_diff;
	double secs;
	double local_secs;
	double utility, efficiency = 0.0;
	static double local_mhashes_done = 0;
	static double rolling = 0;
	double local_mhashes = (double)hashes_done / 1000000.0;
	struct cgpu_info *cgpu = thr_info[thr_id].cgpu;
	bool showlog = false;

	/* Update the last time this thread reported in */
	if (thr_id >= 0)
		gettimeofday(&thr_info[thr_id].last, NULL);

	/* Don't bother calculating anything if we're not displaying it */
	if (opt_realquiet || !opt_log_interval)
		return;
	
	secs = (double)diff->tv_sec + ((double)diff->tv_usec / 1000000.0);

	/* So we can call hashmeter from a non worker thread */
	if (thr_id >= 0) {
		struct thr_info *thr = &thr_info[thr_id];
		double thread_rolling = 0.0;
		int i;

		if (opt_debug)
			applog(LOG_DEBUG, "[thread %d: %lu hashes, %.0f khash/sec]",
				thr_id, hashes_done, hashes_done / secs);

		/* Rolling average for each thread and each device */
		decay_time(&thr->rolling, local_mhashes / secs);
		for (i = 0; i < mining_threads; i++) {
			struct thr_info *th = &thr_info[i];

			if (th->cgpu == cgpu)
				thread_rolling += th->rolling;
		}
		decay_time(&cgpu->rolling, thread_rolling);
		cgpu->total_mhashes += local_mhashes;

		// If needed, output detailed, per-device stats
		if (want_per_device_stats) {
			struct timeval now;
			struct timeval elapsed;
			gettimeofday(&now, NULL);
			timeval_subtract(&elapsed, &now, &thr->cgpu->last_message_tv);
			if (opt_log_interval <= elapsed.tv_sec) {
				struct cgpu_info *cgpu = thr->cgpu;
				char logline[255];

				cgpu->last_message_tv = now;

				get_statline(logline, cgpu);
				if (!curses_active) {
					printf("%s          \r", logline);
					fflush(stdout);
				} else
					applog(LOG_INFO, "%s", logline);
			}
		}
	}

	/* Totals are updated by all threads so can race without locking */
	mutex_lock(&hash_lock);
	gettimeofday(&temp_tv_end, NULL);
	timeval_subtract(&total_diff, &temp_tv_end, &total_tv_end);

	total_mhashes_done += local_mhashes;
	local_mhashes_done += local_mhashes;
	if (total_diff.tv_sec < opt_log_interval)
		/* Only update the total every opt_log_interval seconds */
		goto out_unlock;
	showlog = true;
	gettimeofday(&total_tv_end, NULL);

	local_secs = (double)total_diff.tv_sec + ((double)total_diff.tv_usec / 1000000.0);
	decay_time(&rolling, local_mhashes_done / local_secs);

	timeval_subtract(&total_diff, &total_tv_end, &total_tv_start);
	total_secs = (double)total_diff.tv_sec +
		((double)total_diff.tv_usec / 1000000.0);

	utility = total_accepted / ( total_secs ? total_secs : 1 ) * 60;
	efficiency = total_getworks ? total_accepted * 100.0 / total_getworks : 0.0;

	sprintf(statusline, "%s(%ds):%.1f (avg):%.1f Mh/s | Q:%d  A:%d  R:%d  HW:%d  E:%.0f%%  U:%.2f/m",
		want_per_device_stats ? "ALL " : "",
		opt_log_interval, rolling, total_mhashes_done / total_secs,
		total_getworks, total_accepted, total_rejected, hw_errors, efficiency, utility);


	local_mhashes_done = 0;
out_unlock:
	mutex_unlock(&hash_lock);
	if (showlog) {
		if (!curses_active) {
			printf("%s          \r", statusline);
			fflush(stdout);
		} else
			applog(LOG_INFO, "%s", statusline);
	}
}

static bool pool_active(struct pool *pool, bool pinging)
{
	bool ret = false;
	json_t *val;
	CURL *curl;
	bool rolltime;

	curl = curl_easy_init();
	if (unlikely(!curl)) {
		applog(LOG_ERR, "CURL initialisation failed");
		return false;
	}

	applog(LOG_INFO, "Testing pool %s", pool->rpc_url);
	val = json_rpc_call(curl, pool->rpc_url, pool->rpc_userpass, rpc_req,
			true, false, &rolltime, pool);

	if (val) {
		struct work *work = make_work();
		bool rc;

		rc = work_decode(json_object_get(val, "result"), work);
		if (rc) {
			applog(LOG_DEBUG, "Successfully retrieved and deciphered work from pool %u %s",
			       pool->pool_no, pool->rpc_url);
			work->pool = pool;
			work->rolltime = rolltime;
			if (opt_debug)
				applog(LOG_DEBUG, "Pushing pooltest work to base pool");

			tq_push(thr_info[stage_thr_id].q, work);
			total_getworks++;
			pool->getwork_requested++;
			inc_queued();
			ret = true;
			gettimeofday(&pool->tv_idle, NULL);
		} else {
			applog(LOG_DEBUG, "Successfully retrieved but FAILED to decipher work from pool %u %s",
			       pool->pool_no, pool->rpc_url);
			free_work(work);
		}
		json_decref(val);
	} else {
		applog(LOG_DEBUG, "FAILED to retrieve work from pool %u %s",
		       pool->pool_no, pool->rpc_url);
		if (!pinging)
			applog(LOG_WARNING, "Pool down, URL or credentials invalid");
	}

	curl_easy_cleanup(curl);
	return ret;
}

static void pool_died(struct pool *pool)
{
	if (!pool_tset(pool, &pool->idle)) {
		applog(LOG_WARNING, "Pool %d %s not responding!", pool->pool_no, pool->rpc_url);
		gettimeofday(&pool->tv_idle, NULL);
		switch_pools(NULL);
	}
}

static inline int cp_prio(void)
{
	int prio;

	mutex_lock(&control_lock);
	prio = currentpool->prio;
	mutex_unlock(&control_lock);
	return prio;
}

static void pool_resus(struct pool *pool)
{
	applog(LOG_WARNING, "Pool %d %s recovered", pool->pool_no, pool->rpc_url);
	if (pool->prio < cp_prio() && pool_strategy == POOL_FAILOVER)
		switch_pools(NULL);
}

static bool queue_request(struct thr_info *thr, bool needed)
{
	struct workio_cmd *wc;
	int rq = requests_queued();

	if (rq >= mining_threads + staged_clones)
		return true;

	/* fill out work request message */
	wc = calloc(1, sizeof(*wc));
	if (unlikely(!wc)) {
		applog(LOG_ERR, "Failed to calloc wc in queue_request");
		return false;
	}

	wc->cmd = WC_GET_WORK;
	if (thr)
		wc->thr = thr;
	else
		wc->thr = NULL;

	/* If we're queueing work faster than we can stage it, consider the
	 * system lagging and allow work to be gathered from another pool if
	 * possible */
	if (rq && needed && !requests_staged() && !opt_fail_only)
		wc->lagging = true;

	if (opt_debug)
		applog(LOG_DEBUG, "Queueing getwork request to work thread");

	/* send work request to workio thread */
	if (unlikely(!tq_push(thr_info[work_thr_id].q, wc))) {
		applog(LOG_ERR, "Failed to tq_push in queue_request");
		workio_cmd_free(wc);
		return false;
	}

	inc_queued();
	return true;
}

struct work *hash_pop(const struct timespec *abstime)
{
	struct work *work = NULL;
	int rc;

	mutex_lock(stgd_lock);
	if (HASH_COUNT(staged_work))
		goto pop;

	if (abstime)
		rc = pthread_cond_timedwait(&getq->cond, stgd_lock, abstime);
	else
		rc = pthread_cond_wait(&getq->cond, stgd_lock);
	if (rc)
		goto out;
	if (!HASH_COUNT(staged_work))
		goto out;

pop:
	work = staged_work;
	HASH_DEL(staged_work, work);
	if (work->clone)
		--staged_clones;
out:
	mutex_unlock(stgd_lock);
	return work;
}

static inline bool should_roll(struct work *work)
{
	int rs;

	rs = requests_staged();
	if (rs >= mining_threads)
		return false;
	if (work->pool == current_pool() || pool_strategy == POOL_LOADBALANCE || !rs)
		return true;
	return false;
}

static inline bool can_roll(struct work *work)
{
	return (work->pool && !stale_work(work) && work->rolltime &&
		work->rolls < 11 && !work->clone);
}

static void roll_work(struct work *work)
{
	uint32_t *work_ntime;
	uint32_t ntime;

	work_ntime = (uint32_t *)(work->data + 68);
	ntime = be32toh(*work_ntime);
	ntime++;
	*work_ntime = htobe32(ntime);
	local_work++;
	work->rolls++;
	work->blk.nonce = 0;
	if (opt_debug)
		applog(LOG_DEBUG, "Successfully rolled work");
}

/* Recycle the work at a higher starting res_nonce if we know the thread we're
 * giving it to will not finish scanning it. We keep the master copy to be
 * recycled more rapidly and discard the clone to avoid repeating work */
static bool divide_work(struct timeval *now, struct work *work, uint32_t hash_div)
{
	uint64_t hash_inc;

	if (work->clone)
		return false;

	hash_inc = MAXTHREADS / hash_div * 2;
	if ((uint64_t)work->blk.nonce + hash_inc < MAXTHREADS) {
		/* Okay we can divide it up */
		work->blk.nonce += hash_inc;
		work->cloned = true;
		local_work++;
		if (opt_debug)
			applog(LOG_DEBUG, "Successfully divided work");
		return true;
	} else if (can_roll(work) && should_roll(work)) {
		roll_work(work);
		return true;
	}
	return false;
}

static bool get_work(struct work *work, bool requested, struct thr_info *thr,
		     const int thr_id, uint32_t hash_div)
{
	struct timespec abstime = {};
	struct timeval now;
	struct work *work_heap;
	struct pool *pool;
	bool ret = false;
	int failures = 0;

	/* Tell the watchdog thread this thread is waiting on getwork and
	 * should not be restarted */
	thread_reportout(thr);
retry:
	pool = current_pool();
	if (unlikely((!requested || requests_queued() < opt_queue) && !queue_request(thr, true))) {
		applog(LOG_WARNING, "Failed to queue_request in get_work");
		goto out;
	}

	if (!requests_staged()) {
		if (can_roll(work)) {
			roll_work(work);
			ret = true;
			goto out;
		}
		if (requested && requests_queued() >= mining_threads &&
		    !pool_tset(pool, &pool->lagging)) {
			applog(LOG_WARNING, "Pool %d not providing work fast enough",
				pool->pool_no);
			pool->getfail_occasions++;
			total_go++;
		}
	}

	requested = false;
	gettimeofday(&now, NULL);
	abstime.tv_sec = now.tv_sec + 60;

	if (opt_debug)
		applog(LOG_DEBUG, "Popping work from get queue to get work");

	/* wait for 1st response, or get cached response */
	work_heap = hash_pop(&abstime);
	if (unlikely(!work_heap)) {
		/* Attempt to switch pools if this one times out */
		pool_died(pool);
		goto retry;
	}

	if (stale_work(work_heap)) {
		dec_queued();
		discard_work(work_heap);
		goto retry;
	}

	pool = work_heap->pool;
	/* If we make it here we have succeeded in getting fresh work */
	if (!work_heap->mined) {
		pool_tclear(pool, &pool->lagging);
		if (pool_tclear(pool, &pool->idle))
			pool_resus(pool);
	}

	memcpy(work, work_heap, sizeof(*work));

	/* Copy the res nonce back so we know to start at a higher baseline
	 * should we divide the same work up again. Make the work we're
	 * handing out be clone */
	if (divide_work(&now, work_heap, hash_div)) {
		if (opt_debug)
			applog(LOG_DEBUG, "Pushing divided work to get queue head");

		hash_push(work_heap);
		work->clone = true;
	} else {
		dec_queued();
		free_work(work_heap);
	}

	ret = true;
out:
	if (unlikely(ret == false)) {
		if ((opt_retries >= 0) && (++failures > opt_retries)) {
			applog(LOG_ERR, "Failed %d times to get_work");
			return ret;
		}
		applog(LOG_DEBUG, "Retrying after %d seconds", fail_pause);
		sleep(fail_pause);
		fail_pause += opt_fail_pause;
		goto retry;
	}
	fail_pause = opt_fail_pause;

	work->thr_id = thr_id;
	thread_reportin(thr);
	if (ret)
		work->mined = true;
	return ret;
}

static bool submit_work_sync(struct thr_info *thr, const struct work *work_in)
{
	struct workio_cmd *wc;

	/* fill out work request message */
	wc = calloc(1, sizeof(*wc));
	if (unlikely(!wc)) {
		applog(LOG_ERR, "Failed to calloc wc in submit_work_sync");
		return false;
	}

	wc->u.work = make_work();
	wc->cmd = WC_SUBMIT_WORK;
	wc->thr = thr;
	memcpy(wc->u.work, work_in, sizeof(*work_in));

	if (opt_debug)
		applog(LOG_DEBUG, "Pushing submit work to work thread");

	/* send solution to workio thread */
	if (unlikely(!tq_push(thr_info[work_thr_id].q, wc))) {
		applog(LOG_ERR, "Failed to tq_push work in submit_work_sync");
		goto err_out;
	}

	return true;
err_out:
	workio_cmd_free(wc);
	return false;
}

bool submit_nonce(struct thr_info *thr, struct work *work, uint32_t nonce)
{
	work->data[64+12+0] = (nonce>>0) & 0xff;
	work->data[64+12+1] = (nonce>>8) & 0xff;
	work->data[64+12+2] = (nonce>>16) & 0xff;
	work->data[64+12+3] = (nonce>>24) & 0xff;
	/* Do one last check before attempting to submit the work */
	if (!fulltest(work->data + 64, work->target))
		return true;
	return submit_work_sync(thr, work);
}

static void *miner_thread(void *userdata)
{
	struct work *work = make_work();
	struct thr_info *mythr = userdata;
	const int thr_id = mythr->id;
	uint32_t max_nonce = 0xffffff, total_hashes = 0;
	unsigned long hashes_done = max_nonce;
	bool needs_work = true;
	/* Try to cycle approximately 5 times before each log update */
	const unsigned long cycle = opt_log_interval / 5 ? : 1;
	unsigned const int request_interval = opt_scantime * 2 / 3 ? : 1;
	bool requested = false;
	uint32_t nonce_inc = max_nonce, hash_div = 1;
	double hash_divfloat = 1.0;

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	/* Set worker threads to nice 19 and then preferentially to SCHED_IDLE
	 * and if that fails, then SCHED_BATCH. No need for this to be an
	 * error if it fails */
	setpriority(PRIO_PROCESS, 0, 19);
	drop_policy();

	/* Cpu affinity only makes sense if the number of threads is a multiple
	 * of the number of CPUs */
	if (!(opt_n_threads % num_processors))
		affine_to_cpu(thr_id - gpu_threads, dev_from_id(thr_id));

	/* Invalidate pool so it fails can_roll() test */
	work->pool = NULL;

	while (1) {
		struct timeval tv_workstart, tv_start, tv_end, diff;
		uint64_t max64;
		bool rc;

		if (needs_work) {
			gettimeofday(&tv_workstart, NULL);
			/* obtain new work from internal workio thread */
			if (unlikely(!get_work(work, requested, mythr, thr_id, hash_div))) {
				applog(LOG_ERR, "work retrieval failed, exiting "
					"mining thread %d", thr_id);
				goto out;
			}
			needs_work = requested = false;
			total_hashes = 0;
			max_nonce = work->blk.nonce + hashes_done;
		}
		hashes_done = 0;
		gettimeofday(&tv_start, NULL);

		/* scan nonces for a proof-of-work hash */
		switch (opt_algo) {
		case ALGO_C:
			rc = scanhash_c(thr_id, work->midstate, work->data + 64,
				        work->hash1, work->hash, work->target,
					max_nonce, &hashes_done,
					work->blk.nonce);
			break;

#ifdef WANT_X8632_SSE2
		case ALGO_SSE2_32: {
			unsigned int rc5 =
			        scanhash_sse2_32(thr_id, work->midstate, work->data + 64,
						 work->hash1, work->hash,
						 work->target,
					         max_nonce, &hashes_done,
						 work->blk.nonce);
			rc = (rc5 == -1) ? false : true;
			}
			break;
#endif

#ifdef WANT_X8664_SSE2
		case ALGO_SSE2_64: {
			unsigned int rc5 =
			        scanhash_sse2_64(thr_id, work->midstate, work->data + 64,
						 work->hash1, work->hash,
						 work->target,
					         max_nonce, &hashes_done,
						 work->blk.nonce);
			rc = (rc5 == -1) ? false : true;
			}
			break;
#endif

#ifdef WANT_X8664_SSE4
		case ALGO_SSE4_64: {
			unsigned int rc5 =
			        scanhash_sse4_64(thr_id, work->midstate, work->data + 64,
						 work->hash1, work->hash,
						 work->target,
					         max_nonce, &hashes_done,
						 work->blk.nonce);
			rc = (rc5 == -1) ? false : true;
			}
			break;
#endif

#ifdef WANT_SSE2_4WAY
		case ALGO_4WAY: {
			unsigned int rc4 =
				ScanHash_4WaySSE2(thr_id, work->midstate, work->data + 64,
						  work->hash1, work->hash,
						  work->target,
						  max_nonce, &hashes_done,
						  work->blk.nonce);
			rc = (rc4 == -1) ? false : true;
			}
			break;
#endif

#ifdef WANT_VIA_PADLOCK
		case ALGO_VIA:
			rc = scanhash_via(thr_id, work->data, work->target,
					  max_nonce, &hashes_done,
					  work->blk.nonce);
			break;
#endif
		case ALGO_CRYPTOPP:
			rc = scanhash_cryptopp(thr_id, work->midstate, work->data + 64,
				        work->hash1, work->hash, work->target,
					max_nonce, &hashes_done,
					work->blk.nonce);
			break;

#ifdef WANT_CRYPTOPP_ASM32
		case ALGO_CRYPTOPP_ASM32:
			rc = scanhash_asm32(thr_id, work->midstate, work->data + 64,
				        work->hash1, work->hash, work->target,
					max_nonce, &hashes_done,
					work->blk.nonce);
			break;
#endif

		default:
			/* should never happen */
			goto out;
		}

		/* record scanhash elapsed time */
		gettimeofday(&tv_end, NULL);
		timeval_subtract(&diff, &tv_end, &tv_start);

		hashes_done -= work->blk.nonce;
		hashmeter(thr_id, &diff, hashes_done);
		total_hashes += hashes_done;
		work->blk.nonce += hashes_done;

		/* adjust max_nonce to meet target cycle time */
		if (diff.tv_usec > 500000)
			diff.tv_sec++;
		if (diff.tv_sec && diff.tv_sec != cycle) {
			uint64_t next_inc = ((uint64_t)hashes_done * (uint64_t)cycle) / (uint64_t)diff.tv_sec;

			if (next_inc > (uint64_t)nonce_inc / 2 * 3)
				next_inc = nonce_inc / 2 * 3;
			nonce_inc = next_inc;
		} else if (!diff.tv_sec)
			nonce_inc = hashes_done * 2;
		if (nonce_inc < 4)
			nonce_inc = 0xffffff;
		max64 = work->blk.nonce + nonce_inc;
		if (max64 > 0xfffffffaULL)
			max64 = 0xfffffffaULL;
		max_nonce = max64;

		/* if nonce found, submit work */
		if (unlikely(rc)) {
			if (opt_debug)
				applog(LOG_DEBUG, "CPU %d found something?", dev_from_id(thr_id));
			if (unlikely(!submit_work_sync(mythr, work))) {
				applog(LOG_ERR, "Failed to submit_work_sync in miner_thread %d", thr_id);
				break;
			}
			work->blk.nonce += 4;
		}

		timeval_subtract(&diff, &tv_end, &tv_workstart);
		if (!requested && (diff.tv_sec >= request_interval)) {
			thread_reportout(mythr);
			if (unlikely(!queue_request(mythr, false))) {
				applog(LOG_ERR, "Failed to queue_request in miner_thread %d", thr_id);
				goto out;
			}
			thread_reportin(mythr);
			requested = true;
		}

		if (diff.tv_sec > opt_scantime) {
			decay_time(&hash_divfloat , (double)((MAXTHREADS / total_hashes) ? : 1));
			hash_div = hash_divfloat;
			needs_work = true;
		} else if (work_restart[thr_id].restart || stale_work(work) ||
			work->blk.nonce >= MAXTHREADS - hashes_done)
				needs_work = true;

		if (unlikely(mythr->pause)) {
			applog(LOG_WARNING, "Thread %d being disabled", thr_id);
			mythr->rolling = mythr->cgpu->rolling = 0;
			if (opt_debug)
				applog(LOG_DEBUG, "Popping wakeup ping in miner thread");

			thread_reportout(mythr);
			tq_pop(mythr->q, NULL); /* Ignore ping that's popped */
			thread_reportin(mythr);
			applog(LOG_WARNING, "Thread %d being re-enabled", thr_id);
		}
	}

out:
	thread_reportin(mythr);
	applog(LOG_ERR, "Thread %d failure, exiting", thr_id);
	tq_freeze(mythr->q);

	return NULL;
}

enum {
	STAT_SLEEP_INTERVAL		= 1,
	STAT_CTR_INTERVAL		= 10000000,
	FAILURE_INTERVAL		= 30,
};

#ifdef HAVE_OPENCL
static _clState *clStates[MAX_GPUDEVICES];

static cl_int queue_poclbm_kernel(_clState *clState, dev_blk_ctx *blk)
{
	cl_kernel *kernel = &clState->kernel;
	cl_int status = 0;
	int num = 0;

	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->ctx_a);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->ctx_b);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->ctx_c);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->ctx_d);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->ctx_e);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->ctx_f);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->ctx_g);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->ctx_h);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->cty_b);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->cty_c);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->cty_d);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->cty_f);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->cty_g);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->cty_h);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->nonce);

	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->fW0);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->fW1);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->fW2);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->fW3);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->fW15);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->fW01r);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->fcty_e);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->fcty_e2);

	status |= clSetKernelArg(*kernel, num++, sizeof(clState->outputBuffer),
				 (void *)&clState->outputBuffer);

	return status;
}

static cl_int queue_phatk_kernel(_clState *clState, dev_blk_ctx *blk)
{
	cl_uint vwidth = clState->preferred_vwidth;
	cl_kernel *kernel = &clState->kernel;
	cl_int status = 0;
	int i, num = 0;
	uint *nonces;

	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->ctx_a);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->ctx_b);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->ctx_c);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->ctx_d);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->ctx_e);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->ctx_f);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->ctx_g);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->ctx_h);
	
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->cty_b);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->cty_c);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->cty_d);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->cty_f);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->cty_g);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->cty_h);

	nonces = alloca(sizeof(uint) * vwidth);
	for (i = 0; i < vwidth; i++)
		nonces[i] = blk->nonce + i;
	status |= clSetKernelArg(*kernel, num++, vwidth * sizeof(uint), (void *)nonces);

	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->W16);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->W17);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->PreVal4_2);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->PreVal0);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->PreW18);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->PreW19);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->PreW31);
	status |= clSetKernelArg(*kernel, num++, sizeof(uint), (void *)&blk->PreW32);

	status |= clSetKernelArg(*kernel, num++, sizeof(clState->outputBuffer),
				 (void *)&clState->outputBuffer);

	return status;
}

static void set_threads_hashes(unsigned int vectors, unsigned int *threads,
			       unsigned int *hashes, size_t *globalThreads,
			       unsigned int minthreads)
{
	*threads = 1 << (15 + scan_intensity);
	if (*threads < minthreads)
		*threads = minthreads;
	*globalThreads = *threads;
	*hashes = *threads * vectors;
}

static void *gpuminer_thread(void *userdata)
{
	cl_int (*queue_kernel_parameters)(_clState *, dev_blk_ctx *);

	const unsigned long cycle = opt_log_interval / 5 ? : 1;
	struct timeval tv_start, tv_end, diff, tv_workstart;
	struct thr_info *mythr = userdata;
	const int thr_id = mythr->id;
	uint32_t *res, *blank_res;
	double gpu_ms_average = 7;
	int gpu = dev_from_id(thr_id);

	size_t globalThreads[1];
	size_t localThreads[1];

	cl_int status;

	_clState *clState = clStates[thr_id];
	const cl_kernel *kernel = &clState->kernel;

	struct work *work = make_work();
	unsigned int threads;
	unsigned const int vectors = clState->preferred_vwidth;
	unsigned int hashes;
	unsigned int hashes_done = 0;

	/* Request the next work item at 2/3 of the scantime */
	unsigned const int request_interval = opt_scantime * 2 / 3 ? : 1;
	unsigned const long request_nonce = MAXTHREADS / 3 * 2;
	bool requested = false;
	uint32_t total_hashes = 0, hash_div = 1;

	switch (chosen_kernel) {
		case KL_POCLBM:
			queue_kernel_parameters = &queue_poclbm_kernel;
			break;
		case KL_PHATK:
		default:
			queue_kernel_parameters = &queue_phatk_kernel;
			break;
	}

	if (opt_dynamic) {
		/* Minimise impact on desktop if we want dynamic mode */
		setpriority(PRIO_PROCESS, 0, 19);
		drop_policy();
	}

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	res = calloc(BUFFERSIZE, 1);
	blank_res = calloc(BUFFERSIZE, 1);

	if (!res || !blank_res) {
		applog(LOG_ERR, "Failed to calloc in gpuminer_thread");
		goto out;
	}

	gettimeofday(&tv_start, NULL);
	localThreads[0] = clState->work_size;
	set_threads_hashes(vectors, &threads, &hashes, &globalThreads[0],
			   localThreads[0]);

	diff.tv_sec = 0;
	gettimeofday(&tv_end, NULL);

	work->pool = NULL;

	status = clEnqueueWriteBuffer(clState->commandQueue, clState->outputBuffer, CL_TRUE, 0,
			BUFFERSIZE, blank_res, 0, NULL, NULL);
	if (unlikely(status != CL_SUCCESS))
		{ applog(LOG_ERR, "Error: clEnqueueWriteBuffer failed."); goto out; }

	mythr->cgpu->status = LIFE_WELL;
	if (opt_debug)
		applog(LOG_DEBUG, "Popping ping in gpuminer thread");

	tq_pop(mythr->q, NULL); /* Wait for a ping to start */
	gettimeofday(&tv_workstart, NULL);
	/* obtain new work from internal workio thread */
	if (unlikely(!get_work(work, requested, mythr, thr_id, hash_div))) {
		applog(LOG_ERR, "work retrieval failed, exiting "
			"gpu mining thread %d", thr_id);
		goto out;
	}
	requested = false;
	precalc_hash(&work->blk, (uint32_t *)(work->midstate), (uint32_t *)(work->data + 64));
	work->blk.nonce = 0;

	while (1) {
		struct timeval tv_gpustart, tv_gpuend;
		suseconds_t gpu_us;

		gettimeofday(&tv_gpustart, NULL);
		timeval_subtract(&diff, &tv_gpustart, &tv_gpuend);
		/* This finish flushes the readbuffer set with CL_FALSE later */
		clFinish(clState->commandQueue);
		gettimeofday(&tv_gpuend, NULL);
		timeval_subtract(&diff, &tv_gpuend, &tv_gpustart);
		gpu_us = diff.tv_sec * 1000000 + diff.tv_usec;
		decay_time(&gpu_ms_average, gpu_us / 1000);
		if (opt_dynamic) {
			/* Try to not let the GPU be out for longer than 6ms, but
			 * increase intensity when the system is idle, unless
			 * dynamic is disabled. */
			if (gpu_ms_average > 7) {
				if (scan_intensity > -10)
					scan_intensity--;
			} else if (gpu_ms_average < 3) {
				if (scan_intensity < 10)
					scan_intensity++;
			}
		}
		set_threads_hashes(vectors, &threads, &hashes, globalThreads, localThreads[0]);

		if (diff.tv_sec > opt_scantime ||
		    work->blk.nonce >= MAXTHREADS - hashes ||
		    work_restart[thr_id].restart ||
		    stale_work(work)) {
			/* Ignore any reads since we're getting new work and queue a clean buffer */
			status = clEnqueueWriteBuffer(clState->commandQueue, clState->outputBuffer, CL_FALSE, 0,
					BUFFERSIZE, blank_res, 0, NULL, NULL);
			if (unlikely(status != CL_SUCCESS))
				{ applog(LOG_ERR, "Error: clEnqueueWriteBuffer failed."); goto out; }
			memset(res, 0, BUFFERSIZE);

			gettimeofday(&tv_workstart, NULL);
			if (opt_debug)
				applog(LOG_DEBUG, "getwork thread %d", thr_id);
			/* obtain new work from internal workio thread */
			if (unlikely(!get_work(work, requested, mythr, thr_id, hash_div))) {
				applog(LOG_ERR, "work retrieval failed, exiting "
					"gpu mining thread %d", thr_id);
				goto out;
			}
			requested = false;

			precalc_hash(&work->blk, (uint32_t *)(work->midstate), (uint32_t *)(work->data + 64));
			work_restart[thr_id].restart = 0;

			/* Flushes the writebuffer set with CL_FALSE above */
			clFinish(clState->commandQueue);
		}
		status = queue_kernel_parameters(clState, &work->blk);
		if (unlikely(status != CL_SUCCESS))
			{ applog(LOG_ERR, "Error: clSetKernelArg of all params failed."); goto out; }

		/* MAXBUFFERS entry is used as a flag to say nonces exist */
		if (res[FOUND]) {
			/* Clear the buffer again */
			status = clEnqueueWriteBuffer(clState->commandQueue, clState->outputBuffer, CL_FALSE, 0,
					BUFFERSIZE, blank_res, 0, NULL, NULL);
			if (unlikely(status != CL_SUCCESS))
				{ applog(LOG_ERR, "Error: clEnqueueWriteBuffer failed."); goto out; }
			if (opt_debug)
				applog(LOG_DEBUG, "GPU %d found something?", gpu);
			postcalc_hash_async(mythr, work, res);
			memset(res, 0, BUFFERSIZE);
			clFinish(clState->commandQueue);
		}

		status = clEnqueueNDRangeKernel(clState->commandQueue, *kernel, 1, NULL,
				globalThreads, localThreads, 0,  NULL, NULL);
		if (unlikely(status != CL_SUCCESS))
			{ applog(LOG_ERR, "Error: Enqueueing kernel onto command queue. (clEnqueueNDRangeKernel)"); goto out; }

		status = clEnqueueReadBuffer(clState->commandQueue, clState->outputBuffer, CL_FALSE, 0,
				BUFFERSIZE, res, 0, NULL, NULL);
		if (unlikely(status != CL_SUCCESS))
			{ applog(LOG_ERR, "Error: clEnqueueReadBuffer failed. (clEnqueueReadBuffer)"); goto out;}

		gettimeofday(&tv_end, NULL);
		timeval_subtract(&diff, &tv_end, &tv_start);
		hashes_done += hashes;
		total_hashes += hashes;
		work->blk.nonce += hashes;
		if (diff.tv_sec >= cycle) {
			hashmeter(thr_id, &diff, hashes_done);
			gettimeofday(&tv_start, NULL);
			hashes_done = 0;
		}

		timeval_subtract(&diff, &tv_end, &tv_workstart);
		if (!requested) {
#if 0
			if (diff.tv_sec > request_interval)
				hash_div = (MAXTHREADS / total_hashes) ? : 1;
#endif
			if (diff.tv_sec > request_interval || work->blk.nonce > request_nonce) {
				thread_reportout(mythr);
				if (unlikely(!queue_request(mythr, false))) {
					applog(LOG_ERR, "Failed to queue_request in gpuminer_thread %d", thr_id);
					goto out;
				}
				thread_reportin(mythr);
				requested = true;
			}
		}
		if (unlikely(!gpu_devices[gpu] || mythr->pause)) {
			applog(LOG_WARNING, "Thread %d being disabled", thr_id);
			mythr->rolling = mythr->cgpu->rolling = 0;
			if (opt_debug)
				applog(LOG_DEBUG, "Popping wakeup ping in gpuminer thread");

			thread_reportout(mythr);
			tq_pop(mythr->q, NULL); /* Ignore ping that's popped */
			thread_reportin(mythr);
			applog(LOG_WARNING, "Thread %d being re-enabled", thr_id);
		}
	}
out:
	clReleaseCommandQueue(clState->commandQueue);
	clReleaseKernel(clState->kernel);
	clReleaseProgram(clState->program);
	clReleaseContext(clState->context);

	thread_reportin(mythr);
	applog(LOG_ERR, "Thread %d failure, exiting", thr_id);
	tq_freeze(mythr->q);

	return NULL;
}
#endif /* HAVE_OPENCL */

/* Stage another work item from the work returned in a longpoll */
static void convert_to_work(json_t *val, bool rolltime)
{
	struct work *work;
	bool rc;

	work = make_work();

	rc= work_decode(json_object_get(val, "result"), work);
	if (unlikely(!rc)) {
		applog(LOG_ERR, "Could not convert longpoll data to work");
		return;
	}
	work->pool = current_pool();
	work->rolltime = rolltime;
	/* We'll be checking this work item twice, but we already know it's
	 * from a new block so explicitly force the new block detection now
	 * rather than waiting for it to hit the stage thread */
	test_work_current(work);

	if (opt_debug)
		applog(LOG_DEBUG, "Pushing converted work to stage thread");

	if (unlikely(!tq_push(thr_info[stage_thr_id].q, work)))
		applog(LOG_ERR, "Could not tq_push work in convert_to_work");
	else if (opt_debug)
		applog(LOG_DEBUG, "Converted longpoll data to work");
}

static void *longpoll_thread(void *userdata)
{
	struct thr_info *mythr = userdata;
	CURL *curl = NULL;
	char *copy_start, *hdr_path, *lp_url = NULL;
	bool need_slash = false;
	int failures = 0;
	struct pool *pool = current_pool();

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	pthread_detach(pthread_self());

	curl = curl_easy_init();
	if (unlikely(!curl)) {
		applog(LOG_ERR, "CURL initialisation failed");
		goto out;
	}

	tq_pop(mythr->q, NULL);
	if (!pool->hdr_path) {
		applog(LOG_WARNING, "No long-poll found on this server");
		goto out;
	}
	hdr_path = pool->hdr_path;

	/* full URL */
	if (strstr(hdr_path, "://")) {
		lp_url = hdr_path;
		hdr_path = NULL;
	}
	
	/* absolute path, on current server */
	else {
		copy_start = (*hdr_path == '/') ? (hdr_path + 1) : hdr_path;
		if (pool->rpc_url[strlen(pool->rpc_url) - 1] != '/')
			need_slash = true;

		lp_url = malloc(strlen(pool->rpc_url) + strlen(copy_start) + 2);
		if (!lp_url)
			goto out;

		sprintf(lp_url, "%s%s%s", pool->rpc_url, need_slash ? "/" : "", copy_start);
	}

	have_longpoll = true;
	applog(LOG_WARNING, "Long-polling activated for %s", lp_url);

	while (1) {
		struct timeval start, end;
		bool rolltime;
		json_t *val;

		gettimeofday(&start, NULL);
		val = json_rpc_call(curl, lp_url, pool->rpc_userpass, rpc_req,
				    false, true, &rolltime, pool);
		if (likely(val)) {
			/* Keep track of who ordered a restart_threads to make
			 * sure it's only done once per new block */
			if (block_changed != BLOCK_DETECT) {
				block_changed = BLOCK_LP;
				applog(LOG_WARNING, "LONGPOLL detected new block on network, waiting on fresh work");
			} else {
				applog(LOG_INFO, "LONGPOLL received after new block already detected");
				block_changed = BLOCK_NONE;
			}

			convert_to_work(val, rolltime);
			failures = 0;
			json_decref(val);
		} else {
			/* Some pools regularly drop the longpoll request so
			 * only see this as longpoll failure if it happens
			 * immediately and just restart it the rest of the
			 * time. */
			gettimeofday(&end, NULL);
			if (end.tv_sec - start.tv_sec > 30)
				continue;
			if (failures++ < 10) {
				sleep(30);
				applog(LOG_WARNING,
					"longpoll failed for %s, sleeping for 30s", lp_url);
			} else {
				applog(LOG_ERR,
					"longpoll failed for %s, ending thread", lp_url);
				goto out;
			}
		}
	}

out:
	have_longpoll = false;
	tq_freeze(mythr->q);
	if (curl)
		curl_easy_cleanup(curl);

	return NULL;
}

static void stop_longpoll(void)
{
	struct thr_info *thr = &thr_info[longpoll_thr_id];

	thr_info_cancel(thr);
	have_longpoll = false;
}

static void start_longpoll(void)
{
	struct thr_info *thr = &thr_info[longpoll_thr_id];

	tq_thaw(thr->q);
	if (unlikely(thr_info_create(thr, NULL, longpoll_thread, thr)))
		quit(1, "longpoll thread create failed");
	if (opt_debug)
		applog(LOG_DEBUG, "Pushing ping to longpoll thread");
	tq_push(thr_info[longpoll_thr_id].q, &ping);
}

static void restart_longpoll(void)
{
	if (want_longpoll && have_longpoll)
		return;
	stop_longpoll();
	if (want_longpoll)
		start_longpoll();
}

static void *reinit_cpu(void *userdata)
{
	pthread_detach(pthread_self());
#if 0
	struct cgpu_info *cgpu = (struct cgpu_info *)userdata;
	int cpu = cgpu->cpu_gpu;
	long thr_id = ....(long)userdata;
	struct thr_info *thr = &thr_info[thr_id];
	int cpu = dev_from_id(thr_id);

	cpus[cpu].alive = false;
	thr->rolling = thr->cgpu->rolling = 0;
	tq_freeze(thr->q);
	if (!pthread_cancel(*thr->pth))
		pthread_join(*thr->pth, NULL);
	free(thr->q);
	thr->q = tq_new();
	if (!thr->q)
		quit(1, "Failed to tq_new in reinit_cputhread");

	applog(LOG_INFO, "Reinit CPU thread %d", thr_id);

	if (unlikely(thr_info_create(thr, NULL, miner_thread, thr))) {
		applog(LOG_ERR, "thread %d create failed", thr_id);
		return NULL;
	}
	tq_push(thr->q, &ping);

	applog(LOG_WARNING, "Thread %d restarted", thr_id);
#endif
	return NULL;
}

#ifdef HAVE_OPENCL
/* We have only one thread that ever re-initialises GPUs, thus if any GPU
 * init command fails due to a completely wedged GPU, the thread will never
 * return, unable to harm other GPUs. If it does return, it means we only had
 * a soft failure and then the reinit_gpu thread is ready to tackle another
 * GPU */
static void *reinit_gpu(void *userdata)
{
	struct thr_info *mythr = userdata;
	struct cgpu_info *cgpu;
	struct thr_info *thr;
	struct timeval now;
	char name[256];
	int thr_id;
	int gpu;

	pthread_detach(pthread_self());

select_cgpu:
	cgpu = tq_pop(mythr->q, NULL);
	if (!cgpu)
		goto out;

	if (clDevicesNum() != nDevs) {
		applog(LOG_WARNING, "Hardware not reporting same number of active devices, will not attempt to restart GPU");
		goto out;
	}

	gpu = cgpu->cpu_gpu;
	gpu_devices[gpu] = false;

	for (thr_id = 0; thr_id < gpu_threads; thr_id ++) {
		if (dev_from_id(thr_id) != gpu)
			continue;

		thr = &thr_info[thr_id];
		if (!thr) {
			applog(LOG_WARNING, "No reference to thread %d exists", thr_id);
			continue;
		}

		thr->rolling = thr->cgpu->rolling = 0;
		/* Reports the last time we tried to revive a sick GPU */
		gettimeofday(&thr->sick, NULL);
		if (!pthread_cancel(thr->pth)) {
			applog(LOG_WARNING, "Thread %d still exists, killing it off", thr_id);
		} else
			applog(LOG_WARNING, "Thread %d no longer exists", thr_id);
	}

	gpu_devices[gpu] = true;

	for (thr_id = 0; thr_id < gpu_threads; thr_id ++) {
		if (dev_from_id(thr_id) != gpu)
			continue;

		thr = &thr_info[thr_id];

		/* Lose this ram cause we may get stuck here! */
		//tq_freeze(thr->q);

		thr->q = tq_new();
		if (!thr->q)
			quit(1, "Failed to tq_new in reinit_gpu");

		/* Lose this ram cause we may dereference in the dying thread! */
		//free(clState);

		applog(LOG_INFO, "Reinit GPU thread %d", thr_id);
		clStates[thr_id] = initCl(gpu, name, sizeof(name));
		if (!clStates[thr_id]) {
			applog(LOG_ERR, "Failed to reinit GPU thread %d", thr_id);
			goto select_cgpu;
		}
		applog(LOG_INFO, "initCl() finished. Found %s", name);

		if (unlikely(thr_info_create(thr, NULL, gpuminer_thread, thr))) {
			applog(LOG_ERR, "thread %d create failed", thr_id);
			return NULL;
		}
		applog(LOG_WARNING, "Thread %d restarted", thr_id);
	}

	gettimeofday(&now, NULL);
	get_datestamp(cgpu->init, &now);

	for (thr_id = 0; thr_id < gpu_threads; thr_id ++) {
		if (dev_from_id(thr_id) != gpu)
			continue;

		thr = &thr_info[thr_id];
		tq_push(thr->q, &ping);
	}

	goto select_cgpu;
out:
	return NULL;
}
#else
static void *reinit_gpu(void *userdata)
{
}
#endif

static void reinit_device(struct cgpu_info *cgpu)
{
	if (cgpu->is_gpu)
		tq_push(thr_info[gpur_thr_id].q, cgpu);
	else
		tq_push(thr_info[cpur_thr_id].q, cgpu);
}

/* Determine which are the first threads belonging to a device and if they're
 * active */
static bool active_device(int thr_id)
{
	if (thr_id < gpu_threads) {
		if (thr_id >= total_devices)
			return false;
		if (!gpu_devices[dev_from_id(thr_id)])
			return false;
	} else if (thr_id > gpu_threads + num_processors)
		return false;
	return true;
}

/* Makes sure the hashmeter keeps going even if mining threads stall, updates
 * the screen at regular intervals, and restarts threads if they appear to have
 * died. */
static void *watchdog_thread(void *userdata)
{
	const unsigned int interval = opt_log_interval / 2 ? : 1;
	static struct timeval rotate_tv;
	struct timeval zero_tv;

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	memset(&zero_tv, 0, sizeof(struct timeval));
	gettimeofday(&rotate_tv, NULL);

	while (1) {
		int i;
		struct timeval now;

		sleep(interval);
		if (requests_queued() < opt_queue)
			queue_request(NULL, false);

		hashmeter(-1, &zero_tv, 0);

		if (curses_active_locked()) {
			change_logwinsize();
			curses_print_status();
			for (i = 0; i < mining_threads; i++)
				curses_print_devstatus(i);
			clearok(statuswin, true);
			doupdate();
			unlock_curses();
		}

		gettimeofday(&now, NULL);

		for (i = 0; i < total_pools; i++) {
			struct pool *pool = pools[i];

			if (!pool->enabled)
				continue;

			/* Test pool is idle once every minute */
			if (pool->idle && now.tv_sec - pool->tv_idle.tv_sec > 60) {
				gettimeofday(&pool->tv_idle, NULL);
				if (pool_active(pool, true) && pool_tclear(pool, &pool->idle))
					pool_resus(pool);
			}
		}

		if (pool_strategy == POOL_ROTATE && now.tv_sec - rotate_tv.tv_sec > 60 * opt_rotate_period) {
			gettimeofday(&rotate_tv, NULL);
			switch_pools(NULL);
		}

		if (!sched_paused && !should_run()) {
			applog(LOG_WARNING, "Pausing execution as per stop time %02d:%02d scheduled",
			       schedstop.tm.tm_hour, schedstop.tm.tm_min);
			if (!schedstart.enable) {
				quit(0, "Terminating execution as planned");
				break;
			}

			applog(LOG_WARNING, "Will restart execution as scheduled at %02d:%02d",
			       schedstart.tm.tm_hour, schedstart.tm.tm_min);
			sched_paused = true;
			for (i = 0; i < mining_threads; i++) {
				struct thr_info *thr;
				thr = &thr_info[i];

				thr->pause = true;
			}
		} else if (sched_paused && should_run()) {
			applog(LOG_WARNING, "Restarting execution as per start time %02d:%02d scheduled",
				schedstart.tm.tm_hour, schedstart.tm.tm_min);
			if (schedstop.enable)
				applog(LOG_WARNING, "Will pause execution as scheduled at %02d:%02d",
					schedstop.tm.tm_hour, schedstop.tm.tm_min);
			sched_paused = false;

			for (i = 0; i < mining_threads; i++) {
				struct thr_info *thr;
				thr = &thr_info[i];

				/* Don't touch disabled GPUs */
				if (thr->cgpu->is_gpu && !gpu_devices[thr->cgpu->cpu_gpu])
					continue;
				thr->pause = false;
				tq_push(thr->q, &ping);
			}
		}

		for (i = 0; i < gpu_threads; i++) {
			struct thr_info *thr;
			bool *enable;
			int gpu;

			/* Use only one thread per device to determine if the GPU is healthy */
			if (i >= nDevs)
				break;
			thr = &thr_info[i];
			gpu = thr->cgpu->cpu_gpu;
			enable = &gpu_devices[gpu];
#ifdef HAVE_ADL
			if (adl_active && gpus[gpu].has_adl && *enable)
				gpu_autotune(gpu, enable);
			if (opt_debug && gpus[gpu].has_adl) {
				int engineclock = 0, memclock = 0, activity = 0, fanspeed = 0, fanpercent = 0, powertune = 0;
				float temp = 0, vddc = 0;

				if (gpu_stats(gpu, &temp, &engineclock, &memclock, &vddc, &activity, &fanspeed, &fanpercent, &powertune))
					applog(LOG_DEBUG, "%.1f C  F: %d%%(%dRPM)  E: %dMHz  M: %dMhz  V: %.3fV  A: %d%%  P: %d%%",
					temp, fanpercent, fanspeed, engineclock, memclock, vddc, activity, powertune);
			}
#endif
			/* Thread is waiting on getwork or disabled */
			if (thr->getwork || !*enable)
				continue;

			if (gpus[gpu].status != LIFE_WELL && now.tv_sec - thr->last.tv_sec < 60) {
				applog(LOG_ERR, "Thread %d recovered, GPU %d declared WELL!", i, gpu);
				gpus[gpu].status = LIFE_WELL;
			} else if (now.tv_sec - thr->last.tv_sec > 60 && gpus[gpu].status == LIFE_WELL) {
				thr->rolling = thr->cgpu->rolling = 0;
				gpus[gpu].status = LIFE_SICK;
				applog(LOG_ERR, "Thread %d idle for more than 60 seconds, GPU %d declared SICK!", i, gpu);
				gettimeofday(&thr->sick, NULL);
				if (opt_restart) {
					applog(LOG_ERR, "Attempting to restart GPU");
					reinit_device(thr->cgpu);
				}
			} else if (now.tv_sec - thr->last.tv_sec > 600 && gpus[i].status == LIFE_SICK) {
				gpus[gpu].status = LIFE_DEAD;
				applog(LOG_ERR, "Thread %d not responding for more than 10 minutes, GPU %d declared DEAD!", i, gpu);
			} else if (now.tv_sec - thr->sick.tv_sec > 60 && gpus[i].status == LIFE_SICK) {
				/* Attempt to restart a GPU once every minute */
				gettimeofday(&thr->sick, NULL);
				if (opt_restart)
					reinit_device(thr->cgpu);
			}
		}
	}

	return NULL;
}

static void log_print_status(int thr_id)
{
	struct cgpu_info *cgpu;
	char logline[255];

	cgpu = thr_info[thr_id].cgpu;
	get_statline(logline, cgpu);
	applog(LOG_WARNING, "%s", logline);
}

static void print_summary(void)
{
	struct timeval diff;
	int hours, mins, secs, i;
	double utility, efficiency = 0.0;

	timeval_subtract(&diff, &total_tv_end, &total_tv_start);
	hours = diff.tv_sec / 3600;
	mins = (diff.tv_sec % 3600) / 60;
	secs = diff.tv_sec % 60;

	utility = total_accepted / ( total_secs ? total_secs : 1 ) * 60;
	efficiency = total_getworks ? total_accepted * 100.0 / total_getworks : 0.0;

	applog(LOG_WARNING, "\nSummary of runtime statistics:\n");
	applog(LOG_WARNING, "Started at %s", datestamp);
	if (opt_n_threads)
		applog(LOG_WARNING, "CPU hasher algorithm used: %s", algo_names[opt_algo]);
	applog(LOG_WARNING, "Runtime: %d hrs : %d mins : %d secs", hours, mins, secs);
	if (total_secs)
		applog(LOG_WARNING, "Average hashrate: %.1f Megahash/s", total_mhashes_done / total_secs);
	applog(LOG_WARNING, "Queued work requests: %d", total_getworks);
	applog(LOG_WARNING, "Share submissions: %d", total_accepted + total_rejected);
	applog(LOG_WARNING, "Accepted shares: %d", total_accepted);
	applog(LOG_WARNING, "Rejected shares: %d", total_rejected);
	if (total_accepted || total_rejected)
		applog(LOG_WARNING, "Reject ratio: %.1f", (double)(total_rejected * 100) / (double)(total_accepted + total_rejected));
	applog(LOG_WARNING, "Hardware errors: %d", hw_errors);
	applog(LOG_WARNING, "Efficiency (accepted / queued): %.0f%%", efficiency);
	applog(LOG_WARNING, "Utility (accepted shares / min): %.2f/min\n", utility);

	applog(LOG_WARNING, "Discarded work due to new blocks: %d", total_discarded);
	applog(LOG_WARNING, "Stale submissions discarded due to new blocks: %d", total_stale);
	applog(LOG_WARNING, "Unable to get work from server occasions: %d", total_go);
	applog(LOG_WARNING, "Work items generated locally: %d", local_work);
	applog(LOG_WARNING, "Submitting work remotely delay occasions: %d", total_ro);
	applog(LOG_WARNING, "New blocks detected on network: %d\n", new_blocks);

	if (total_pools > 1) {
		for (i = 0; i < total_pools; i++) {
			struct pool *pool = pools[i];

			applog(LOG_WARNING, "Pool: %s", pool->rpc_url);
			applog(LOG_WARNING, " Queued work requests: %d", pool->getwork_requested);
			applog(LOG_WARNING, " Share submissions: %d", pool->accepted + pool->rejected);
			applog(LOG_WARNING, " Accepted shares: %d", pool->accepted);
			applog(LOG_WARNING, " Rejected shares: %d", pool->rejected);
			if (pool->accepted || pool->rejected)
				applog(LOG_WARNING, " Reject ratio: %.1f", (double)(pool->rejected * 100) / (double)(pool->accepted + pool->rejected));
			efficiency = pool->getwork_requested ? pool->accepted * 100.0 / pool->getwork_requested : 0.0;
			applog(LOG_WARNING, " Efficiency (accepted / queued): %.0f%%", efficiency);

			applog(LOG_WARNING, " Discarded work due to new blocks: %d", pool->discarded_work);
			applog(LOG_WARNING, " Stale submissions discarded due to new blocks: %d", pool->stale_shares);
			applog(LOG_WARNING, " Unable to get work from server occasions: %d", pool->getfail_occasions);
			applog(LOG_WARNING, " Submitting work remotely delay occasions: %d\n", pool->remotefail_occasions);
		}
	}

	applog(LOG_WARNING, "Summary of per device statistics:\n");
	for (i = 0; i < mining_threads; i++) {
		if (active_device(i))
			log_print_status(i);
	}

	if (opt_shares)
		applog(LOG_WARNING, "Mined %d accepted shares of %d requested\n", total_accepted, opt_shares);
	fflush(stdout);
	fflush(stderr);
	if (opt_shares > total_accepted)
		quit(1, "Did not successfully mine as many shares as were requested.");
}

void quit(int status, const char *format, ...)
{
	va_list ap;

	disable_curses();

	if (!opt_realquiet && successful_connect)
		print_summary();

	if (format) {
		va_start(ap, format);
		vfprintf(stderr, format, ap);
		va_end(ap);
	}
	fprintf(stderr, "\n");
	fflush(stderr);

	exit(status);
}

char *curses_input(const char *query)
{
	char *input;

	echo();
	input = malloc(255);
	if (!input)
		quit(1, "Failed to malloc input");
	leaveok(logwin, false);
	wlogprint("%s: ", query);
	wgetnstr(logwin, input, 255);
	if (!strlen(input))
		strcpy(input, "-1");
	leaveok(logwin, true);
	noecho();
	return input;
}

static bool input_pool(bool live)
{
	char *url = NULL, *user = NULL, *pass = NULL;
	struct pool *pool = NULL;
	bool ret = false;

	immedok(logwin, true);
	if (total_pools == MAX_POOLS) {
		wlogprint("Reached maximum number of pools.\n");
		goto out;
	}
	wlogprint("Input server details.\n");

	url = curses_input("URL");
	if (!url)
		goto out;

	if (strncmp(url, "http://", 7) &&
	    strncmp(url, "https://", 8)) {
		char *httpinput;

		httpinput = malloc(255);
		if (!httpinput)
			quit(1, "Failed to malloc httpinput");
		strcpy(httpinput, "http://");
		strncat(httpinput, url, 248);
		free(url);
		url = httpinput;
	}

	user = curses_input("Username");
	if (!user)
		goto out;

	pass = curses_input("Password");
	if (!pass)
		goto out;

	pool = calloc(sizeof(struct pool), 1);
	if (!pool)
		quit(1, "Failed to realloc pools in input_pool");
	pool->pool_no = total_pools;
	pool->prio = total_pools;
	if (unlikely(pthread_mutex_init(&pool->pool_lock, NULL)))
		quit (1, "Failed to pthread_mutex_init in input_pool");
	pool->rpc_url = url;
	pool->rpc_user = user;
	pool->rpc_pass = pass;
	pool->rpc_userpass = malloc(strlen(pool->rpc_user) + strlen(pool->rpc_pass) + 2);
	if (!pool->rpc_userpass)
		quit(1, "Failed to malloc userpass");
	sprintf(pool->rpc_userpass, "%s:%s", pool->rpc_user, pool->rpc_pass);

	pool->tv_idle.tv_sec = ~0UL;

	/* Test the pool is not idle if we're live running, otherwise
	 * it will be tested separately */
	ret = true;
	pool->enabled = true;
	if (live && !pool_active(pool, false))
		pool->idle = true;
	pools[total_pools++] = pool;
out:
	immedok(logwin, false);

	if (!ret) {
		if (url)
			free(url);
		if (user)
			free(user);
		if (pass)
			free(pass);
		if (pool)
			free(pool);
	}
	return ret;
}

#if defined(unix)
	static void fork_monitor()
	{
		// Make a pipe: [readFD, writeFD]
		int pfd[2];
		int r = pipe(pfd);
		if (r<0) {
			perror("pipe - failed to create pipe for --monitor");
			exit(1);
		}

		// Make stderr write end of pipe
		fflush(stderr);
		r = dup2(pfd[1], 2);
		if (r<0) {
			perror("dup2 - failed to alias stderr to write end of pipe for --monitor");
			exit(1);
		}
		r = close(pfd[1]);
		if (r<0) {
			perror("close - failed to close write end of pipe for --monitor");
			exit(1);
		}

		// Don't allow a dying monitor to kill the main process
		sighandler_t sr0 = signal(SIGPIPE, SIG_IGN);
		sighandler_t sr1 = signal(SIGPIPE, SIG_IGN);
		if (SIG_ERR==sr0 || SIG_ERR==sr1) {
			perror("signal - failed to edit signal mask for --monitor");
			exit(1);
		}

		// Fork a child process
		r = fork();
		if (r<0) {
			perror("fork - failed to fork child process for --monitor");
			exit(1);
		}

		// Child: launch monitor command
		if (0==r) {
			// Make stdin read end of pipe
			r = dup2(pfd[0], 0);
			if (r<0) {
				perror("dup2 - in child, failed to alias read end of pipe to stdin for --monitor");
				exit(1);
			}
			close(pfd[0]);
			if (r<0) {
				perror("close - in child, failed to close read end of  pipe for --monitor");
				exit(1);
			}

			// Launch user specified command
			execl("/bin/bash", "/bin/bash", "-c", opt_stderr_cmd, (char*)NULL);
			perror("execl - in child failed to exec user specified command for --monitor");
			exit(1);
		}

		// Parent: clean up unused fds and bail
		r = close(pfd[0]);
		if (r<0) {
			perror("close - failed to close read end of pipe for --monitor");
			exit(1);
		}
	}
#endif // defined(unix)

static void enable_curses(void) {
	int x,y;

	lock_curses();
	if (curses_active) {
		unlock_curses();
		return;
	}

	mainwin = initscr();
	getmaxyx(mainwin, y, x);
	statuswin = newwin(logstart, x, 0, 0);
	leaveok(statuswin, true);
	logwin = newwin(y - logcursor, 0, logcursor, 0);
	idlok(logwin, true);
	scrollok(logwin, true);
	leaveok(logwin, true);
	cbreak();
	noecho();
	curses_active = true;
	unlock_curses();
}

int main (int argc, char *argv[])
{
	unsigned int i, pools_active = 0;
	struct block *block, *tmpblock;
	struct work *work, *tmpwork;
	struct sigaction handler;
	struct thr_info *thr;
	char name[256];

	/* This dangerous functions tramples random dynamically allocated
	 * variables so do it before anything at all */
	if (unlikely(curl_global_init(CURL_GLOBAL_ALL)))
		quit(1, "Failed to curl_global_init");

	if (unlikely(pthread_mutex_init(&hash_lock, NULL)))
		quit(1, "Failed to pthread_mutex_init");
	if (unlikely(pthread_mutex_init(&qd_lock, NULL)))
		quit(1, "Failed to pthread_mutex_init");
	if (unlikely(pthread_mutex_init(&curses_lock, NULL)))
		quit(1, "Failed to pthread_mutex_init");
	if (unlikely(pthread_mutex_init(&control_lock, NULL)))
		quit(1, "Failed to pthread_mutex_init");
	if (unlikely(pthread_rwlock_init(&blk_lock, NULL)))
		quit(1, "Failed to pthread_rwlock_init");

	sprintf(packagename, "%s %s", PACKAGE, VERSION);

	init_max_name_len();

	handler.sa_handler = &sighandler;
	sigaction(SIGTERM, &handler, &termhandler);
	sigaction(SIGINT, &handler, &inthandler);

	opt_kernel_path = alloca(PATH_MAX);
	strcpy(opt_kernel_path, CGMINER_PREFIX);
	cgminer_path = alloca(PATH_MAX);
	strcpy(cgminer_path, dirname(argv[0]));
	strcat(cgminer_path, "/");

	// Hack to make cgminer silent when called recursively on WIN32
	int skip_to_bench = 0;
	#if defined(WIN32)
		char buf[32];
		if (GetEnvironmentVariable("CGMINER_BENCH_ALGO", buf, 16))
			skip_to_bench = 1;
	#endif // defined(WIN32)

	block = calloc(sizeof(struct block), 1);
	if (unlikely(!block))
		quit (1, "main OOM");
	for (i = 0; i < 36; i++)
		strcat(block->hash, "0");
	HASH_ADD_STR(blocks, hash, block);
	strcpy(current_block, block->hash);

	// Reckon number of cores in the box
	#if defined(WIN32)

		DWORD system_am;
		DWORD process_am;
		BOOL ok = GetProcessAffinityMask(
			GetCurrentProcess(),
			&system_am,
			&process_am
		);
		if (!ok) {
			applog(LOG_ERR, "couldn't figure out number of processors :(");
			num_processors = 1;
		} else {
			size_t n = 32;
			num_processors = 0;
			while (n--)
				if (process_am & (1<<n))
					++num_processors;
		}

	#else

		num_processors = sysconf(_SC_NPROCESSORS_ONLN);

	#endif /* !WIN32 */

	opt_n_threads = num_processors;

#ifdef HAVE_OPENCL
	if (!skip_to_bench) {
		for (i = 0; i < MAX_GPUDEVICES; i++)
			gpu_devices[i] = false;
		nDevs = clDevicesNum();
		if (nDevs < 0) {
			applog(LOG_ERR, "clDevicesNum returned error, none usable");
			nDevs = 0;
		}
	}
#endif

	/* parse command line */
	opt_register_table(opt_config_table,
			   "Options for both config file and command line");
	opt_register_table(opt_cmdline_table,
			   "Options for command line only");

	opt_parse(&argc, argv, applog_and_exit);
	if (argc != 1)
		quit(1, "Unexpected extra commandline arguments");

	applog(LOG_WARNING, "Started %s", packagename);

	if (opt_nogpu)
		nDevs = 0;

	if (nDevs && !opt_usecpu)
		opt_n_threads = 0;

	strcat(opt_kernel_path, "/");

	if (want_per_device_stats)
		opt_log_output = true;

	if (0<=opt_bench_algo) {
		double rate = bench_algo_stage3(opt_bench_algo);
		if (!skip_to_bench) {
			printf("%.5f (%s)\n", rate, algo_names[opt_bench_algo]);
		} else {
			// Write result to shared memory for parent
			#if defined(WIN32)
				char unique_name[64];
				if (GetEnvironmentVariable("CGMINER_SHARED_MEM", unique_name, 32)) {
					HANDLE map_handle = CreateFileMapping( 
						INVALID_HANDLE_VALUE,   // use paging file
						NULL,                   // default security attributes
						PAGE_READWRITE,         // read/write access
						0,                      // size: high 32-bits
						4096,			// size: low 32-bits
						unique_name		// name of map object
					);
					if (NULL!=map_handle) {
						void *shared_mem = MapViewOfFile( 
							map_handle,	// object to map view of
							FILE_MAP_WRITE, // read/write access
							0,              // high offset:  map from
							0,              // low offset:   beginning
							0		// default: map entire file
						);
						if (NULL!=shared_mem)  
							CopyMemory(shared_mem, &rate, sizeof(rate));
						(void)UnmapViewOfFile(shared_mem); 
					}
					(void)CloseHandle(map_handle); 
				}
			#endif
		}
		exit(0);
	}

	if (opt_kernel) {
		if (strcmp(opt_kernel, "poclbm") && strcmp(opt_kernel, "phatk"))
			quit(1, "Invalid kernel name specified - must be poclbm or phatk");
		if (!strcmp(opt_kernel, "poclbm"))
			chosen_kernel = KL_POCLBM;
		else
			chosen_kernel = KL_PHATK;
	} else
		chosen_kernel = KL_NONE;

	gpu_threads = nDevs * opt_g_threads;
	if (total_devices) {
		if (total_devices > nDevs)
			quit(1, "More devices specified than exist");
		for (i = 0; i < MAX_GPUDEVICES; i++)
			if (gpu_devices[i] && i + 1 > nDevs)
				quit (1, "Command line options set a device that doesn't exist");
	} else {
		for (i = 0; i < nDevs; i++)
			gpu_devices[i] = true;
		total_devices = nDevs;
	}

	if (!gpu_threads && !forced_n_threads) {
		/* Maybe they turned GPU off; restore default CPU threads. */
		opt_n_threads = num_processors;
	}

	if (!opt_n_threads && ! gpu_threads)
		quit(1, "All devices disabled, cannot mine!");

	logcursor = 8;
	gpucursor = logcursor;
	cpucursor = gpucursor + nDevs;
	logstart = cpucursor + (opt_n_threads ? num_processors : 0) + 1;
	logcursor = logstart + 1;

	if (opt_realquiet)
		use_curses = false;

	if (!total_pools) {
		enable_curses();
		applog(LOG_WARNING, "Need to specify at least one pool server.");
		if (!input_pool(false))
			quit(1, "Pool setup failed");
		if (!use_curses)
			disable_curses();
	}

	for (i = 0; i < total_pools; i++) {
		struct pool *pool = pools[i];

		if (!pool->rpc_userpass) {
			if (!pool->rpc_user || !pool->rpc_pass)
				quit(1, "No login credentials supplied for pool %u %s", i, pool->rpc_url);
			pool->rpc_userpass = malloc(strlen(pool->rpc_user) + strlen(pool->rpc_pass) + 2);
			if (!pool->rpc_userpass)
				quit(1, "Failed to malloc userpass");
			sprintf(pool->rpc_userpass, "%s:%s", pool->rpc_user, pool->rpc_pass);
		} else {
			pool->rpc_user = malloc(strlen(pool->rpc_userpass));
			if (!pool->rpc_user)
				quit(1, "Failed to malloc user");
			strcpy(pool->rpc_user, pool->rpc_userpass);
			pool->rpc_user = strtok(pool->rpc_user, ":");
			if (!pool->rpc_user)
				quit(1, "Failed to find colon delimiter in userpass");
		}
	}
	/* Set the currentpool to pool 0 */
	currentpool = pools[0];

#ifdef HAVE_SYSLOG_H
	if (use_syslog)
		openlog(PACKAGE, LOG_PID, LOG_USER);
#endif

	#if defined(unix)
		if (opt_stderr_cmd)
			fork_monitor();
	#endif // defined(unix)

	mining_threads = opt_n_threads + gpu_threads;

	total_threads = mining_threads + 7;
	work_restart = calloc(total_threads, sizeof(*work_restart));
	if (!work_restart)
		quit(1, "Failed to calloc work_restart");

	thr_info = calloc(total_threads, sizeof(*thr));
	if (!thr_info)
		quit(1, "Failed to calloc thr_info");

	/* init workio thread info */
	work_thr_id = mining_threads;
	thr = &thr_info[work_thr_id];
	thr->id = work_thr_id;
	thr->q = tq_new();
	if (!thr->q)
		quit(1, "Failed to tq_new");

	/* start work I/O thread */
	if (thr_info_create(thr, NULL, workio_thread, thr)) 
		quit(1, "workio thread create failed");

	/* init longpoll thread info */
	longpoll_thr_id = mining_threads + 1;
	thr = &thr_info[longpoll_thr_id];
	thr->id = longpoll_thr_id;
	thr->q = tq_new();
	if (!thr->q)
		quit(1, "Failed to tq_new");

	if (opt_n_threads ) {
		cpus = calloc(num_processors, sizeof(struct cgpu_info));
		if (unlikely(!cpus))
			quit(1, "Failed to calloc cpus");
	}

	stage_thr_id = mining_threads + 3;
	thr = &thr_info[stage_thr_id];
	thr->q = tq_new();
	if (!thr->q)
		quit(1, "Failed to tq_new");
	/* start stage thread */
	if (thr_info_create(thr, NULL, stage_thread, thr))
		quit(1, "stage thread create failed");
	pthread_detach(thr->pth);

	/* Create a unique get work queue */
	getq = tq_new();
	if (!getq)
		quit(1, "Failed to create getq");
	/* We use the getq mutex as the staged lock */
	stgd_lock = &getq->mutex;

	/* Test each pool to see if we can retrieve and use work and for what
	 * it supports */
	for (i = 0; i < total_pools; i++) {
		struct pool *pool;

		pool = pools[i];
		pool->enabled = true;
		if (pool_active(pool, false)) {
			if (!currentpool)
				currentpool = pool;
			applog(LOG_INFO, "Pool %d %s active", pool->pool_no, pool->rpc_url);
			pools_active++;
		} else {
			if (pool == currentpool)
				currentpool = NULL;
			applog(LOG_WARNING, "Unable to get work from pool %d %s", pool->pool_no, pool->rpc_url);
			pool->idle = true;
		}
	}

	if (!pools_active) {
		enable_curses();
		applog(LOG_ERR, "No servers were found that could be used to get work from.");
		applog(LOG_ERR, "Please check the details from the list below of the servers you have input");
		applog(LOG_ERR, "Most likely you have input the wrong URL, forgotten to add a port, or have not set up workers");
		for (i = 0; i < total_pools; i++) {
			struct pool *pool;

			pool = pools[i];
			applog(LOG_WARNING, "Pool: %d  URL: %s  User: %s  Password: %s",
			       i, pool->rpc_url, pool->rpc_user, pool->rpc_pass);
		}
		curses_input("Press enter to exit");
		quit(0, "No servers could be used! Exiting.");
	}

	/* If we want longpoll, enable it for the chosen default pool, or, if
	 * the pool does not support longpoll, find the first one that does
	 * and use its longpoll support */
	if (want_longpoll) {
		if (currentpool->hdr_path)
			start_longpoll();
		else {
			for (i = 0; i < total_pools; i++) {
				struct pool *pool;

				pool = pools[i];
				if (pool->hdr_path) {
					struct pool *temp = currentpool;

					currentpool = pool;
					start_longpoll();
					/* Not real blocking, but good enough */
					sleep(1);
					currentpool = temp;
					break;
				}
			}
		}
	}

	gettimeofday(&total_tv_start, NULL);
	gettimeofday(&total_tv_end, NULL);
	get_datestamp(datestamp, &total_tv_start);

#ifdef HAVE_OPENCL
	if (!opt_noadl)
		init_adl(nDevs);
	bool failmessage = false;

	/* start GPU mining threads */
	for (i = 0; i < nDevs * opt_g_threads; i++) {
		int gpu = i % nDevs;
		struct cgpu_info *cgpu;
		struct timeval now;

		gpus[gpu].is_gpu = 1;
		gpus[gpu].cpu_gpu = gpu;

		thr = &thr_info[i];
		thr->id = i;
		cgpu = thr->cgpu = &gpus[gpu];

		thr->q = tq_new();
		if (!thr->q)
			quit(1, "tq_new failed in starting gpu mining threads");

		/* Enable threads for devices set not to mine but disable
		 * their queue in case we wish to enable them later*/
		if (gpu_devices[gpu]) {
			if (opt_debug)
				applog(LOG_DEBUG, "Pushing ping to thread %d", thr->id);

			tq_push(thr->q, &ping);
		}

		applog(LOG_INFO, "Init GPU thread %i", i);
		clStates[i] = initCl(gpu, name, sizeof(name));
		if (!clStates[i]) {
			enable_curses();
			applog(LOG_ERR, "Failed to init GPU thread %d, disabling device %d", i, gpu);
			if (!failmessage) {
				char *buf;

				applog(LOG_ERR, "The most common reason for this failure is cgminer being unable to read the kernel .cl files");
				applog(LOG_ERR, "Alternatively if it has failed on different GPUs, restarting might help.");
				failmessage = true;
				buf = curses_input("Press enter to continue");
				if (buf)
					free(buf);
			}
			gpu_devices[gpu] = false;
			cgpu->status = LIFE_NOSTART;
			continue;
		}
		applog(LOG_INFO, "initCl() finished. Found %s", name);
		gettimeofday(&now, NULL);
		get_datestamp(cgpu->init, &now);

		if (unlikely(thr_info_create(thr, NULL, gpuminer_thread, thr)))
			quit(1, "thread %d create failed", i);
	}

	applog(LOG_INFO, "%d gpu miner threads started", gpu_threads);
#else
	opt_g_threads = 0;
#endif

	/* start CPU mining threads */
	for (i = gpu_threads; i < mining_threads; i++) {
		int cpu = (i - gpu_threads) % num_processors;

		thr = &thr_info[i];

		thr->id = i;
		cpus[cpu].cpu_gpu = cpu;
		thr->cgpu = &cpus[cpu];

		thr->q = tq_new();
		if (!thr->q)
			quit(1, "tq_new failed in starting cpu mining threads");

		thread_reportin(thr);

		if (unlikely(thr_info_create(thr, NULL, miner_thread, thr)))
			quit(1, "thread %d create failed", i);
	}

	applog(LOG_INFO, "%d cpu miner threads started, "
		"using SHA256 '%s' algorithm.",
		opt_n_threads,
		algo_names[opt_algo]);

	if (use_curses)
		enable_curses();

	watchdog_thr_id = mining_threads + 2;
	thr = &thr_info[watchdog_thr_id];
	/* start wakeup thread */
	if (thr_info_create(thr, NULL, watchdog_thread, NULL))
		quit(1, "wakeup thread create failed");

	/* Create curses input thread for keyboard input */
	input_thr_id = mining_threads + 4;
	thr = &thr_info[input_thr_id];
	if (thr_info_create(thr, NULL, input_thread, thr))
		quit(1, "input thread create failed");
	pthread_detach(thr->pth);

	/* Create reinit cpu thread */
	cpur_thr_id = mining_threads + 5;
	thr = &thr_info[cpur_thr_id];
	thr->q = tq_new();
	if (!thr->q)
		quit(1, "tq_new failed for cpur_thr_id");
	if (thr_info_create(thr, NULL, reinit_cpu, thr))
		quit(1, "reinit_cpu thread create failed");

	/* Create reinit gpu thread */
	gpur_thr_id = mining_threads + 6;
	thr = &thr_info[gpur_thr_id];
	thr->q = tq_new();
	if (!thr->q)
		quit(1, "tq_new failed for gpur_thr_id");
	if (thr_info_create(thr, NULL, reinit_gpu, thr))
		quit(1, "reinit_gpu thread create failed");

	/* main loop - simply wait for workio thread to exit */
	pthread_join(thr_info[work_thr_id].pth, NULL);
	applog(LOG_INFO, "workio thread dead, exiting.");

	gettimeofday(&total_tv_end, NULL);
	disable_curses();
	if (!opt_realquiet && successful_connect)
		print_summary();

#ifdef HAVE_OPENCL
	clear_adl(nDevs);
#endif

	if (opt_n_threads)
		free(cpus);

	HASH_ITER(hh, staged_work, work, tmpwork) {
		HASH_DEL(staged_work, work);
		free_work(work);
	}
	HASH_ITER(hh, blocks, block, tmpblock) {
		HASH_DEL(blocks, block);
		free(block);
	}

	curl_global_cleanup();

	return 0;
}

