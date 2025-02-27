/*
 * pgbench.c
 *
 * A simple benchmark program for PostgreSQL
 * Originally written by Tatsuo Ishii and enhanced by many contributors.
 *
 * src/bin/pgbench/pgbench.c
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 * ALL RIGHTS RESERVED;
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written agreement
 * is hereby granted, provided that the above copyright notice and this
 * paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE AUTHOR OR DISTRIBUTORS HAVE BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHOR AND DISTRIBUTORS SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE AUTHOR AND DISTRIBUTORS HAS NO OBLIGATIONS TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 */

#if defined(WIN32) && FD_SETSIZE < 1024
#error FD_SETSIZE needs to have been increased
#endif

#include "postgres_fe.h"

#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>		/* for getrlimit */

/* For testing, PGBENCH_USE_SELECT can be defined to force use of that code */
#if defined(HAVE_PPOLL) && !defined(PGBENCH_USE_SELECT)
#define POLL_USING_PPOLL
#ifdef HAVE_POLL_H
#include <poll.h>
#endif
#else							/* no ppoll(), so use select() */
#define POLL_USING_SELECT
#include <sys/select.h>
#endif

#include "catalog/pg_class_d.h"
#include "common/int.h"
#include "common/logging.h"
#include "common/pg_prng.h"
#include "common/string.h"
#include "common/username.h"
#include "fe_utils/cancel.h"
#include "fe_utils/conditional.h"
#include "fe_utils/option_utils.h"
#include "fe_utils/string_utils.h"
#include "getopt_long.h"
#include "libpq-fe.h"
#include "pgbench.h"
#include "port/pg_bitutils.h"
#include "portability/instr_time.h"

/* X/Open (XSI) requires <math.h> to provide M_PI, but core POSIX does not */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define ERRCODE_T_R_SERIALIZATION_FAILURE  "40001"
#define ERRCODE_T_R_DEADLOCK_DETECTED  "40P01"
#define ERRCODE_UNDEFINED_TABLE  "42P01"

/*
 * Hashing constants
 */
#define FNV_PRIME			UINT64CONST(0x100000001b3)
#define FNV_OFFSET_BASIS	UINT64CONST(0xcbf29ce484222325)
#define MM2_MUL				UINT64CONST(0xc6a4a7935bd1e995)
#define MM2_MUL_TIMES_8		UINT64CONST(0x35253c9ade8f4ca8)
#define MM2_ROT				47

/*
 * Multi-platform socket set implementations
 */

#ifdef POLL_USING_PPOLL
#define SOCKET_WAIT_METHOD "ppoll"

typedef struct socket_set
{
	int			maxfds;			/* allocated length of pollfds[] array */
	int			curfds;			/* number currently in use */
	struct pollfd pollfds[FLEXIBLE_ARRAY_MEMBER];
} socket_set;

#endif							/* POLL_USING_PPOLL */

#ifdef POLL_USING_SELECT
#define SOCKET_WAIT_METHOD "select"

typedef struct socket_set
{
	int			maxfd;			/* largest FD currently set in fds */
	fd_set		fds;
} socket_set;

#endif							/* POLL_USING_SELECT */

/*
 * Multi-platform thread implementations
 */

#ifdef WIN32
/* Use Windows threads */
#include <windows.h>
#define GETERRNO() (_dosmaperr(GetLastError()), errno)
#define THREAD_T HANDLE
#define THREAD_FUNC_RETURN_TYPE unsigned
#define THREAD_FUNC_RETURN return 0
#define THREAD_FUNC_CC __stdcall
#define THREAD_CREATE(handle, function, arg) \
	((*(handle) = (HANDLE) _beginthreadex(NULL, 0, (function), (arg), 0, NULL)) == 0 ? errno : 0)
#define THREAD_JOIN(handle) \
	(WaitForSingleObject(handle, INFINITE) != WAIT_OBJECT_0 ? \
	GETERRNO() : CloseHandle(handle) ? 0 : GETERRNO())
#define THREAD_BARRIER_T SYNCHRONIZATION_BARRIER
#define THREAD_BARRIER_INIT(barrier, n) \
	(InitializeSynchronizationBarrier((barrier), (n), 0) ? 0 : GETERRNO())
#define THREAD_BARRIER_WAIT(barrier) \
	EnterSynchronizationBarrier((barrier), \
								SYNCHRONIZATION_BARRIER_FLAGS_BLOCK_ONLY)
#define THREAD_BARRIER_DESTROY(barrier)
#else
/* Use POSIX threads */
#include "port/pg_pthread.h"
#define THREAD_T pthread_t
#define THREAD_FUNC_RETURN_TYPE void *
#define THREAD_FUNC_RETURN return NULL
#define THREAD_FUNC_CC
#define THREAD_CREATE(handle, function, arg) \
	pthread_create((handle), NULL, (function), (arg))
#define THREAD_JOIN(handle) \
	pthread_join((handle), NULL)
#define THREAD_BARRIER_T pthread_barrier_t
#define THREAD_BARRIER_INIT(barrier, n) \
	pthread_barrier_init((barrier), NULL, (n))
#define THREAD_BARRIER_WAIT(barrier) pthread_barrier_wait((barrier))
#define THREAD_BARRIER_DESTROY(barrier) pthread_barrier_destroy((barrier))
#endif


/********************************************************************
 * some configurable parameters */

#define DEFAULT_INIT_STEPS "dtgvp"	/* default -I setting */
#define ALL_INIT_STEPS "dtgGvpf"	/* all possible steps */

#define LOG_STEP_SECONDS	5	/* seconds between log messages */
#define DEFAULT_NXACTS	10		/* default nxacts */

#define MIN_GAUSSIAN_PARAM		2.0 /* minimum parameter for gauss */

#define MIN_ZIPFIAN_PARAM		1.001	/* minimum parameter for zipfian */
#define MAX_ZIPFIAN_PARAM		1000.0	/* maximum parameter for zipfian */

static int	nxacts = 0;			/* number of transactions per client */
static int	duration = 0;		/* duration in seconds */
static int64 end_time = 0;		/* when to stop in micro seconds, under -T */

/*
 * scaling factor. for example, scale = 10 will make 1000000 tuples in
 * pgbench_accounts table.
 */
static int	scale = 1;

/*
 * fillfactor. for example, fillfactor = 90 will use only 90 percent
 * space during inserts and leave 10 percent free.
 */
static int	fillfactor = 100;

/*
 * use unlogged tables?
 */
static bool unlogged_tables = false;

/*
 * log sampling rate (1.0 = log everything, 0.0 = option not given)
 */
static double sample_rate = 0.0;

/*
 * When threads are throttled to a given rate limit, this is the target delay
 * to reach that rate in usec.  0 is the default and means no throttling.
 */
static double throttle_delay = 0;

/*
 * Transactions which take longer than this limit (in usec) are counted as
 * late, and reported as such, although they are completed anyway. When
 * throttling is enabled, execution time slots that are more than this late
 * are skipped altogether, and counted separately.
 */
static int64 latency_limit = 0;

/*
 * tablespace selection
 */
static char *tablespace = NULL;
static char *index_tablespace = NULL;

/*
 * Number of "pgbench_accounts" partitions.  0 is the default and means no
 * partitioning.
 */
static int	partitions = 0;

/* partitioning strategy for "pgbench_accounts" */
typedef enum
{
	PART_NONE,					/* no partitioning */
	PART_RANGE,					/* range partitioning */
	PART_HASH,					/* hash partitioning */
} partition_method_t;

static partition_method_t partition_method = PART_NONE;
static const char *const PARTITION_METHOD[] = {"none", "range", "hash"};

/* random seed used to initialize base_random_sequence */
static int64 random_seed = -1;

/*
 * end of configurable parameters
 *********************************************************************/

#define nbranches	1			/* Makes little sense to change this.  Change
								 * -s instead */
#define ntellers	10
#define naccounts	100000

/*
 * The scale factor at/beyond which 32bit integers are incapable of storing
 * 64bit values.
 *
 * Although the actual threshold is 21474, we use 20000 because it is easier to
 * document and remember, and isn't that far away from the real threshold.
 */
#define SCALE_32BIT_THRESHOLD 20000

static bool use_log;			/* log transaction latencies to a file */
static bool use_quiet;			/* quiet logging onto stderr */
static int	agg_interval;		/* log aggregates instead of individual
								 * transactions */
static bool per_script_stats = false;	/* whether to collect stats per script */
static int	progress = 0;		/* thread progress report every this seconds */
static bool progress_timestamp = false; /* progress report with Unix time */
static int	nclients = 1;		/* number of clients */
static int	nthreads = 1;		/* number of threads */
static bool is_connect;			/* establish connection for each transaction */
static bool report_per_command = false; /* report per-command latencies,
										 * retries after errors and failures
										 * (errors without retrying) */
static int	main_pid;			/* main process id used in log filename */

/*
 * There are different types of restrictions for deciding that the current
 * transaction with a serialization/deadlock error can no longer be retried and
 * should be reported as failed:
 * - max_tries (--max-tries) can be used to limit the number of tries;
 * - latency_limit (-L) can be used to limit the total time of tries;
 * - duration (-T) can be used to limit the total benchmark time.
 *
 * They can be combined together, and you need to use at least one of them to
 * retry the transactions with serialization/deadlock errors. If none of them is
 * used, the default value of max_tries is 1 and such transactions will not be
 * retried.
 */

/*
 * We cannot retry a transaction after the serialization/deadlock error if its
 * number of tries reaches this maximum; if its value is zero, it is not used.
 */
static uint32 max_tries = 1;

static bool failures_detailed = false;	/* whether to group failures in
										 * reports or logs by basic types */

static const char *pghost = NULL;
static const char *pgport = NULL;
static const char *username = NULL;
static const char *dbName = NULL;
static char *logfile_prefix = NULL;
static const char *progname;

#define WSEP '@'				/* weight separator */

static volatile sig_atomic_t timer_exceeded = false;	/* flag from signal
														 * handler */

/*
 * We don't want to allocate variables one by one; for efficiency, add a
 * constant margin each time it overflows.
 */
#define VARIABLES_ALLOC_MARGIN	8

/*
 * Variable definitions.
 *
 * If a variable only has a string value, "svalue" is that value, and value is
 * "not set".  If the value is known, "value" contains the value (in any
 * variant).
 *
 * In this case "svalue" contains the string equivalent of the value, if we've
 * had occasion to compute that, or NULL if we haven't.
 */
typedef struct
{
	char	   *name;			/* variable's name */
	char	   *svalue;			/* its value in string form, if known */
	PgBenchValue value;			/* actual variable's value */
} Variable;

/*
 * Data structure for client variables.
 */
typedef struct
{
	Variable   *vars;			/* array of variable definitions */
	int			nvars;			/* number of variables */

	/*
	 * The maximum number of variables that we can currently store in 'vars'
	 * without having to reallocate more space. We must always have max_vars
	 * >= nvars.
	 */
	int			max_vars;

	bool		vars_sorted;	/* are variables sorted by name? */
} Variables;

#define MAX_SCRIPTS		128		/* max number of SQL scripts allowed */
#define SHELL_COMMAND_SIZE	256 /* maximum size allowed for shell command */

/*
 * Simple data structure to keep stats about something.
 *
 * XXX probably the first value should be kept and used as an offset for
 * better numerical stability...
 */
typedef struct SimpleStats
{
	int64		count;			/* how many values were encountered */
	double		min;			/* the minimum seen */
	double		max;			/* the maximum seen */
	double		sum;			/* sum of values */
	double		sum2;			/* sum of squared values */
} SimpleStats;

/*
 * The instr_time type is expensive when dealing with time arithmetic.  Define
 * a type to hold microseconds instead.  Type int64 is good enough for about
 * 584500 years.
 */
typedef int64 pg_time_usec_t;

/*
 * Data structure to hold various statistics: per-thread and per-script stats
 * are maintained and merged together.
 */
typedef struct StatsData
{
	pg_time_usec_t start_time;	/* interval start time, for aggregates */

	/*----------
	 * Transactions are counted depending on their execution and outcome.
	 * First a transaction may have started or not: skipped transactions occur
	 * under --rate and --latency-limit when the client is too late to execute
	 * them. Secondly, a started transaction may ultimately succeed or fail,
	 * possibly after some retries when --max-tries is not one. Thus
	 *
	 * the number of all transactions =
	 *   'skipped' (it was too late to execute them) +
	 *   'cnt' (the number of successful transactions) +
	 *   'failed' (the number of failed transactions).
	 *
	 * A successful transaction can have several unsuccessful tries before a
	 * successful run. Thus
	 *
	 * 'cnt' (the number of successful transactions) =
	 *   successfully retried transactions (they got a serialization or a
	 *                                      deadlock error(s), but were
	 *                                      successfully retried from the very
	 *                                      beginning) +
	 *   directly successful transactions (they were successfully completed on
	 *                                     the first try).
	 *
	 * A failed transaction is defined as unsuccessfully retried transactions.
	 * It can be one of two types:
	 *
	 * failed (the number of failed transactions) =
	 *   'serialization_failures' (they got a serialization error and were not
	 *                             successfully retried) +
	 *   'deadlock_failures' (they got a deadlock error and were not
	 *                        successfully retried).
	 *
	 * If the transaction was retried after a serialization or a deadlock
	 * error this does not guarantee that this retry was successful. Thus
	 *
	 * 'retries' (number of retries) =
	 *   number of retries in all retried transactions =
	 *   number of retries in (successfully retried transactions +
	 *                         failed transactions);
	 *
	 * 'retried' (number of all retried transactions) =
	 *   successfully retried transactions +
	 *   failed transactions.
	 *----------
	 */
	int64		cnt;			/* number of successful transactions, not
								 * including 'skipped' */
	int64		skipped;		/* number of transactions skipped under --rate
								 * and --latency-limit */
	int64		retries;		/* number of retries after a serialization or
								 * a deadlock error in all the transactions */
	int64		retried;		/* number of all transactions that were
								 * retried after a serialization or a deadlock
								 * error (perhaps the last try was
								 * unsuccessful) */
	int64		serialization_failures; /* number of transactions that were
										 * not successfully retried after a
										 * serialization error */
	int64		deadlock_failures;	/* number of transactions that were not
									 * successfully retried after a deadlock
									 * error */
	SimpleStats latency;
	SimpleStats lag;
} StatsData;

/*
 * For displaying Unix epoch timestamps, as some time functions may have
 * another reference.
 */
static pg_time_usec_t epoch_shift;

/*
 * Error status for errors during script execution.
 */
typedef enum EStatus
{
	ESTATUS_NO_ERROR = 0,
	ESTATUS_META_COMMAND_ERROR,

	/* SQL errors */
	ESTATUS_SERIALIZATION_ERROR,
	ESTATUS_DEADLOCK_ERROR,
	ESTATUS_OTHER_SQL_ERROR,
} EStatus;

/*
 * Transaction status at the end of a command.
 */
typedef enum TStatus
{
	TSTATUS_IDLE,
	TSTATUS_IN_BLOCK,
	TSTATUS_CONN_ERROR,
	TSTATUS_OTHER_ERROR,
} TStatus;

/* Various random sequences are initialized from this one. */
static pg_prng_state base_random_sequence;

/* Synchronization barrier for start and connection */
static THREAD_BARRIER_T barrier;

/*
 * Connection state machine states.
 */
typedef enum
{
	/*
	 * The client must first choose a script to execute.  Once chosen, it can
	 * either be throttled (state CSTATE_PREPARE_THROTTLE under --rate), start
	 * right away (state CSTATE_START_TX) or not start at all if the timer was
	 * exceeded (state CSTATE_FINISHED).
	 */
	CSTATE_CHOOSE_SCRIPT,

	/*
	 * CSTATE_START_TX performs start-of-transaction processing.  Establishes
	 * a new connection for the transaction in --connect mode, records the
	 * transaction start time, and proceed to the first command.
	 *
	 * Note: once a script is started, it will either error or run till its
	 * end, where it may be interrupted. It is not interrupted while running,
	 * so pgbench --time is to be understood as tx are allowed to start in
	 * that time, and will finish when their work is completed.
	 */
	CSTATE_START_TX,

	/*
	 * In CSTATE_PREPARE_THROTTLE state, we calculate when to begin the next
	 * transaction, and advance to CSTATE_THROTTLE.  CSTATE_THROTTLE state
	 * sleeps until that moment, then advances to CSTATE_START_TX, or
	 * CSTATE_FINISHED if the next transaction would start beyond the end of
	 * the run.
	 */
	CSTATE_PREPARE_THROTTLE,
	CSTATE_THROTTLE,

	/*
	 * We loop through these states, to process each command in the script:
	 *
	 * CSTATE_START_COMMAND starts the execution of a command.  On a SQL
	 * command, the command is sent to the server, and we move to
	 * CSTATE_WAIT_RESULT state unless in pipeline mode. On a \sleep
	 * meta-command, the timer is set, and we enter the CSTATE_SLEEP state to
	 * wait for it to expire. Other meta-commands are executed immediately. If
	 * the command about to start is actually beyond the end of the script,
	 * advance to CSTATE_END_TX.
	 *
	 * CSTATE_WAIT_RESULT waits until we get a result set back from the server
	 * for the current command.
	 *
	 * CSTATE_SLEEP waits until the end of \sleep.
	 *
	 * CSTATE_END_COMMAND records the end-of-command timestamp, increments the
	 * command counter, and loops back to CSTATE_START_COMMAND state.
	 *
	 * CSTATE_SKIP_COMMAND is used by conditional branches which are not
	 * executed. It quickly skip commands that do not need any evaluation.
	 * This state can move forward several commands, till there is something
	 * to do or the end of the script.
	 */
	CSTATE_START_COMMAND,
	CSTATE_WAIT_RESULT,
	CSTATE_SLEEP,
	CSTATE_END_COMMAND,
	CSTATE_SKIP_COMMAND,

	/*
	 * States for failed commands.
	 *
	 * If the SQL/meta command fails, in CSTATE_ERROR clean up after an error:
	 * (1) clear the conditional stack; (2) if we have an unterminated
	 * (possibly failed) transaction block, send the rollback command to the
	 * server and wait for the result in CSTATE_WAIT_ROLLBACK_RESULT.  If
	 * something goes wrong with rolling back, go to CSTATE_ABORTED.
	 *
	 * But if everything is ok we are ready for future transactions: if this
	 * is a serialization or deadlock error and we can re-execute the
	 * transaction from the very beginning, go to CSTATE_RETRY; otherwise go
	 * to CSTATE_FAILURE.
	 *
	 * In CSTATE_RETRY report an error, set the same parameters for the
	 * transaction execution as in the previous tries and process the first
	 * transaction command in CSTATE_START_COMMAND.
	 *
	 * In CSTATE_FAILURE report a failure, set the parameters for the
	 * transaction execution as they were before the first run of this
	 * transaction (except for a random state) and go to CSTATE_END_TX to
	 * complete this transaction.
	 */
	CSTATE_ERROR,
	CSTATE_WAIT_ROLLBACK_RESULT,
	CSTATE_RETRY,
	CSTATE_FAILURE,

	/*
	 * CSTATE_END_TX performs end-of-transaction processing.  It calculates
	 * latency, and logs the transaction.  In --connect mode, it closes the
	 * current connection.
	 *
	 * Then either starts over in CSTATE_CHOOSE_SCRIPT, or enters
	 * CSTATE_FINISHED if we have no more work to do.
	 */
	CSTATE_END_TX,

	/*
	 * Final states.  CSTATE_ABORTED means that the script execution was
	 * aborted because a command failed, CSTATE_FINISHED means success.
	 */
	CSTATE_ABORTED,
	CSTATE_FINISHED,
} ConnectionStateEnum;

/*
 * Connection state.
 */
typedef struct
{
	PGconn	   *con;			/* connection handle to DB */
	int			id;				/* client No. */
	ConnectionStateEnum state;	/* state machine's current state. */
	ConditionalStack cstack;	/* enclosing conditionals state */

	/*
	 * Separate randomness for each client. This is used for random functions
	 * PGBENCH_RANDOM_* during the execution of the script.
	 */
	pg_prng_state cs_func_rs;

	int			use_file;		/* index in sql_script for this client */
	int			command;		/* command number in script */
	int			num_syncs;		/* number of ongoing sync commands */

	/* client variables */
	Variables	variables;

	/* various times about current transaction in microseconds */
	pg_time_usec_t txn_scheduled;	/* scheduled start time of transaction */
	pg_time_usec_t sleep_until; /* scheduled start time of next cmd */
	pg_time_usec_t txn_begin;	/* used for measuring schedule lag times */
	pg_time_usec_t stmt_begin;	/* used for measuring statement latencies */

	/* whether client prepared each command of each script */
	bool	  **prepared;

	/*
	 * For processing failures and repeating transactions with serialization
	 * or deadlock errors:
	 */
	EStatus		estatus;		/* the error status of the current transaction
								 * execution; this is ESTATUS_NO_ERROR if
								 * there were no errors */
	pg_prng_state random_state; /* random state */
	uint32		tries;			/* how many times have we already tried the
								 * current transaction? */

	/* per client collected stats */
	int64		cnt;			/* client transaction count, for -t; skipped
								 * and failed transactions are also counted
								 * here */
} CState;

/*
 * Thread state
 */
typedef struct
{
	int			tid;			/* thread id */
	THREAD_T	thread;			/* thread handle */
	CState	   *state;			/* array of CState */
	int			nstate;			/* length of state[] */

	/*
	 * Separate randomness for each thread. Each thread option uses its own
	 * random state to make all of them independent of each other and
	 * therefore deterministic at the thread level.
	 */
	pg_prng_state ts_choose_rs; /* random state for selecting a script */
	pg_prng_state ts_throttle_rs;	/* random state for transaction throttling */
	pg_prng_state ts_sample_rs; /* random state for log sampling */

	int64		throttle_trigger;	/* previous/next throttling (us) */
	FILE	   *logfile;		/* where to log, or NULL */

	/* per thread collected stats in microseconds */
	pg_time_usec_t create_time; /* thread creation time */
	pg_time_usec_t started_time;	/* thread is running */
	pg_time_usec_t bench_start; /* thread is benchmarking */
	pg_time_usec_t conn_duration;	/* cumulated connection and disconnection
									 * delays */

	StatsData	stats;
	int64		latency_late;	/* count executed but late transactions */
} TState;

/*
 * queries read from files
 */
#define SQL_COMMAND		1
#define META_COMMAND	2

/*
 * max number of backslash command arguments or SQL variables,
 * including the command or SQL statement itself
 */
#define MAX_ARGS		256

typedef enum MetaCommand
{
	META_NONE,					/* not a known meta-command */
	META_SET,					/* \set */
	META_SETSHELL,				/* \setshell */
	META_SHELL,					/* \shell */
	META_SLEEP,					/* \sleep */
	META_GSET,					/* \gset */
	META_ASET,					/* \aset */
	META_IF,					/* \if */
	META_ELIF,					/* \elif */
	META_ELSE,					/* \else */
	META_ENDIF,					/* \endif */
	META_STARTPIPELINE,			/* \startpipeline */
	META_SYNCPIPELINE,			/* \syncpipeline */
	META_ENDPIPELINE,			/* \endpipeline */
} MetaCommand;

typedef enum QueryMode
{
	QUERY_SIMPLE,				/* simple query */
	QUERY_EXTENDED,				/* extended query */
	QUERY_PREPARED,				/* extended query with prepared statements */
	NUM_QUERYMODE
} QueryMode;

static QueryMode querymode = QUERY_SIMPLE;
static const char *const QUERYMODE[] = {"simple", "extended", "prepared"};

/*
 * struct Command represents one command in a script.
 *
 * lines		The raw, possibly multi-line command text.  Variable substitution
 *				not applied.
 * first_line	A short, single-line extract of 'lines', for error reporting.
 * type			SQL_COMMAND or META_COMMAND
 * meta			The type of meta-command, with META_NONE/GSET/ASET if command
 *				is SQL.
 * argc			Number of arguments of the command, 0 if not yet processed.
 * argv			Command arguments, the first of which is the command or SQL
 *				string itself.  For SQL commands, after post-processing
 *				argv[0] is the same as 'lines' with variables substituted.
 * prepname		The name that this command is prepared under, in prepare mode
 * varprefix	SQL commands terminated with \gset or \aset have this set
 *				to a non NULL value.  If nonempty, it's used to prefix the
 *				variable name that receives the value.
 * aset			do gset on all possible queries of a combined query (\;).
 * expr			Parsed expression, if needed.
 * stats		Time spent in this command.
 * retries		Number of retries after a serialization or deadlock error in the
 *				current command.
 * failures		Number of errors in the current command that were not retried.
 */
typedef struct Command
{
	PQExpBufferData lines;
	char	   *first_line;
	int			type;
	MetaCommand meta;
	int			argc;
	char	   *argv[MAX_ARGS];
	char	   *prepname;
	char	   *varprefix;
	PgBenchExpr *expr;
	SimpleStats stats;
	int64		retries;
	int64		failures;
} Command;

typedef struct ParsedScript
{
	const char *desc;			/* script descriptor (eg, file name) */
	int			weight;			/* selection weight */
	Command   **commands;		/* NULL-terminated array of Commands */
	StatsData	stats;			/* total time spent in script */
} ParsedScript;

static ParsedScript sql_script[MAX_SCRIPTS];	/* SQL script files */
static int	num_scripts;		/* number of scripts in sql_script[] */
static int64 total_weight = 0;

static bool verbose_errors = false; /* print verbose messages of all errors */

static bool exit_on_abort = false;	/* exit when any client is aborted */

/* Builtin test scripts */
typedef struct BuiltinScript
{
	const char *name;			/* very short name for -b ... */
	const char *desc;			/* short description */
	const char *script;			/* actual pgbench script */
} BuiltinScript;

static const BuiltinScript builtin_script[] =
{
	{
		"tpcb-like",
		"<builtin: TPC-B (sort of)>",
		"\\set aid random(1, " CppAsString2(naccounts) " * :scale)\n"
		"\\set bid random(1, " CppAsString2(nbranches) " * :scale)\n"
		"\\set tid random(1, " CppAsString2(ntellers) " * :scale)\n"
		"\\set delta random(-5000, 5000)\n"
		"BEGIN;\n"
		"UPDATE pgbench_accounts SET abalance = abalance + :delta WHERE aid = :aid;\n"
		"SELECT abalance FROM pgbench_accounts WHERE aid = :aid;\n"
		"UPDATE pgbench_tellers SET tbalance = tbalance + :delta WHERE tid = :tid;\n"
		"UPDATE pgbench_branches SET bbalance = bbalance + :delta WHERE bid = :bid;\n"
		"INSERT INTO pgbench_history (tid, bid, aid, delta, mtime) VALUES (:tid, :bid, :aid, :delta, CURRENT_TIMESTAMP);\n"
		"END;\n"
	},
	{
		"simple-update",
		"<builtin: simple update>",
		"\\set aid random(1, " CppAsString2(naccounts) " * :scale)\n"
		"\\set bid random(1, " CppAsString2(nbranches) " * :scale)\n"
		"\\set tid random(1, " CppAsString2(ntellers) " * :scale)\n"
		"\\set delta random(-5000, 5000)\n"
		"BEGIN;\n"
		"UPDATE pgbench_accounts SET abalance = abalance + :delta WHERE aid = :aid;\n"
		"SELECT abalance FROM pgbench_accounts WHERE aid = :aid;\n"
		"INSERT INTO pgbench_history (tid, bid, aid, delta, mtime) VALUES (:tid, :bid, :aid, :delta, CURRENT_TIMESTAMP);\n"
		"END;\n"
	},
	{
		"select-only",
		"<builtin: select only>",
		"\\set aid random(1, " CppAsString2(naccounts) " * :scale)\n"
		"SELECT abalance FROM pgbench_accounts WHERE aid = :aid;\n"
	}
};


/* Function prototypes */
static void setNullValue(PgBenchValue *pv);
static void setBoolValue(PgBenchValue *pv, bool bval);
static void setIntValue(PgBenchValue *pv, int64 ival);
static void setDoubleValue(PgBenchValue *pv, double dval);
static bool evaluateExpr(CState *st, PgBenchExpr *expr,
						 PgBenchValue *retval);
static ConnectionStateEnum executeMetaCommand(CState *st, pg_time_usec_t *now);
static void doLog(TState *thread, CState *st,
				  StatsData *agg, bool skipped, double latency, double lag);
static void processXactStats(TState *thread, CState *st, pg_time_usec_t *now,
							 bool skipped, StatsData *agg);
static void addScript(const ParsedScript *script);
static THREAD_FUNC_RETURN_TYPE THREAD_FUNC_CC threadRun(void *arg);
static void finishCon(CState *st);
static void setalarm(int seconds);
static socket_set *alloc_socket_set(int count);
static void free_socket_set(socket_set *sa);
static void clear_socket_set(socket_set *sa);
static void add_socket_to_set(socket_set *sa, int fd, int idx);
static int	wait_on_socket_set(socket_set *sa, int64 usecs);
static bool socket_has_input(socket_set *sa, int fd, int idx);

/* callback used to build rows for COPY during data loading */
typedef void (*initRowMethod) (PQExpBufferData *sql, int64 curr);

/* callback functions for our flex lexer */
static const PsqlScanCallbacks pgbench_callbacks = {
	NULL,						/* don't need get_variable functionality */
};

static char
get_table_relkind(PGconn *con, const char *table)
{
	PGresult   *res;
	char	   *val;
	char		relkind;
	const char *params[1] = {table};
	const char *sql =
		"SELECT relkind FROM pg_catalog.pg_class WHERE oid=$1::pg_catalog.regclass";

	res = PQexecParams(con, sql, 1, NULL, params, NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("query failed: %s", PQerrorMessage(con));
		pg_log_error_detail("Query was: %s", sql);
		exit(1);
	}
	val = PQgetvalue(res, 0, 0);
	Assert(strlen(val) == 1);
	relkind = val[0];
	PQclear(res);

	return relkind;
}

static inline pg_time_usec_t
pg_time_now(void)
{
	instr_time	now;

	INSTR_TIME_SET_CURRENT(now);

	return (pg_time_usec_t) INSTR_TIME_GET_MICROSEC(now);
}

static inline void
pg_time_now_lazy(pg_time_usec_t *now)
{
	if ((*now) == 0)
		(*now) = pg_time_now();
}

#define PG_TIME_GET_DOUBLE(t) (0.000001 * (t))

static void
usage(void)
{
	printf("%s is a benchmarking tool for PostgreSQL.\n\n"
		   "Usage:\n"
		   "  %s [OPTION]... [DBNAME]\n"
		   "\nInitialization options:\n"
		   "  -i, --initialize         invokes initialization mode\n"
		   "  -I, --init-steps=[" ALL_INIT_STEPS "]+ (default \"" DEFAULT_INIT_STEPS "\")\n"
		   "                           run selected initialization steps, in the specified order\n"
		   "                           d: drop any existing pgbench tables\n"
		   "                           t: create the tables used by the standard pgbench scenario\n"
		   "                           g: generate data, client-side\n"
		   "                           G: generate data, server-side\n"
		   "                           v: invoke VACUUM on the standard tables\n"
		   "                           p: create primary key indexes on the standard tables\n"
		   "                           f: create foreign keys between the standard tables\n"
		   "  -F, --fillfactor=NUM     set fill factor\n"
		   "  -n, --no-vacuum          do not run VACUUM during initialization\n"
		   "  -q, --quiet              quiet logging (one message each 5 seconds)\n"
		   "  -s, --scale=NUM          scaling factor\n"
		   "  --foreign-keys           create foreign key constraints between tables\n"
		   "  --index-tablespace=TABLESPACE\n"
		   "                           create indexes in the specified tablespace\n"
		   "  --partition-method=(range|hash)\n"
		   "                           partition pgbench_accounts with this method (default: range)\n"
		   "  --partitions=NUM         partition pgbench_accounts into NUM parts (default: 0)\n"
		   "  --tablespace=TABLESPACE  create tables in the specified tablespace\n"
		   "  --unlogged-tables        create tables as unlogged tables\n"
		   "\nOptions to select what to run:\n"
		   "  -b, --builtin=NAME[@W]   add builtin script NAME weighted at W (default: 1)\n"
		   "                           (use \"-b list\" to list available scripts)\n"
		   "  -f, --file=FILENAME[@W]  add script FILENAME weighted at W (default: 1)\n"
		   "  -N, --skip-some-updates  skip updates of pgbench_tellers and pgbench_branches\n"
		   "                           (same as \"-b simple-update\")\n"
		   "  -S, --select-only        perform SELECT-only transactions\n"
		   "                           (same as \"-b select-only\")\n"
		   "\nBenchmarking options:\n"
		   "  -c, --client=NUM         number of concurrent database clients (default: 1)\n"
		   "  -C, --connect            establish new connection for each transaction\n"
		   "  -D, --define=VARNAME=VALUE\n"
		   "                           define variable for use by custom script\n"
		   "  -j, --jobs=NUM           number of threads (default: 1)\n"
		   "  -l, --log                write transaction times to log file\n"
		   "  -L, --latency-limit=NUM  count transactions lasting more than NUM ms as late\n"
		   "  -M, --protocol=simple|extended|prepared\n"
		   "                           protocol for submitting queries (default: simple)\n"
		   "  -n, --no-vacuum          do not run VACUUM before tests\n"
		   "  -P, --progress=NUM       show thread progress report every NUM seconds\n"
		   "  -r, --report-per-command report latencies, failures, and retries per command\n"
		   "  -R, --rate=NUM           target rate in transactions per second\n"
		   "  -s, --scale=NUM          report this scale factor in output\n"
		   "  -t, --transactions=NUM   number of transactions each client runs (default: 10)\n"
		   "  -T, --time=NUM           duration of benchmark test in seconds\n"
		   "  -v, --vacuum-all         vacuum all four standard tables before tests\n"
		   "  --aggregate-interval=NUM aggregate data over NUM seconds\n"
		   "  --exit-on-abort          exit when any client is aborted\n"
		   "  --failures-detailed      report the failures grouped by basic types\n"
		   "  --log-prefix=PREFIX      prefix for transaction time log file\n"
		   "                           (default: \"pgbench_log\")\n"
		   "  --max-tries=NUM          max number of tries to run transaction (default: 1)\n"
		   "  --progress-timestamp     use Unix epoch timestamps for progress\n"
		   "  --random-seed=SEED       set random seed (\"time\", \"rand\", integer)\n"
		   "  --sampling-rate=NUM      fraction of transactions to log (e.g., 0.01 for 1%%)\n"
		   "  --show-script=NAME       show builtin script code, then exit\n"
		   "  --verbose-errors         print messages of all errors\n"
		   "\nCommon options:\n"
		   "  --debug                  print debugging output\n"
		   "  -d, --dbname=DBNAME      database name to connect to\n"
		   "  -h, --host=HOSTNAME      database server host or socket directory\n"
		   "  -p, --port=PORT          database server port number\n"
		   "  -U, --username=USERNAME  connect as specified database user\n"
		   "  -V, --version            output version information, then exit\n"
		   "  -?, --help               show this help, then exit\n"
		   "\n"
		   "Report bugs to <%s>.\n"
		   "%s home page: <%s>\n",
		   progname, progname, PACKAGE_BUGREPORT, PACKAGE_NAME, PACKAGE_URL);
}

/* return whether str matches "^\s*[-+]?[0-9]+$" */
static bool
is_an_int(const char *str)
{
	const char *ptr = str;

	/* skip leading spaces; cast is consistent with strtoint64 */
	while (*ptr && isspace((unsigned char) *ptr))
		ptr++;

	/* skip sign */
	if (*ptr == '+' || *ptr == '-')
		ptr++;

	/* at least one digit */
	if (*ptr && !isdigit((unsigned char) *ptr))
		return false;

	/* eat all digits */
	while (*ptr && isdigit((unsigned char) *ptr))
		ptr++;

	/* must have reached end of string */
	return *ptr == '\0';
}


/*
 * strtoint64 -- convert a string to 64-bit integer
 *
 * This function is a slightly modified version of pg_strtoint64() from
 * src/backend/utils/adt/numutils.c.
 *
 * The function returns whether the conversion worked, and if so
 * "*result" is set to the result.
 *
 * If not errorOK, an error message is also printed out on errors.
 */
bool
strtoint64(const char *str, bool errorOK, int64 *result)
{
	const char *ptr = str;
	int64		tmp = 0;
	bool		neg = false;

	/*
	 * Do our own scan, rather than relying on sscanf which might be broken
	 * for long long.
	 *
	 * As INT64_MIN can't be stored as a positive 64 bit integer, accumulate
	 * value as a negative number.
	 */

	/* skip leading spaces */
	while (*ptr && isspace((unsigned char) *ptr))
		ptr++;

	/* handle sign */
	if (*ptr == '-')
	{
		ptr++;
		neg = true;
	}
	else if (*ptr == '+')
		ptr++;

	/* require at least one digit */
	if (unlikely(!isdigit((unsigned char) *ptr)))
		goto invalid_syntax;

	/* process digits */
	while (*ptr && isdigit((unsigned char) *ptr))
	{
		int8		digit = (*ptr++ - '0');

		if (unlikely(pg_mul_s64_overflow(tmp, 10, &tmp)) ||
			unlikely(pg_sub_s64_overflow(tmp, digit, &tmp)))
			goto out_of_range;
	}

	/* allow trailing whitespace, but not other trailing chars */
	while (*ptr != '\0' && isspace((unsigned char) *ptr))
		ptr++;

	if (unlikely(*ptr != '\0'))
		goto invalid_syntax;

	if (!neg)
	{
		if (unlikely(tmp == PG_INT64_MIN))
			goto out_of_range;
		tmp = -tmp;
	}

	*result = tmp;
	return true;

out_of_range:
	if (!errorOK)
		pg_log_error("value \"%s\" is out of range for type bigint", str);
	return false;

invalid_syntax:
	if (!errorOK)
		pg_log_error("invalid input syntax for type bigint: \"%s\"", str);
	return false;
}

/* convert string to double, detecting overflows/underflows */
bool
strtodouble(const char *str, bool errorOK, double *dv)
{
	char	   *end;

	errno = 0;
	*dv = strtod(str, &end);

	if (unlikely(errno != 0))
	{
		if (!errorOK)
			pg_log_error("value \"%s\" is out of range for type double", str);
		return false;
	}

	if (unlikely(end == str || *end != '\0'))
	{
		if (!errorOK)
			pg_log_error("invalid input syntax for type double: \"%s\"", str);
		return false;
	}
	return true;
}

/*
 * Initialize a prng state struct.
 *
 * We derive the seed from base_random_sequence, which must be set up already.
 */
static void
initRandomState(pg_prng_state *state)
{
	pg_prng_seed(state, pg_prng_uint64(&base_random_sequence));
}


/*
 * random number generator: uniform distribution from min to max inclusive.
 *
 * Although the limits are expressed as int64, you can't generate the full
 * int64 range in one call, because the difference of the limits mustn't
 * overflow int64.  This is not checked.
 */
static int64
getrand(pg_prng_state *state, int64 min, int64 max)
{
	return min + (int64) pg_prng_uint64_range(state, 0, max - min);
}

/*
 * random number generator: exponential distribution from min to max inclusive.
 * the parameter is so that the density of probability for the last cut-off max
 * value is exp(-parameter).
 */
static int64
getExponentialRand(pg_prng_state *state, int64 min, int64 max,
				   double parameter)
{
	double		cut,
				uniform,
				rand;

	/* abort if wrong parameter, but must really be checked beforehand */
	Assert(parameter > 0.0);
	cut = exp(-parameter);
	/* pg_prng_double value in [0, 1), uniform in (0, 1] */
	uniform = 1.0 - pg_prng_double(state);

	/*
	 * inner expression in (cut, 1] (if parameter > 0), rand in [0, 1)
	 */
	Assert((1.0 - cut) != 0.0);
	rand = -log(cut + (1.0 - cut) * uniform) / parameter;
	/* return int64 random number within between min and max */
	return min + (int64) ((max - min + 1) * rand);
}

/* random number generator: gaussian distribution from min to max inclusive */
static int64
getGaussianRand(pg_prng_state *state, int64 min, int64 max,
				double parameter)
{
	double		stdev;
	double		rand;

	/* abort if parameter is too low, but must really be checked beforehand */
	Assert(parameter >= MIN_GAUSSIAN_PARAM);

	/*
	 * Get normally-distributed random number in the range -parameter <= stdev
	 * < parameter.
	 *
	 * This loop is executed until the number is in the expected range.
	 *
	 * As the minimum parameter is 2.0, the probability of looping is low:
	 * sqrt(-2 ln(r)) <= 2 => r >= e^{-2} ~ 0.135, then when taking the
	 * average sinus multiplier as 2/pi, we have a 8.6% looping probability in
	 * the worst case. For a parameter value of 5.0, the looping probability
	 * is about e^{-5} * 2 / pi ~ 0.43%.
	 */
	do
	{
		stdev = pg_prng_double_normal(state);
	}
	while (stdev < -parameter || stdev >= parameter);

	/* stdev is in [-parameter, parameter), normalization to [0,1) */
	rand = (stdev + parameter) / (parameter * 2.0);

	/* return int64 random number within between min and max */
	return min + (int64) ((max - min + 1) * rand);
}

/*
 * random number generator: generate a value, such that the series of values
 * will approximate a Poisson distribution centered on the given value.
 *
 * Individual results are rounded to integers, though the center value need
 * not be one.
 */
static int64
getPoissonRand(pg_prng_state *state, double center)
{
	/*
	 * Use inverse transform sampling to generate a value > 0, such that the
	 * expected (i.e. average) value is the given argument.
	 */
	double		uniform;

	/* pg_prng_double value in [0, 1), uniform in (0, 1] */
	uniform = 1.0 - pg_prng_double(state);

	return (int64) (-log(uniform) * center + 0.5);
}

/*
 * Computing zipfian using rejection method, based on
 * "Non-Uniform Random Variate Generation",
 * Luc Devroye, p. 550-551, Springer 1986.
 *
 * This works for s > 1.0, but may perform badly for s very close to 1.0.
 */
static int64
computeIterativeZipfian(pg_prng_state *state, int64 n, double s)
{
	double		b = pow(2.0, s - 1.0);
	double		x,
				t,
				u,
				v;

	/* Ensure n is sane */
	if (n <= 1)
		return 1;

	while (true)
	{
		/* random variates */
		u = pg_prng_double(state);
		v = pg_prng_double(state);

		x = floor(pow(u, -1.0 / (s - 1.0)));

		t = pow(1.0 + 1.0 / x, s - 1.0);
		/* reject if too large or out of bound */
		if (v * x * (t - 1.0) / (b - 1.0) <= t / b && x <= n)
			break;
	}
	return (int64) x;
}

/* random number generator: zipfian distribution from min to max inclusive */
static int64
getZipfianRand(pg_prng_state *state, int64 min, int64 max, double s)
{
	int64		n = max - min + 1;

	/* abort if parameter is invalid */
	Assert(MIN_ZIPFIAN_PARAM <= s && s <= MAX_ZIPFIAN_PARAM);

	return min - 1 + computeIterativeZipfian(state, n, s);
}

/*
 * FNV-1a hash function
 */
static int64
getHashFnv1a(int64 val, uint64 seed)
{
	int64		result;
	int			i;

	result = FNV_OFFSET_BASIS ^ seed;
	for (i = 0; i < 8; ++i)
	{
		int32		octet = val & 0xff;

		val = val >> 8;
		result = result ^ octet;
		result = result * FNV_PRIME;
	}

	return result;
}

/*
 * Murmur2 hash function
 *
 * Based on original work of Austin Appleby
 * https://github.com/aappleby/smhasher/blob/master/src/MurmurHash2.cpp
 */
static int64
getHashMurmur2(int64 val, uint64 seed)
{
	uint64		result = seed ^ MM2_MUL_TIMES_8;	/* sizeof(int64) */
	uint64		k = (uint64) val;

	k *= MM2_MUL;
	k ^= k >> MM2_ROT;
	k *= MM2_MUL;

	result ^= k;
	result *= MM2_MUL;

	result ^= result >> MM2_ROT;
	result *= MM2_MUL;
	result ^= result >> MM2_ROT;

	return (int64) result;
}

/*
 * Pseudorandom permutation function
 *
 * For small sizes, this generates each of the (size!) possible permutations
 * of integers in the range [0, size) with roughly equal probability.  Once
 * the size is larger than 20, the number of possible permutations exceeds the
 * number of distinct states of the internal pseudorandom number generator,
 * and so not all possible permutations can be generated, but the permutations
 * chosen should continue to give the appearance of being random.
 *
 * THIS FUNCTION IS NOT CRYPTOGRAPHICALLY SECURE.
 * DO NOT USE FOR SUCH PURPOSE.
 */
static int64
permute(const int64 val, const int64 isize, const int64 seed)
{
	/* using a high-end PRNG is probably overkill */
	pg_prng_state state;
	uint64		size;
	uint64		v;
	int			masklen;
	uint64		mask;
	int			i;

	if (isize < 2)
		return 0;				/* nothing to permute */

	/* Initialize prng state using the seed */
	pg_prng_seed(&state, (uint64) seed);

	/* Computations are performed on unsigned values */
	size = (uint64) isize;
	v = (uint64) val % size;

	/* Mask to work modulo largest power of 2 less than or equal to size */
	masklen = pg_leftmost_one_pos64(size);
	mask = (((uint64) 1) << masklen) - 1;

	/*
	 * Permute the input value by applying several rounds of pseudorandom
	 * bijective transformations.  The intention here is to distribute each
	 * input uniformly randomly across the range, and separate adjacent inputs
	 * approximately uniformly randomly from each other, leading to a fairly
	 * random overall choice of permutation.
	 *
	 * To separate adjacent inputs, we multiply by a random number modulo
	 * (mask + 1), which is a power of 2.  For this to be a bijection, the
	 * multiplier must be odd.  Since this is known to lead to less randomness
	 * in the lower bits, we also apply a rotation that shifts the topmost bit
	 * into the least significant bit.  In the special cases where size <= 3,
	 * mask = 1 and each of these operations is actually a no-op, so we also
	 * XOR the value with a different random number to inject additional
	 * randomness.  Since the size is generally not a power of 2, we apply
	 * this bijection on overlapping upper and lower halves of the input.
	 *
	 * To distribute the inputs uniformly across the range, we then also apply
	 * a random offset modulo the full range.
	 *
	 * Taken together, these operations resemble a modified linear
	 * congruential generator, as is commonly used in pseudorandom number
	 * generators.  The number of rounds is fairly arbitrary, but six has been
	 * found empirically to give a fairly good tradeoff between performance
	 * and uniform randomness.  For small sizes it selects each of the (size!)
	 * possible permutations with roughly equal probability.  For larger
	 * sizes, not all permutations can be generated, but the intended random
	 * spread is still produced.
	 */
	for (i = 0; i < 6; i++)
	{
		uint64		m,
					r,
					t;

		/* Random multiply (by an odd number), XOR and rotate of lower half */
		m = (pg_prng_uint64(&state) & mask) | 1;
		r = pg_prng_uint64(&state) & mask;
		if (v <= mask)
		{
			v = ((v * m) ^ r) & mask;
			v = ((v << 1) & mask) | (v >> (masklen - 1));
		}

		/* Random multiply (by an odd number), XOR and rotate of upper half */
		m = (pg_prng_uint64(&state) & mask) | 1;
		r = pg_prng_uint64(&state) & mask;
		t = size - 1 - v;
		if (t <= mask)
		{
			t = ((t * m) ^ r) & mask;
			t = ((t << 1) & mask) | (t >> (masklen - 1));
			v = size - 1 - t;
		}

		/* Random offset */
		r = pg_prng_uint64_range(&state, 0, size - 1);
		v = (v + r) % size;
	}

	return (int64) v;
}

/*
 * Initialize the given SimpleStats struct to all zeroes
 */
static void
initSimpleStats(SimpleStats *ss)
{
	memset(ss, 0, sizeof(SimpleStats));
}

/*
 * Accumulate one value into a SimpleStats struct.
 */
static void
addToSimpleStats(SimpleStats *ss, double val)
{
	if (ss->count == 0 || val < ss->min)
		ss->min = val;
	if (ss->count == 0 || val > ss->max)
		ss->max = val;
	ss->count++;
	ss->sum += val;
	ss->sum2 += val * val;
}

/*
 * Merge two SimpleStats objects
 */
static void
mergeSimpleStats(SimpleStats *acc, SimpleStats *ss)
{
	if (acc->count == 0 || ss->min < acc->min)
		acc->min = ss->min;
	if (acc->count == 0 || ss->max > acc->max)
		acc->max = ss->max;
	acc->count += ss->count;
	acc->sum += ss->sum;
	acc->sum2 += ss->sum2;
}

/*
 * Initialize a StatsData struct to mostly zeroes, with its start time set to
 * the given value.
 */
static void
initStats(StatsData *sd, pg_time_usec_t start)
{
	sd->start_time = start;
	sd->cnt = 0;
	sd->skipped = 0;
	sd->retries = 0;
	sd->retried = 0;
	sd->serialization_failures = 0;
	sd->deadlock_failures = 0;
	initSimpleStats(&sd->latency);
	initSimpleStats(&sd->lag);
}

/*
 * Accumulate one additional item into the given stats object.
 */
static void
accumStats(StatsData *stats, bool skipped, double lat, double lag,
		   EStatus estatus, int64 tries)
{
	/* Record the skipped transaction */
	if (skipped)
	{
		/* no latency to record on skipped transactions */
		stats->skipped++;
		return;
	}

	/*
	 * Record the number of retries regardless of whether the transaction was
	 * successful or failed.
	 */
	if (tries > 1)
	{
		stats->retries += (tries - 1);
		stats->retried++;
	}

	switch (estatus)
	{
			/* Record the successful transaction */
		case ESTATUS_NO_ERROR:
			stats->cnt++;

			addToSimpleStats(&stats->latency, lat);

			/* and possibly the same for schedule lag */
			if (throttle_delay)
				addToSimpleStats(&stats->lag, lag);
			break;

			/* Record the failed transaction */
		case ESTATUS_SERIALIZATION_ERROR:
			stats->serialization_failures++;
			break;
		case ESTATUS_DEADLOCK_ERROR:
			stats->deadlock_failures++;
			break;
		default:
			/* internal error which should never occur */
			pg_fatal("unexpected error status: %d", estatus);
	}
}

/* call PQexec() and exit() on failure */
static void
executeStatement(PGconn *con, const char *sql)
{
	PGresult   *res;

	res = PQexec(con, sql);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		pg_log_error("query failed: %s", PQerrorMessage(con));
		pg_log_error_detail("Query was: %s", sql);
		exit(1);
	}
	PQclear(res);
}

/* call PQexec() and complain, but without exiting, on failure */
static void
tryExecuteStatement(PGconn *con, const char *sql)
{
	PGresult   *res;

	res = PQexec(con, sql);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		pg_log_error("%s", PQerrorMessage(con));
		pg_log_error_detail("(ignoring this error and continuing anyway)");
	}
	PQclear(res);
}

/* set up a connection to the backend */
static PGconn *
doConnect(void)
{
	PGconn	   *conn;
	bool		new_pass;
	static char *password = NULL;

	/*
	 * Start the connection.  Loop until we have a password if requested by
	 * backend.
	 */
	do
	{
#define PARAMS_ARRAY_SIZE	7

		const char *keywords[PARAMS_ARRAY_SIZE];
		const char *values[PARAMS_ARRAY_SIZE];

		keywords[0] = "host";
		values[0] = pghost;
		keywords[1] = "port";
		values[1] = pgport;
		keywords[2] = "user";
		values[2] = username;
		keywords[3] = "password";
		values[3] = password;
		keywords[4] = "dbname";
		values[4] = dbName;
		keywords[5] = "fallback_application_name";
		values[5] = progname;
		keywords[6] = NULL;
		values[6] = NULL;

		new_pass = false;

		conn = PQconnectdbParams(keywords, values, true);

		if (!conn)
		{
			pg_log_error("connection to database \"%s\" failed", dbName);
			return NULL;
		}

		if (PQstatus(conn) == CONNECTION_BAD &&
			PQconnectionNeedsPassword(conn) &&
			!password)
		{
			PQfinish(conn);
			password = simple_prompt("Password: ", false);
			new_pass = true;
		}
	} while (new_pass);

	/* check to see that the backend connection was successfully made */
	if (PQstatus(conn) == CONNECTION_BAD)
	{
		pg_log_error("%s", PQerrorMessage(conn));
		PQfinish(conn);
		return NULL;
	}

	return conn;
}

/* qsort comparator for Variable array */
static int
compareVariableNames(const void *v1, const void *v2)
{
	return strcmp(((const Variable *) v1)->name,
				  ((const Variable *) v2)->name);
}

/* Locate a variable by name; returns NULL if unknown */
static Variable *
lookupVariable(Variables *variables, char *name)
{
	Variable	key;

	/* On some versions of Solaris, bsearch of zero items dumps core */
	if (variables->nvars <= 0)
		return NULL;

	/* Sort if we have to */
	if (!variables->vars_sorted)
	{
		qsort(variables->vars, variables->nvars, sizeof(Variable),
			  compareVariableNames);
		variables->vars_sorted = true;
	}

	/* Now we can search */
	key.name = name;
	return (Variable *) bsearch(&key,
								variables->vars,
								variables->nvars,
								sizeof(Variable),
								compareVariableNames);
}

/* Get the value of a variable, in string form; returns NULL if unknown */
static char *
getVariable(Variables *variables, char *name)
{
	Variable   *var;
	char		stringform[64];

	var = lookupVariable(variables, name);
	if (var == NULL)
		return NULL;			/* not found */

	if (var->svalue)
		return var->svalue;		/* we have it in string form */

	/* We need to produce a string equivalent of the value */
	Assert(var->value.type != PGBT_NO_VALUE);
	if (var->value.type == PGBT_NULL)
		snprintf(stringform, sizeof(stringform), "NULL");
	else if (var->value.type == PGBT_BOOLEAN)
		snprintf(stringform, sizeof(stringform),
				 "%s", var->value.u.bval ? "true" : "false");
	else if (var->value.type == PGBT_INT)
		snprintf(stringform, sizeof(stringform),
				 INT64_FORMAT, var->value.u.ival);
	else if (var->value.type == PGBT_DOUBLE)
		snprintf(stringform, sizeof(stringform),
				 "%.*g", DBL_DIG, var->value.u.dval);
	else						/* internal error, unexpected type */
		Assert(0);
	var->svalue = pg_strdup(stringform);
	return var->svalue;
}

/* Try to convert variable to a value; return false on failure */
static bool
makeVariableValue(Variable *var)
{
	size_t		slen;

	if (var->value.type != PGBT_NO_VALUE)
		return true;			/* no work */

	slen = strlen(var->svalue);

	if (slen == 0)
		/* what should it do on ""? */
		return false;

	if (pg_strcasecmp(var->svalue, "null") == 0)
	{
		setNullValue(&var->value);
	}

	/*
	 * accept prefixes such as y, ye, n, no... but not for "o". 0/1 are
	 * recognized later as an int, which is converted to bool if needed.
	 */
	else if (pg_strncasecmp(var->svalue, "true", slen) == 0 ||
			 pg_strncasecmp(var->svalue, "yes", slen) == 0 ||
			 pg_strcasecmp(var->svalue, "on") == 0)
	{
		setBoolValue(&var->value, true);
	}
	else if (pg_strncasecmp(var->svalue, "false", slen) == 0 ||
			 pg_strncasecmp(var->svalue, "no", slen) == 0 ||
			 pg_strcasecmp(var->svalue, "off") == 0 ||
			 pg_strcasecmp(var->svalue, "of") == 0)
	{
		setBoolValue(&var->value, false);
	}
	else if (is_an_int(var->svalue))
	{
		/* if it looks like an int, it must be an int without overflow */
		int64		iv;

		if (!strtoint64(var->svalue, false, &iv))
			return false;

		setIntValue(&var->value, iv);
	}
	else						/* type should be double */
	{
		double		dv;

		if (!strtodouble(var->svalue, true, &dv))
		{
			pg_log_error("malformed variable \"%s\" value: \"%s\"",
						 var->name, var->svalue);
			return false;
		}
		setDoubleValue(&var->value, dv);
	}
	return true;
}

/*
 * Check whether a variable's name is allowed.
 *
 * We allow any non-ASCII character, as well as ASCII letters, digits, and
 * underscore.
 *
 * Keep this in sync with the definitions of variable name characters in
 * "src/fe_utils/psqlscan.l", "src/bin/psql/psqlscanslash.l" and
 * "src/bin/pgbench/exprscan.l".  Also see parseVariable(), below.
 *
 * Note: this static function is copied from "src/bin/psql/variables.c"
 * but changed to disallow variable names starting with a digit.
 */
static bool
valid_variable_name(const char *name)
{
	const unsigned char *ptr = (const unsigned char *) name;

	/* Mustn't be zero-length */
	if (*ptr == '\0')
		return false;

	/* must not start with [0-9] */
	if (IS_HIGHBIT_SET(*ptr) ||
		strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZ" "abcdefghijklmnopqrstuvwxyz"
			   "_", *ptr) != NULL)
		ptr++;
	else
		return false;

	/* remaining characters can include [0-9] */
	while (*ptr)
	{
		if (IS_HIGHBIT_SET(*ptr) ||
			strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZ" "abcdefghijklmnopqrstuvwxyz"
				   "_0123456789", *ptr) != NULL)
			ptr++;
		else
			return false;
	}

	return true;
}

/*
 * Make sure there is enough space for 'needed' more variable in the variables
 * array.
 */
static void
enlargeVariables(Variables *variables, int needed)
{
	/* total number of variables required now */
	needed += variables->nvars;

	if (variables->max_vars < needed)
	{
		variables->max_vars = needed + VARIABLES_ALLOC_MARGIN;
		variables->vars = (Variable *)
			pg_realloc(variables->vars, variables->max_vars * sizeof(Variable));
	}
}

/*
 * Lookup a variable by name, creating it if need be.
 * Caller is expected to assign a value to the variable.
 * Returns NULL on failure (bad name).
 */
static Variable *
lookupCreateVariable(Variables *variables, const char *context, char *name)
{
	Variable   *var;

	var = lookupVariable(variables, name);
	if (var == NULL)
	{
		/*
		 * Check for the name only when declaring a new variable to avoid
		 * overhead.
		 */
		if (!valid_variable_name(name))
		{
			pg_log_error("%s: invalid variable name: \"%s\"", context, name);
			return NULL;
		}

		/* Create variable at the end of the array */
		enlargeVariables(variables, 1);

		var = &(variables->vars[variables->nvars]);

		var->name = pg_strdup(name);
		var->svalue = NULL;
		/* caller is expected to initialize remaining fields */

		variables->nvars++;
		/* we don't re-sort the array till we have to */
		variables->vars_sorted = false;
	}

	return var;
}

/* Assign a string value to a variable, creating it if need be */
/* Returns false on failure (bad name) */
static bool
putVariable(Variables *variables, const char *context, char *name,
			const char *value)
{
	Variable   *var;
	char	   *val;

	var = lookupCreateVariable(variables, context, name);
	if (!var)
		return false;

	/* dup then free, in case value is pointing at this variable */
	val = pg_strdup(value);

	free(var->svalue);
	var->svalue = val;
	var->value.type = PGBT_NO_VALUE;

	return true;
}

/* Assign a value to a variable, creating it if need be */
/* Returns false on failure (bad name) */
static bool
putVariableValue(Variables *variables, const char *context, char *name,
				 const PgBenchValue *value)
{
	Variable   *var;

	var = lookupCreateVariable(variables, context, name);
	if (!var)
		return false;

	free(var->svalue);
	var->svalue = NULL;
	var->value = *value;

	return true;
}

/* Assign an integer value to a variable, creating it if need be */
/* Returns false on failure (bad name) */
static bool
putVariableInt(Variables *variables, const char *context, char *name,
			   int64 value)
{
	PgBenchValue val;

	setIntValue(&val, value);
	return putVariableValue(variables, context, name, &val);
}

/*
 * Parse a possible variable reference (:varname).
 *
 * "sql" points at a colon.  If what follows it looks like a valid
 * variable name, return a malloc'd string containing the variable name,
 * and set *eaten to the number of characters consumed (including the colon).
 * Otherwise, return NULL.
 */
static char *
parseVariable(const char *sql, int *eaten)
{
	int			i = 1;			/* starting at 1 skips the colon */
	char	   *name;

	/* keep this logic in sync with valid_variable_name() */
	if (IS_HIGHBIT_SET(sql[i]) ||
		strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZ" "abcdefghijklmnopqrstuvwxyz"
			   "_", sql[i]) != NULL)
		i++;
	else
		return NULL;

	while (IS_HIGHBIT_SET(sql[i]) ||
		   strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZ" "abcdefghijklmnopqrstuvwxyz"
				  "_0123456789", sql[i]) != NULL)
		i++;

	name = pg_malloc(i);
	memcpy(name, &sql[1], i - 1);
	name[i - 1] = '\0';

	*eaten = i;
	return name;
}

static char *
replaceVariable(char **sql, char *param, int len, char *value)
{
	int			valueln = strlen(value);

	if (valueln > len)
	{
		size_t		offset = param - *sql;

		*sql = pg_realloc(*sql, strlen(*sql) - len + valueln + 1);
		param = *sql + offset;
	}

	if (valueln != len)
		memmove(param + valueln, param + len, strlen(param + len) + 1);
	memcpy(param, value, valueln);

	return param + valueln;
}

static char *
assignVariables(Variables *variables, char *sql)
{
	char	   *p,
			   *name,
			   *val;

	p = sql;
	while ((p = strchr(p, ':')) != NULL)
	{
		int			eaten;

		name = parseVariable(p, &eaten);
		if (name == NULL)
		{
			while (*p == ':')
			{
				p++;
			}
			continue;
		}

		val = getVariable(variables, name);
		free(name);
		if (val == NULL)
		{
			p++;
			continue;
		}

		p = replaceVariable(&sql, p, eaten, val);
	}

	return sql;
}

static void
getQueryParams(Variables *variables, const Command *command,
			   const char **params)
{
	int			i;

	for (i = 0; i < command->argc - 1; i++)
		params[i] = getVariable(variables, command->argv[i + 1]);
}

static char *
valueTypeName(PgBenchValue *pval)
{
	if (pval->type == PGBT_NO_VALUE)
		return "none";
	else if (pval->type == PGBT_NULL)
		return "null";
	else if (pval->type == PGBT_INT)
		return "int";
	else if (pval->type == PGBT_DOUBLE)
		return "double";
	else if (pval->type == PGBT_BOOLEAN)
		return "boolean";
	else
	{
		/* internal error, should never get there */
		Assert(false);
		return NULL;
	}
}

/* get a value as a boolean, or tell if there is a problem */
static bool
coerceToBool(PgBenchValue *pval, bool *bval)
{
	if (pval->type == PGBT_BOOLEAN)
	{
		*bval = pval->u.bval;
		return true;
	}
	else						/* NULL, INT or DOUBLE */
	{
		pg_log_error("cannot coerce %s to boolean", valueTypeName(pval));
		*bval = false;			/* suppress uninitialized-variable warnings */
		return false;
	}
}

/*
 * Return true or false from an expression for conditional purposes.
 * Non zero numerical values are true, zero and NULL are false.
 */
static bool
valueTruth(PgBenchValue *pval)
{
	switch (pval->type)
	{
		case PGBT_NULL:
			return false;
		case PGBT_BOOLEAN:
			return pval->u.bval;
		case PGBT_INT:
			return pval->u.ival != 0;
		case PGBT_DOUBLE:
			return pval->u.dval != 0.0;
		default:
			/* internal error, unexpected type */
			Assert(0);
			return false;
	}
}

/* get a value as an int, tell if there is a problem */
static bool
coerceToInt(PgBenchValue *pval, int64 *ival)
{
	if (pval->type == PGBT_INT)
	{
		*ival = pval->u.ival;
		return true;
	}
	else if (pval->type == PGBT_DOUBLE)
	{
		double		dval = rint(pval->u.dval);

		if (isnan(dval) || !FLOAT8_FITS_IN_INT64(dval))
		{
			pg_log_error("double to int overflow for %f", dval);
			return false;
		}
		*ival = (int64) dval;
		return true;
	}
	else						/* BOOLEAN or NULL */
	{
		pg_log_error("cannot coerce %s to int", valueTypeName(pval));
		return false;
	}
}

/* get a value as a double, or tell if there is a problem */
static bool
coerceToDouble(PgBenchValue *pval, double *dval)
{
	if (pval->type == PGBT_DOUBLE)
	{
		*dval = pval->u.dval;
		return true;
	}
	else if (pval->type == PGBT_INT)
	{
		*dval = (double) pval->u.ival;
		return true;
	}
	else						/* BOOLEAN or NULL */
	{
		pg_log_error("cannot coerce %s to double", valueTypeName(pval));
		return false;
	}
}

/* assign a null value */
static void
setNullValue(PgBenchValue *pv)
{
	pv->type = PGBT_NULL;
	pv->u.ival = 0;
}

/* assign a boolean value */
static void
setBoolValue(PgBenchValue *pv, bool bval)
{
	pv->type = PGBT_BOOLEAN;
	pv->u.bval = bval;
}

/* assign an integer value */
static void
setIntValue(PgBenchValue *pv, int64 ival)
{
	pv->type = PGBT_INT;
	pv->u.ival = ival;
}

/* assign a double value */
static void
setDoubleValue(PgBenchValue *pv, double dval)
{
	pv->type = PGBT_DOUBLE;
	pv->u.dval = dval;
}

static bool
isLazyFunc(PgBenchFunction func)
{
	return func == PGBENCH_AND || func == PGBENCH_OR || func == PGBENCH_CASE;
}

/* lazy evaluation of some functions */
static bool
evalLazyFunc(CState *st,
			 PgBenchFunction func, PgBenchExprLink *args, PgBenchValue *retval)
{
	PgBenchValue a1,
				a2;
	bool		ba1,
				ba2;

	Assert(isLazyFunc(func) && args != NULL && args->next != NULL);

	/* args points to first condition */
	if (!evaluateExpr(st, args->expr, &a1))
		return false;

	/* second condition for AND/OR and corresponding branch for CASE */
	args = args->next;

	switch (func)
	{
		case PGBENCH_AND:
			if (a1.type == PGBT_NULL)
			{
				setNullValue(retval);
				return true;
			}

			if (!coerceToBool(&a1, &ba1))
				return false;

			if (!ba1)
			{
				setBoolValue(retval, false);
				return true;
			}

			if (!evaluateExpr(st, args->expr, &a2))
				return false;

			if (a2.type == PGBT_NULL)
			{
				setNullValue(retval);
				return true;
			}
			else if (!coerceToBool(&a2, &ba2))
				return false;
			else
			{
				setBoolValue(retval, ba2);
				return true;
			}

			return true;

		case PGBENCH_OR:

			if (a1.type == PGBT_NULL)
			{
				setNullValue(retval);
				return true;
			}

			if (!coerceToBool(&a1, &ba1))
				return false;

			if (ba1)
			{
				setBoolValue(retval, true);
				return true;
			}

			if (!evaluateExpr(st, args->expr, &a2))
				return false;

			if (a2.type == PGBT_NULL)
			{
				setNullValue(retval);
				return true;
			}
			else if (!coerceToBool(&a2, &ba2))
				return false;
			else
			{
				setBoolValue(retval, ba2);
				return true;
			}

		case PGBENCH_CASE:
			/* when true, execute branch */
			if (valueTruth(&a1))
				return evaluateExpr(st, args->expr, retval);

			/* now args contains next condition or final else expression */
			args = args->next;

			/* final else case? */
			if (args->next == NULL)
				return evaluateExpr(st, args->expr, retval);

			/* no, another when, proceed */
			return evalLazyFunc(st, PGBENCH_CASE, args, retval);

		default:
			/* internal error, cannot get here */
			Assert(0);
			break;
	}
	return false;
}

/* maximum number of function arguments */
#define MAX_FARGS 16

/*
 * Recursive evaluation of standard functions,
 * which do not require lazy evaluation.
 */
static bool
evalStandardFunc(CState *st,
				 PgBenchFunction func, PgBenchExprLink *args,
				 PgBenchValue *retval)
{
	/* evaluate all function arguments */
	int			nargs = 0;
	PgBenchExprLink *l = args;
	bool		has_null = false;

	/*
	 * This value is double braced to workaround GCC bug 53119, which seems to
	 * exist at least on gcc (Debian 4.7.2-5) 4.7.2, 32-bit.
	 */
	PgBenchValue vargs[MAX_FARGS] = {{0}};

	for (nargs = 0; nargs < MAX_FARGS && l != NULL; nargs++, l = l->next)
	{
		if (!evaluateExpr(st, l->expr, &vargs[nargs]))
			return false;
		has_null |= vargs[nargs].type == PGBT_NULL;
	}

	if (l != NULL)
	{
		pg_log_error("too many function arguments, maximum is %d", MAX_FARGS);
		return false;
	}

	/* NULL arguments */
	if (has_null && func != PGBENCH_IS && func != PGBENCH_DEBUG)
	{
		setNullValue(retval);
		return true;
	}

	/* then evaluate function */
	switch (func)
	{
			/* overloaded operators */
		case PGBENCH_ADD:
		case PGBENCH_SUB:
		case PGBENCH_MUL:
		case PGBENCH_DIV:
		case PGBENCH_MOD:
		case PGBENCH_EQ:
		case PGBENCH_NE:
		case PGBENCH_LE:
		case PGBENCH_LT:
			{
				PgBenchValue *lval = &vargs[0],
						   *rval = &vargs[1];

				Assert(nargs == 2);

				/* overloaded type management, double if some double */
				if ((lval->type == PGBT_DOUBLE ||
					 rval->type == PGBT_DOUBLE) && func != PGBENCH_MOD)
				{
					double		ld,
								rd;

					if (!coerceToDouble(lval, &ld) ||
						!coerceToDouble(rval, &rd))
						return false;

					switch (func)
					{
						case PGBENCH_ADD:
							setDoubleValue(retval, ld + rd);
							return true;

						case PGBENCH_SUB:
							setDoubleValue(retval, ld - rd);
							return true;

						case PGBENCH_MUL:
							setDoubleValue(retval, ld * rd);
							return true;

						case PGBENCH_DIV:
							setDoubleValue(retval, ld / rd);
							return true;

						case PGBENCH_EQ:
							setBoolValue(retval, ld == rd);
							return true;

						case PGBENCH_NE:
							setBoolValue(retval, ld != rd);
							return true;

						case PGBENCH_LE:
							setBoolValue(retval, ld <= rd);
							return true;

						case PGBENCH_LT:
							setBoolValue(retval, ld < rd);
							return true;

						default:
							/* cannot get here */
							Assert(0);
					}
				}
				else			/* we have integer operands, or % */
				{
					int64		li,
								ri,
								res;

					if (!coerceToInt(lval, &li) ||
						!coerceToInt(rval, &ri))
						return false;

					switch (func)
					{
						case PGBENCH_ADD:
							if (pg_add_s64_overflow(li, ri, &res))
							{
								pg_log_error("bigint add out of range");
								return false;
							}
							setIntValue(retval, res);
							return true;

						case PGBENCH_SUB:
							if (pg_sub_s64_overflow(li, ri, &res))
							{
								pg_log_error("bigint sub out of range");
								return false;
							}
							setIntValue(retval, res);
							return true;

						case PGBENCH_MUL:
							if (pg_mul_s64_overflow(li, ri, &res))
							{
								pg_log_error("bigint mul out of range");
								return false;
							}
							setIntValue(retval, res);
							return true;

						case PGBENCH_EQ:
							setBoolValue(retval, li == ri);
							return true;

						case PGBENCH_NE:
							setBoolValue(retval, li != ri);
							return true;

						case PGBENCH_LE:
							setBoolValue(retval, li <= ri);
							return true;

						case PGBENCH_LT:
							setBoolValue(retval, li < ri);
							return true;

						case PGBENCH_DIV:
						case PGBENCH_MOD:
							if (ri == 0)
							{
								pg_log_error("division by zero");
								return false;
							}
							/* special handling of -1 divisor */
							if (ri == -1)
							{
								if (func == PGBENCH_DIV)
								{
									/* overflow check (needed for INT64_MIN) */
									if (li == PG_INT64_MIN)
									{
										pg_log_error("bigint div out of range");
										return false;
									}
									else
										setIntValue(retval, -li);
								}
								else
									setIntValue(retval, 0);
								return true;
							}
							/* else divisor is not -1 */
							if (func == PGBENCH_DIV)
								setIntValue(retval, li / ri);
							else	/* func == PGBENCH_MOD */
								setIntValue(retval, li % ri);

							return true;

						default:
							/* cannot get here */
							Assert(0);
					}
				}

				Assert(0);
				return false;	/* NOTREACHED */
			}

			/* integer bitwise operators */
		case PGBENCH_BITAND:
		case PGBENCH_BITOR:
		case PGBENCH_BITXOR:
		case PGBENCH_LSHIFT:
		case PGBENCH_RSHIFT:
			{
				int64		li,
							ri;

				if (!coerceToInt(&vargs[0], &li) || !coerceToInt(&vargs[1], &ri))
					return false;

				if (func == PGBENCH_BITAND)
					setIntValue(retval, li & ri);
				else if (func == PGBENCH_BITOR)
					setIntValue(retval, li | ri);
				else if (func == PGBENCH_BITXOR)
					setIntValue(retval, li ^ ri);
				else if (func == PGBENCH_LSHIFT)
					setIntValue(retval, li << ri);
				else if (func == PGBENCH_RSHIFT)
					setIntValue(retval, li >> ri);
				else			/* cannot get here */
					Assert(0);

				return true;
			}

			/* logical operators */
		case PGBENCH_NOT:
			{
				bool		b;

				if (!coerceToBool(&vargs[0], &b))
					return false;

				setBoolValue(retval, !b);
				return true;
			}

			/* no arguments */
		case PGBENCH_PI:
			setDoubleValue(retval, M_PI);
			return true;

			/* 1 overloaded argument */
		case PGBENCH_ABS:
			{
				PgBenchValue *varg = &vargs[0];

				Assert(nargs == 1);

				if (varg->type == PGBT_INT)
				{
					int64		i = varg->u.ival;

					setIntValue(retval, i < 0 ? -i : i);
				}
				else
				{
					double		d = varg->u.dval;

					Assert(varg->type == PGBT_DOUBLE);
					setDoubleValue(retval, d < 0.0 ? -d : d);
				}

				return true;
			}

		case PGBENCH_DEBUG:
			{
				PgBenchValue *varg = &vargs[0];

				Assert(nargs == 1);

				fprintf(stderr, "debug(script=%d,command=%d): ",
						st->use_file, st->command + 1);

				if (varg->type == PGBT_NULL)
					fprintf(stderr, "null\n");
				else if (varg->type == PGBT_BOOLEAN)
					fprintf(stderr, "boolean %s\n", varg->u.bval ? "true" : "false");
				else if (varg->type == PGBT_INT)
					fprintf(stderr, "int " INT64_FORMAT "\n", varg->u.ival);
				else if (varg->type == PGBT_DOUBLE)
					fprintf(stderr, "double %.*g\n", DBL_DIG, varg->u.dval);
				else			/* internal error, unexpected type */
					Assert(0);

				*retval = *varg;

				return true;
			}

			/* 1 double argument */
		case PGBENCH_DOUBLE:
		case PGBENCH_SQRT:
		case PGBENCH_LN:
		case PGBENCH_EXP:
			{
				double		dval;

				Assert(nargs == 1);

				if (!coerceToDouble(&vargs[0], &dval))
					return false;

				if (func == PGBENCH_SQRT)
					dval = sqrt(dval);
				else if (func == PGBENCH_LN)
					dval = log(dval);
				else if (func == PGBENCH_EXP)
					dval = exp(dval);
				/* else is cast: do nothing */

				setDoubleValue(retval, dval);
				return true;
			}

			/* 1 int argument */
		case PGBENCH_INT:
			{
				int64		ival;

				Assert(nargs == 1);

				if (!coerceToInt(&vargs[0], &ival))
					return false;

				setIntValue(retval, ival);
				return true;
			}

			/* variable number of arguments */
		case PGBENCH_LEAST:
		case PGBENCH_GREATEST:
			{
				bool		havedouble;
				int			i;

				Assert(nargs >= 1);

				/* need double result if any input is double */
				havedouble = false;
				for (i = 0; i < nargs; i++)
				{
					if (vargs[i].type == PGBT_DOUBLE)
					{
						havedouble = true;
						break;
					}
				}
				if (havedouble)
				{
					double		extremum;

					if (!coerceToDouble(&vargs[0], &extremum))
						return false;
					for (i = 1; i < nargs; i++)
					{
						double		dval;

						if (!coerceToDouble(&vargs[i], &dval))
							return false;
						if (func == PGBENCH_LEAST)
							extremum = Min(extremum, dval);
						else
							extremum = Max(extremum, dval);
					}
					setDoubleValue(retval, extremum);
				}
				else
				{
					int64		extremum;

					if (!coerceToInt(&vargs[0], &extremum))
						return false;
					for (i = 1; i < nargs; i++)
					{
						int64		ival;

						if (!coerceToInt(&vargs[i], &ival))
							return false;
						if (func == PGBENCH_LEAST)
							extremum = Min(extremum, ival);
						else
							extremum = Max(extremum, ival);
					}
					setIntValue(retval, extremum);
				}
				return true;
			}

			/* random functions */
		case PGBENCH_RANDOM:
		case PGBENCH_RANDOM_EXPONENTIAL:
		case PGBENCH_RANDOM_GAUSSIAN:
		case PGBENCH_RANDOM_ZIPFIAN:
			{
				int64		imin,
							imax,
							delta;

				Assert(nargs >= 2);

				if (!coerceToInt(&vargs[0], &imin) ||
					!coerceToInt(&vargs[1], &imax))
					return false;

				/* check random range */
				if (unlikely(imin > imax))
				{
					pg_log_error("empty range given to random");
					return false;
				}
				else if (unlikely(pg_sub_s64_overflow(imax, imin, &delta) ||
								  pg_add_s64_overflow(delta, 1, &delta)))
				{
					/* prevent int overflows in random functions */
					pg_log_error("random range is too large");
					return false;
				}

				if (func == PGBENCH_RANDOM)
				{
					Assert(nargs == 2);
					setIntValue(retval, getrand(&st->cs_func_rs, imin, imax));
				}
				else			/* gaussian & exponential */
				{
					double		param;

					Assert(nargs == 3);

					if (!coerceToDouble(&vargs[2], &param))
						return false;

					if (func == PGBENCH_RANDOM_GAUSSIAN)
					{
						if (param < MIN_GAUSSIAN_PARAM)
						{
							pg_log_error("gaussian parameter must be at least %f (not %f)",
										 MIN_GAUSSIAN_PARAM, param);
							return false;
						}

						setIntValue(retval,
									getGaussianRand(&st->cs_func_rs,
													imin, imax, param));
					}
					else if (func == PGBENCH_RANDOM_ZIPFIAN)
					{
						if (param < MIN_ZIPFIAN_PARAM || param > MAX_ZIPFIAN_PARAM)
						{
							pg_log_error("zipfian parameter must be in range [%.3f, %.0f] (not %f)",
										 MIN_ZIPFIAN_PARAM, MAX_ZIPFIAN_PARAM, param);
							return false;
						}

						setIntValue(retval,
									getZipfianRand(&st->cs_func_rs, imin, imax, param));
					}
					else		/* exponential */
					{
						if (param <= 0.0)
						{
							pg_log_error("exponential parameter must be greater than zero (not %f)",
										 param);
							return false;
						}

						setIntValue(retval,
									getExponentialRand(&st->cs_func_rs,
													   imin, imax, param));
					}
				}

				return true;
			}

		case PGBENCH_POW:
			{
				PgBenchValue *lval = &vargs[0];
				PgBenchValue *rval = &vargs[1];
				double		ld,
							rd;

				Assert(nargs == 2);

				if (!coerceToDouble(lval, &ld) ||
					!coerceToDouble(rval, &rd))
					return false;

				setDoubleValue(retval, pow(ld, rd));

				return true;
			}

		case PGBENCH_IS:
			{
				Assert(nargs == 2);

				/*
				 * note: this simple implementation is more permissive than
				 * SQL
				 */
				setBoolValue(retval,
							 vargs[0].type == vargs[1].type &&
							 vargs[0].u.bval == vargs[1].u.bval);
				return true;
			}

			/* hashing */
		case PGBENCH_HASH_FNV1A:
		case PGBENCH_HASH_MURMUR2:
			{
				int64		val,
							seed;

				Assert(nargs == 2);

				if (!coerceToInt(&vargs[0], &val) ||
					!coerceToInt(&vargs[1], &seed))
					return false;

				if (func == PGBENCH_HASH_MURMUR2)
					setIntValue(retval, getHashMurmur2(val, seed));
				else if (func == PGBENCH_HASH_FNV1A)
					setIntValue(retval, getHashFnv1a(val, seed));
				else
					/* cannot get here */
					Assert(0);

				return true;
			}

		case PGBENCH_PERMUTE:
			{
				int64		val,
							size,
							seed;

				Assert(nargs == 3);

				if (!coerceToInt(&vargs[0], &val) ||
					!coerceToInt(&vargs[1], &size) ||
					!coerceToInt(&vargs[2], &seed))
					return false;

				if (size <= 0)
				{
					pg_log_error("permute size parameter must be greater than zero");
					return false;
				}

				setIntValue(retval, permute(val, size, seed));
				return true;
			}

		default:
			/* cannot get here */
			Assert(0);
			/* dead code to avoid a compiler warning */
			return false;
	}
}

/* evaluate some function */
static bool
evalFunc(CState *st,
		 PgBenchFunction func, PgBenchExprLink *args, PgBenchValue *retval)
{
	if (isLazyFunc(func))
		return evalLazyFunc(st, func, args, retval);
	else
		return evalStandardFunc(st, func, args, retval);
}

/*
 * Recursive evaluation of an expression in a pgbench script
 * using the current state of variables.
 * Returns whether the evaluation was ok,
 * the value itself is returned through the retval pointer.
 */
static bool
evaluateExpr(CState *st, PgBenchExpr *expr, PgBenchValue *retval)
{
	switch (expr->etype)
	{
		case ENODE_CONSTANT:
			{
				*retval = expr->u.constant;
				return true;
			}

		case ENODE_VARIABLE:
			{
				Variable   *var;

				if ((var = lookupVariable(&st->variables, expr->u.variable.varname)) == NULL)
				{
					pg_log_error("undefined variable \"%s\"", expr->u.variable.varname);
					return false;
				}

				if (!makeVariableValue(var))
					return false;

				*retval = var->value;
				return true;
			}

		case ENODE_FUNCTION:
			return evalFunc(st,
							expr->u.function.function,
							expr->u.function.args,
							retval);

		default:
			/* internal error which should never occur */
			pg_fatal("unexpected enode type in evaluation: %d", expr->etype);
	}
}

/*
 * Convert command name to meta-command enum identifier
 */
static MetaCommand
getMetaCommand(const char *cmd)
{
	MetaCommand mc;

	if (cmd == NULL)
		mc = META_NONE;
	else if (pg_strcasecmp(cmd, "set") == 0)
		mc = META_SET;
	else if (pg_strcasecmp(cmd, "setshell") == 0)
		mc = META_SETSHELL;
	else if (pg_strcasecmp(cmd, "shell") == 0)
		mc = META_SHELL;
	else if (pg_strcasecmp(cmd, "sleep") == 0)
		mc = META_SLEEP;
	else if (pg_strcasecmp(cmd, "if") == 0)
		mc = META_IF;
	else if (pg_strcasecmp(cmd, "elif") == 0)
		mc = META_ELIF;
	else if (pg_strcasecmp(cmd, "else") == 0)
		mc = META_ELSE;
	else if (pg_strcasecmp(cmd, "endif") == 0)
		mc = META_ENDIF;
	else if (pg_strcasecmp(cmd, "gset") == 0)
		mc = META_GSET;
	else if (pg_strcasecmp(cmd, "aset") == 0)
		mc = META_ASET;
	else if (pg_strcasecmp(cmd, "startpipeline") == 0)
		mc = META_STARTPIPELINE;
	else if (pg_strcasecmp(cmd, "syncpipeline") == 0)
		mc = META_SYNCPIPELINE;
	else if (pg_strcasecmp(cmd, "endpipeline") == 0)
		mc = META_ENDPIPELINE;
	else
		mc = META_NONE;
	return mc;
}

/*
 * Run a shell command. The result is assigned to the variable if not NULL.
 * Return true if succeeded, or false on error.
 */
static bool
runShellCommand(Variables *variables, char *variable, char **argv, int argc)
{
	char		command[SHELL_COMMAND_SIZE];
	int			i,
				len = 0;
	FILE	   *fp;
	char		res[64];
	char	   *endptr;
	int			retval;

	/*----------
	 * Join arguments with whitespace separators. Arguments starting with
	 * exactly one colon are treated as variables:
	 *	name - append a string "name"
	 *	:var - append a variable named 'var'
	 *	::name - append a string ":name"
	 *----------
	 */
	for (i = 0; i < argc; i++)
	{
		char	   *arg;
		int			arglen;

		if (argv[i][0] != ':')
		{
			arg = argv[i];		/* a string literal */
		}
		else if (argv[i][1] == ':')
		{
			arg = argv[i] + 1;	/* a string literal starting with colons */
		}
		else if ((arg = getVariable(variables, argv[i] + 1)) == NULL)
		{
			pg_log_error("%s: undefined variable \"%s\"", argv[0], argv[i]);
			return false;
		}

		arglen = strlen(arg);
		if (len + arglen + (i > 0 ? 1 : 0) >= SHELL_COMMAND_SIZE - 1)
		{
			pg_log_error("%s: shell command is too long", argv[0]);
			return false;
		}

		if (i > 0)
			command[len++] = ' ';
		memcpy(command + len, arg, arglen);
		len += arglen;
	}

	command[len] = '\0';

	fflush(NULL);				/* needed before either system() or popen() */

	/* Fast path for non-assignment case */
	if (variable == NULL)
	{
		if (system(command))
		{
			if (!timer_exceeded)
				pg_log_error("%s: could not launch shell command", argv[0]);
			return false;
		}
		return true;
	}

	/* Execute the command with pipe and read the standard output. */
	if ((fp = popen(command, "r")) == NULL)
	{
		pg_log_error("%s: could not launch shell command", argv[0]);
		return false;
	}
	if (fgets(res, sizeof(res), fp) == NULL)
	{
		if (!timer_exceeded)
			pg_log_error("%s: could not read result of shell command", argv[0]);
		(void) pclose(fp);
		return false;
	}
	if (pclose(fp) < 0)
	{
		pg_log_error("%s: could not run shell command: %m", argv[0]);
		return false;
	}

	/* Check whether the result is an integer and assign it to the variable */
	retval = (int) strtol(res, &endptr, 10);
	while (*endptr != '\0' && isspace((unsigned char) *endptr))
		endptr++;
	if (*res == '\0' || *endptr != '\0')
	{
		pg_log_error("%s: shell command must return an integer (not \"%s\")", argv[0], res);
		return false;
	}
	if (!putVariableInt(variables, "setshell", variable, retval))
		return false;

	pg_log_debug("%s: shell parameter name: \"%s\", value: \"%s\"", argv[0], argv[1], res);

	return true;
}

/*
 * Report the abortion of the client when processing SQL commands.
 */
static void
commandFailed(CState *st, const char *cmd, const char *message)
{
	pg_log_error("client %d aborted in command %d (%s) of script %d; %s",
				 st->id, st->command, cmd, st->use_file, message);
}

/*
 * Report the error in the command while the script is executing.
 */
static void
commandError(CState *st, const char *message)
{
	Assert(sql_script[st->use_file].commands[st->command]->type == SQL_COMMAND);
	pg_log_info("client %d got an error in command %d (SQL) of script %d; %s",
				st->id, st->command, st->use_file, message);
}

/* return a script number with a weighted choice. */
static int
chooseScript(TState *thread)
{
	int			i = 0;
	int64		w;

	if (num_scripts == 1)
		return 0;

	w = getrand(&thread->ts_choose_rs, 0, total_weight - 1);
	do
	{
		w -= sql_script[i++].weight;
	} while (w >= 0);

	return i - 1;
}

/*
 * Allocate space for CState->prepared: we need one boolean for each command
 * of each script.
 */
static void
allocCStatePrepared(CState *st)
{
	Assert(st->prepared == NULL);

	st->prepared = pg_malloc(sizeof(bool *) * num_scripts);
	for (int i = 0; i < num_scripts; i++)
	{
		ParsedScript *script = &sql_script[i];
		int			numcmds;

		for (numcmds = 0; script->commands[numcmds] != NULL; numcmds++)
			;
		st->prepared[i] = pg_malloc0(sizeof(bool) * numcmds);
	}
}

/*
 * Prepare the SQL command from st->use_file at command_num.
 */
static void
prepareCommand(CState *st, int command_num)
{
	Command    *command = sql_script[st->use_file].commands[command_num];

	/* No prepare for non-SQL commands */
	if (command->type != SQL_COMMAND)
		return;

	if (!st->prepared)
		allocCStatePrepared(st);

	if (!st->prepared[st->use_file][command_num])
	{
		PGresult   *res;

		pg_log_debug("client %d preparing %s", st->id, command->prepname);
		res = PQprepare(st->con, command->prepname,
						command->argv[0], command->argc - 1, NULL);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
			pg_log_error("%s", PQerrorMessage(st->con));
		PQclear(res);
		st->prepared[st->use_file][command_num] = true;
	}
}

/*
 * Prepare all the commands in the script that come after the \startpipeline
 * that's at position st->command, and the first \endpipeline we find.
 *
 * This sets the ->prepared flag for each relevant command as well as the
 * \startpipeline itself, but doesn't move the st->command counter.
 */
static void
prepareCommandsInPipeline(CState *st)
{
	int			j;
	Command   **commands = sql_script[st->use_file].commands;

	Assert(commands[st->command]->type == META_COMMAND &&
		   commands[st->command]->meta == META_STARTPIPELINE);

	if (!st->prepared)
		allocCStatePrepared(st);

	/*
	 * We set the 'prepared' flag on the \startpipeline itself to flag that we
	 * don't need to do this next time without calling prepareCommand(), even
	 * though we don't actually prepare this command.
	 */
	if (st->prepared[st->use_file][st->command])
		return;

	for (j = st->command + 1; commands[j] != NULL; j++)
	{
		if (commands[j]->type == META_COMMAND &&
			commands[j]->meta == META_ENDPIPELINE)
			break;

		prepareCommand(st, j);
	}

	st->prepared[st->use_file][st->command] = true;
}

/* Send a SQL command, using the chosen querymode */
static bool
sendCommand(CState *st, Command *command)
{
	int			r;

	if (querymode == QUERY_SIMPLE)
	{
		char	   *sql;

		sql = pg_strdup(command->argv[0]);
		sql = assignVariables(&st->variables, sql);

		pg_log_debug("client %d sending %s", st->id, sql);
		r = PQsendQuery(st->con, sql);
		free(sql);
	}
	else if (querymode == QUERY_EXTENDED)
	{
		const char *sql = command->argv[0];
		const char *params[MAX_ARGS];

		getQueryParams(&st->variables, command, params);

		pg_log_debug("client %d sending %s", st->id, sql);
		r = PQsendQueryParams(st->con, sql, command->argc - 1,
							  NULL, params, NULL, NULL, 0);
	}
	else if (querymode == QUERY_PREPARED)
	{
		const char *params[MAX_ARGS];

		prepareCommand(st, st->command);
		getQueryParams(&st->variables, command, params);

		pg_log_debug("client %d sending %s", st->id, command->prepname);
		r = PQsendQueryPrepared(st->con, command->prepname, command->argc - 1,
								params, NULL, NULL, 0);
	}
	else						/* unknown sql mode */
		r = 0;

	if (r == 0)
	{
		pg_log_debug("client %d could not send %s", st->id, command->argv[0]);
		return false;
	}
	else
		return true;
}

/*
 * Get the error status from the error code.
 */
static EStatus
getSQLErrorStatus(const char *sqlState)
{
	if (sqlState != NULL)
	{
		if (strcmp(sqlState, ERRCODE_T_R_SERIALIZATION_FAILURE) == 0)
			return ESTATUS_SERIALIZATION_ERROR;
		else if (strcmp(sqlState, ERRCODE_T_R_DEADLOCK_DETECTED) == 0)
			return ESTATUS_DEADLOCK_ERROR;
	}

	return ESTATUS_OTHER_SQL_ERROR;
}

/*
 * Returns true if this type of error can be retried.
 */
static bool
canRetryError(EStatus estatus)
{
	return (estatus == ESTATUS_SERIALIZATION_ERROR ||
			estatus == ESTATUS_DEADLOCK_ERROR);
}

/*
 * Process query response from the backend.
 *
 * If varprefix is not NULL, it's the variable name prefix where to store
 * the results of the *last* command (META_GSET) or *all* commands
 * (META_ASET).
 *
 * Returns true if everything is A-OK, false if any error occurs.
 */
static bool
readCommandResponse(CState *st, MetaCommand meta, char *varprefix)
{
	PGresult   *res;
	PGresult   *next_res;
	int			qrynum = 0;

	/*
	 * varprefix should be set only with \gset or \aset, and \endpipeline and
	 * SQL commands do not need it.
	 */
	Assert((meta == META_NONE && varprefix == NULL) ||
		   ((meta == META_ENDPIPELINE) && varprefix == NULL) ||
		   ((meta == META_GSET || meta == META_ASET) && varprefix != NULL));

	res = PQgetResult(st->con);

	while (res != NULL)
	{
		bool		is_last;

		/* peek at the next result to know whether the current is last */
		next_res = PQgetResult(st->con);
		is_last = (next_res == NULL);

		switch (PQresultStatus(res))
		{
			case PGRES_COMMAND_OK:	/* non-SELECT commands */
			case PGRES_EMPTY_QUERY: /* may be used for testing no-op overhead */
				if (is_last && meta == META_GSET)
				{
					pg_log_error("client %d script %d command %d query %d: expected one row, got %d",
								 st->id, st->use_file, st->command, qrynum, 0);
					st->estatus = ESTATUS_META_COMMAND_ERROR;
					goto error;
				}
				break;

			case PGRES_TUPLES_OK:
				if ((is_last && meta == META_GSET) || meta == META_ASET)
				{
					int			ntuples = PQntuples(res);

					if (meta == META_GSET && ntuples != 1)
					{
						/* under \gset, report the error */
						pg_log_error("client %d script %d command %d query %d: expected one row, got %d",
									 st->id, st->use_file, st->command, qrynum, PQntuples(res));
						st->estatus = ESTATUS_META_COMMAND_ERROR;
						goto error;
					}
					else if (meta == META_ASET && ntuples <= 0)
					{
						/* coldly skip empty result under \aset */
						break;
					}

					/* store results into variables */
					for (int fld = 0; fld < PQnfields(res); fld++)
					{
						char	   *varname = PQfname(res, fld);

						/* allocate varname only if necessary, freed below */
						if (*varprefix != '\0')
							varname = psprintf("%s%s", varprefix, varname);

						/* store last row result as a string */
						if (!putVariable(&st->variables, meta == META_ASET ? "aset" : "gset", varname,
										 PQgetvalue(res, ntuples - 1, fld)))
						{
							/* internal error */
							pg_log_error("client %d script %d command %d query %d: error storing into variable %s",
										 st->id, st->use_file, st->command, qrynum, varname);
							st->estatus = ESTATUS_META_COMMAND_ERROR;
							goto error;
						}

						if (*varprefix != '\0')
							pg_free(varname);
					}
				}
				/* otherwise the result is simply thrown away by PQclear below */
				break;

			case PGRES_PIPELINE_SYNC:
				pg_log_debug("client %d pipeline ending, ongoing syncs: %d",
							 st->id, st->num_syncs);
				st->num_syncs--;
				if (st->num_syncs == 0 && PQexitPipelineMode(st->con) != 1)
					pg_log_error("client %d failed to exit pipeline mode: %s", st->id,
								 PQerrorMessage(st->con));
				break;

			case PGRES_NONFATAL_ERROR:
			case PGRES_FATAL_ERROR:
				st->estatus = getSQLErrorStatus(PQresultErrorField(res,
																   PG_DIAG_SQLSTATE));
				if (canRetryError(st->estatus))
				{
					if (verbose_errors)
						commandError(st, PQerrorMessage(st->con));
					goto error;
				}
				/* fall through */

			default:
				/* anything else is unexpected */
				pg_log_error("client %d script %d aborted in command %d query %d: %s",
							 st->id, st->use_file, st->command, qrynum,
							 PQerrorMessage(st->con));
				goto error;
		}

		PQclear(res);
		qrynum++;
		res = next_res;
	}

	if (qrynum == 0)
	{
		pg_log_error("client %d command %d: no results", st->id, st->command);
		return false;
	}

	return true;

error:
	PQclear(res);
	PQclear(next_res);
	do
	{
		res = PQgetResult(st->con);
		PQclear(res);
	} while (res);

	return false;
}

/*
 * Parse the argument to a \sleep command, and return the requested amount
 * of delay, in microseconds.  Returns true on success, false on error.
 */
static bool
evaluateSleep(Variables *variables, int argc, char **argv, int *usecs)
{
	char	   *var;
	int			usec;

	if (*argv[1] == ':')
	{
		if ((var = getVariable(variables, argv[1] + 1)) == NULL)
		{
			pg_log_error("%s: undefined variable \"%s\"", argv[0], argv[1] + 1);
			return false;
		}

		usec = atoi(var);

		/* Raise an error if the value of a variable is not a number */
		if (usec == 0 && !isdigit((unsigned char) *var))
		{
			pg_log_error("%s: invalid sleep time \"%s\" for variable \"%s\"",
						 argv[0], var, argv[1] + 1);
			return false;
		}
	}
	else
		usec = atoi(argv[1]);

	if (argc > 2)
	{
		if (pg_strcasecmp(argv[2], "ms") == 0)
			usec *= 1000;
		else if (pg_strcasecmp(argv[2], "s") == 0)
			usec *= 1000000;
	}
	else
		usec *= 1000000;

	*usecs = usec;
	return true;
}


/*
 * Returns true if the error can be retried.
 */
static bool
doRetry(CState *st, pg_time_usec_t *now)
{
	Assert(st->estatus != ESTATUS_NO_ERROR);

	/* We can only retry serialization or deadlock errors. */
	if (!canRetryError(st->estatus))
		return false;

	/*
	 * We must have at least one option to limit the retrying of transactions
	 * that got an error.
	 */
	Assert(max_tries || latency_limit || duration > 0);

	/*
	 * We cannot retry the error if we have reached the maximum number of
	 * tries.
	 */
	if (max_tries && st->tries >= max_tries)
		return false;

	/*
	 * We cannot retry the error if we spent too much time on this
	 * transaction.
	 */
	if (latency_limit)
	{
		pg_time_now_lazy(now);
		if (*now - st->txn_scheduled > latency_limit)
			return false;
	}

	/*
	 * We cannot retry the error if the benchmark duration is over.
	 */
	if (timer_exceeded)
		return false;

	/* OK */
	return true;
}

/*
 * Read results and discard it until a sync point.
 */
static int
discardUntilSync(CState *st)
{
	/* send a sync */
	if (!PQpipelineSync(st->con))
	{
		pg_log_error("client %d aborted: failed to send a pipeline sync",
					 st->id);
		return 0;
	}

	/* receive PGRES_PIPELINE_SYNC and null following it */
	for (;;)
	{
		PGresult   *res = PQgetResult(st->con);

		if (PQresultStatus(res) == PGRES_PIPELINE_SYNC)
		{
			PQclear(res);
			res = PQgetResult(st->con);
			Assert(res == NULL);
			break;
		}
		PQclear(res);
	}

	/* exit pipeline */
	if (PQexitPipelineMode(st->con) != 1)
	{
		pg_log_error("client %d aborted: failed to exit pipeline mode for rolling back the failed transaction",
					 st->id);
		return 0;
	}
	return 1;
}

/*
 * Get the transaction status at the end of a command especially for
 * checking if we are in a (failed) transaction block.
 */
static TStatus
getTransactionStatus(PGconn *con)
{
	PGTransactionStatusType tx_status;

	tx_status = PQtransactionStatus(con);
	switch (tx_status)
	{
		case PQTRANS_IDLE:
			return TSTATUS_IDLE;
		case PQTRANS_INTRANS:
		case PQTRANS_INERROR:
			return TSTATUS_IN_BLOCK;
		case PQTRANS_UNKNOWN:
			/* PQTRANS_UNKNOWN is expected given a broken connection */
			if (PQstatus(con) == CONNECTION_BAD)
				return TSTATUS_CONN_ERROR;
			/* fall through */
		case PQTRANS_ACTIVE:
		default:

			/*
			 * We cannot find out whether we are in a transaction block or
			 * not. Internal error which should never occur.
			 */
			pg_log_error("unexpected transaction status %d", tx_status);
			return TSTATUS_OTHER_ERROR;
	}

	/* not reached */
	Assert(false);
	return TSTATUS_OTHER_ERROR;
}

/*
 * Print verbose messages of an error
 */
static void
printVerboseErrorMessages(CState *st, pg_time_usec_t *now, bool is_retry)
{
	static PQExpBuffer buf = NULL;

	if (buf == NULL)
		buf = createPQExpBuffer();
	else
		resetPQExpBuffer(buf);

	printfPQExpBuffer(buf, "client %d ", st->id);
	appendPQExpBufferStr(buf, (is_retry ?
							   "repeats the transaction after the error" :
							   "ends the failed transaction"));
	appendPQExpBuffer(buf, " (try %u", st->tries);

	/* Print max_tries if it is not unlimited. */
	if (max_tries)
		appendPQExpBuffer(buf, "/%u", max_tries);

	/*
	 * If the latency limit is used, print a percentage of the current
	 * transaction latency from the latency limit.
	 */
	if (latency_limit)
	{
		pg_time_now_lazy(now);
		appendPQExpBuffer(buf, ", %.3f%% of the maximum time of tries was used",
						  (100.0 * (*now - st->txn_scheduled) / latency_limit));
	}
	appendPQExpBufferStr(buf, ")\n");

	pg_log_info("%s", buf->data);
}

/*
 * Advance the state machine of a connection.
 */
static void
advanceConnectionState(TState *thread, CState *st, StatsData *agg)
{

	/*
	 * gettimeofday() isn't free, so we get the current timestamp lazily the
	 * first time it's needed, and reuse the same value throughout this
	 * function after that.  This also ensures that e.g. the calculated
	 * latency reported in the log file and in the totals are the same. Zero
	 * means "not set yet".  Reset "now" when we execute shell commands or
	 * expressions, which might take a non-negligible amount of time, though.
	 */
	pg_time_usec_t now = 0;

	/*
	 * Loop in the state machine, until we have to wait for a result from the
	 * server or have to sleep for throttling or \sleep.
	 *
	 * Note: In the switch-statement below, 'break' will loop back here,
	 * meaning "continue in the state machine".  Return is used to return to
	 * the caller, giving the thread the opportunity to advance another
	 * client.
	 */
	for (;;)
	{
		Command    *command;

		switch (st->state)
		{
				/* Select transaction (script) to run.  */
			case CSTATE_CHOOSE_SCRIPT:
				st->use_file = chooseScript(thread);
				Assert(conditional_stack_empty(st->cstack));

				/* reset transaction variables to default values */
				st->estatus = ESTATUS_NO_ERROR;
				st->tries = 1;

				pg_log_debug("client %d executing script \"%s\"",
							 st->id, sql_script[st->use_file].desc);

				/*
				 * If time is over, we're done; otherwise, get ready to start
				 * a new transaction, or to get throttled if that's requested.
				 */
				st->state = timer_exceeded ? CSTATE_FINISHED :
					throttle_delay > 0 ? CSTATE_PREPARE_THROTTLE : CSTATE_START_TX;
				break;

				/* Start new transaction (script) */
			case CSTATE_START_TX:
				pg_time_now_lazy(&now);

				/* establish connection if needed, i.e. under --connect */
				if (st->con == NULL)
				{
					pg_time_usec_t start = now;

					if ((st->con = doConnect()) == NULL)
					{
						/*
						 * as the bench is already running, we do not abort
						 * the process
						 */
						pg_log_error("client %d aborted while establishing connection", st->id);
						st->state = CSTATE_ABORTED;
						break;
					}

					/* reset now after connection */
					now = pg_time_now();

					thread->conn_duration += now - start;

					/* Reset session-local state */
					pg_free(st->prepared);
					st->prepared = NULL;
				}

				/*
				 * It is the first try to run this transaction. Remember the
				 * random state: maybe it will get an error and we will need
				 * to run it again.
				 */
				st->random_state = st->cs_func_rs;

				/* record transaction start time */
				st->txn_begin = now;

				/*
				 * When not throttling, this is also the transaction's
				 * scheduled start time.
				 */
				if (!throttle_delay)
					st->txn_scheduled = now;

				/* Begin with the first command */
				st->state = CSTATE_START_COMMAND;
				st->command = 0;
				break;

				/*
				 * Handle throttling once per transaction by sleeping.
				 */
			case CSTATE_PREPARE_THROTTLE:

				/*
				 * Generate a delay such that the series of delays will
				 * approximate a Poisson distribution centered on the
				 * throttle_delay time.
				 *
				 * If transactions are too slow or a given wait is shorter
				 * than a transaction, the next transaction will start right
				 * away.
				 */
				Assert(throttle_delay > 0);

				thread->throttle_trigger +=
					getPoissonRand(&thread->ts_throttle_rs, throttle_delay);
				st->txn_scheduled = thread->throttle_trigger;

				/*
				 * If --latency-limit is used, and this slot is already late
				 * so that the transaction will miss the latency limit even if
				 * it completed immediately, skip this time slot and loop to
				 * reschedule.
				 */
				if (latency_limit)
				{
					pg_time_now_lazy(&now);

					if (thread->throttle_trigger < now - latency_limit)
					{
						processXactStats(thread, st, &now, true, agg);

						/*
						 * Finish client if -T or -t was exceeded.
						 *
						 * Stop counting skipped transactions under -T as soon
						 * as the timer is exceeded. Because otherwise it can
						 * take a very long time to count all of them
						 * especially when quite a lot of them happen with
						 * unrealistically high rate setting in -R, which
						 * would prevent pgbench from ending immediately.
						 * Because of this behavior, note that there is no
						 * guarantee that all skipped transactions are counted
						 * under -T though there is under -t. This is OK in
						 * practice because it's very unlikely to happen with
						 * realistic setting.
						 */
						if (timer_exceeded || (nxacts > 0 && st->cnt >= nxacts))
							st->state = CSTATE_FINISHED;

						/* Go back to top of loop with CSTATE_PREPARE_THROTTLE */
						break;
					}
				}

				/*
				 * stop client if next transaction is beyond pgbench end of
				 * execution; otherwise, throttle it.
				 */
				st->state = end_time > 0 && st->txn_scheduled > end_time ?
					CSTATE_FINISHED : CSTATE_THROTTLE;
				break;

				/*
				 * Wait until it's time to start next transaction.
				 */
			case CSTATE_THROTTLE:
				pg_time_now_lazy(&now);

				if (now < st->txn_scheduled)
					return;		/* still sleeping, nothing to do here */

				/* done sleeping, but don't start transaction if we're done */
				st->state = timer_exceeded ? CSTATE_FINISHED : CSTATE_START_TX;
				break;

				/*
				 * Send a command to server (or execute a meta-command)
				 */
			case CSTATE_START_COMMAND:
				command = sql_script[st->use_file].commands[st->command];

				/*
				 * Transition to script end processing if done, but close up
				 * shop if a pipeline is open at this point.
				 */
				if (command == NULL)
				{
					if (PQpipelineStatus(st->con) == PQ_PIPELINE_OFF)
						st->state = CSTATE_END_TX;
					else
					{
						pg_log_error("client %d aborted: end of script reached with pipeline open",
									 st->id);
						st->state = CSTATE_ABORTED;
					}

					break;
				}

				/* record begin time of next command, and initiate it */
				if (report_per_command)
				{
					pg_time_now_lazy(&now);
					st->stmt_begin = now;
				}

				/* Execute the command */
				if (command->type == SQL_COMMAND)
				{
					/* disallow \aset and \gset in pipeline mode */
					if (PQpipelineStatus(st->con) != PQ_PIPELINE_OFF)
					{
						if (command->meta == META_GSET)
						{
							commandFailed(st, "gset", "\\gset is not allowed in pipeline mode");
							st->state = CSTATE_ABORTED;
							break;
						}
						else if (command->meta == META_ASET)
						{
							commandFailed(st, "aset", "\\aset is not allowed in pipeline mode");
							st->state = CSTATE_ABORTED;
							break;
						}
					}

					if (!sendCommand(st, command))
					{
						commandFailed(st, "SQL", "SQL command send failed");
						st->state = CSTATE_ABORTED;
					}
					else
					{
						/* Wait for results, unless in pipeline mode */
						if (PQpipelineStatus(st->con) == PQ_PIPELINE_OFF)
							st->state = CSTATE_WAIT_RESULT;
						else
							st->state = CSTATE_END_COMMAND;
					}
				}
				else if (command->type == META_COMMAND)
				{
					/*-----
					 * Possible state changes when executing meta commands:
					 * - on errors CSTATE_ABORTED
					 * - on sleep CSTATE_SLEEP
					 * - else CSTATE_END_COMMAND
					 */
					st->state = executeMetaCommand(st, &now);
					if (st->state == CSTATE_ABORTED)
						st->estatus = ESTATUS_META_COMMAND_ERROR;
				}

				/*
				 * We're now waiting for an SQL command to complete, or
				 * finished processing a metacommand, or need to sleep, or
				 * something bad happened.
				 */
				Assert(st->state == CSTATE_WAIT_RESULT ||
					   st->state == CSTATE_END_COMMAND ||
					   st->state == CSTATE_SLEEP ||
					   st->state == CSTATE_ABORTED);
				break;

				/*
				 * non executed conditional branch
				 */
			case CSTATE_SKIP_COMMAND:
				Assert(!conditional_active(st->cstack));
				/* quickly skip commands until something to do... */
				while (true)
				{
					command = sql_script[st->use_file].commands[st->command];

					/* cannot reach end of script in that state */
					Assert(command != NULL);

					/*
					 * if this is conditional related, update conditional
					 * state
					 */
					if (command->type == META_COMMAND &&
						(command->meta == META_IF ||
						 command->meta == META_ELIF ||
						 command->meta == META_ELSE ||
						 command->meta == META_ENDIF))
					{
						switch (conditional_stack_peek(st->cstack))
						{
							case IFSTATE_FALSE:
								if (command->meta == META_IF)
								{
									/* nested if in skipped branch - ignore */
									conditional_stack_push(st->cstack,
														   IFSTATE_IGNORED);
									st->command++;
								}
								else if (command->meta == META_ELIF)
								{
									/* we must evaluate the condition */
									st->state = CSTATE_START_COMMAND;
								}
								else if (command->meta == META_ELSE)
								{
									/* we must execute next command */
									conditional_stack_poke(st->cstack,
														   IFSTATE_ELSE_TRUE);
									st->state = CSTATE_START_COMMAND;
									st->command++;
								}
								else if (command->meta == META_ENDIF)
								{
									Assert(!conditional_stack_empty(st->cstack));
									conditional_stack_pop(st->cstack);
									if (conditional_active(st->cstack))
										st->state = CSTATE_START_COMMAND;
									/* else state remains CSTATE_SKIP_COMMAND */
									st->command++;
								}
								break;

							case IFSTATE_IGNORED:
							case IFSTATE_ELSE_FALSE:
								if (command->meta == META_IF)
									conditional_stack_push(st->cstack,
														   IFSTATE_IGNORED);
								else if (command->meta == META_ENDIF)
								{
									Assert(!conditional_stack_empty(st->cstack));
									conditional_stack_pop(st->cstack);
									if (conditional_active(st->cstack))
										st->state = CSTATE_START_COMMAND;
								}
								/* could detect "else" & "elif" after "else" */
								st->command++;
								break;

							case IFSTATE_NONE:
							case IFSTATE_TRUE:
							case IFSTATE_ELSE_TRUE:
							default:

								/*
								 * inconsistent if inactive, unreachable dead
								 * code
								 */
								Assert(false);
						}
					}
					else
					{
						/* skip and consider next */
						st->command++;
					}

					if (st->state != CSTATE_SKIP_COMMAND)
						/* out of quick skip command loop */
						break;
				}
				break;

				/*
				 * Wait for the current SQL command to complete
				 */
			case CSTATE_WAIT_RESULT:
				pg_log_debug("client %d receiving", st->id);

				/*
				 * Only check for new network data if we processed all data
				 * fetched prior. Otherwise we end up doing a syscall for each
				 * individual pipelined query, which has a measurable
				 * performance impact.
				 */
				if (PQisBusy(st->con) && !PQconsumeInput(st->con))
				{
					/* there's something wrong */
					commandFailed(st, "SQL", "perhaps the backend died while processing");
					st->state = CSTATE_ABORTED;
					break;
				}
				if (PQisBusy(st->con))
					return;		/* don't have the whole result yet */

				/* store or discard the query results */
				if (readCommandResponse(st,
										sql_script[st->use_file].commands[st->command]->meta,
										sql_script[st->use_file].commands[st->command]->varprefix))
				{
					/*
					 * outside of pipeline mode: stop reading results.
					 * pipeline mode: continue reading results until an
					 * end-of-pipeline response.
					 */
					if (PQpipelineStatus(st->con) != PQ_PIPELINE_ON)
						st->state = CSTATE_END_COMMAND;
				}
				else if (canRetryError(st->estatus))
					st->state = CSTATE_ERROR;
				else
					st->state = CSTATE_ABORTED;
				break;

				/*
				 * Wait until sleep is done. This state is entered after a
				 * \sleep metacommand. The behavior is similar to
				 * CSTATE_THROTTLE, but proceeds to CSTATE_START_COMMAND
				 * instead of CSTATE_START_TX.
				 */
			case CSTATE_SLEEP:
				pg_time_now_lazy(&now);
				if (now < st->sleep_until)
					return;		/* still sleeping, nothing to do here */
				/* Else done sleeping. */
				st->state = CSTATE_END_COMMAND;
				break;

				/*
				 * End of command: record stats and proceed to next command.
				 */
			case CSTATE_END_COMMAND:

				/*
				 * command completed: accumulate per-command execution times
				 * in thread-local data structure, if per-command latencies
				 * are requested.
				 */
				if (report_per_command)
				{
					pg_time_now_lazy(&now);

					command = sql_script[st->use_file].commands[st->command];
					/* XXX could use a mutex here, but we choose not to */
					addToSimpleStats(&command->stats,
									 PG_TIME_GET_DOUBLE(now - st->stmt_begin));
				}

				/* Go ahead with next command, to be executed or skipped */
				st->command++;
				st->state = conditional_active(st->cstack) ?
					CSTATE_START_COMMAND : CSTATE_SKIP_COMMAND;
				break;

				/*
				 * Clean up after an error.
				 */
			case CSTATE_ERROR:
				{
					TStatus		tstatus;

					Assert(st->estatus != ESTATUS_NO_ERROR);

					/* Clear the conditional stack */
					conditional_stack_reset(st->cstack);

					/* Read and discard until a sync point in pipeline mode */
					if (PQpipelineStatus(st->con) != PQ_PIPELINE_OFF)
					{
						if (!discardUntilSync(st))
						{
							st->state = CSTATE_ABORTED;
							break;
						}
					}

					/*
					 * Check if we have a (failed) transaction block or not,
					 * and roll it back if any.
					 */
					tstatus = getTransactionStatus(st->con);
					if (tstatus == TSTATUS_IN_BLOCK)
					{
						/* Try to rollback a (failed) transaction block. */
						if (!PQsendQuery(st->con, "ROLLBACK"))
						{
							pg_log_error("client %d aborted: failed to send sql command for rolling back the failed transaction",
										 st->id);
							st->state = CSTATE_ABORTED;
						}
						else
							st->state = CSTATE_WAIT_ROLLBACK_RESULT;
					}
					else if (tstatus == TSTATUS_IDLE)
					{
						/*
						 * If time is over, we're done; otherwise, check if we
						 * can retry the error.
						 */
						st->state = timer_exceeded ? CSTATE_FINISHED :
							doRetry(st, &now) ? CSTATE_RETRY : CSTATE_FAILURE;
					}
					else
					{
						if (tstatus == TSTATUS_CONN_ERROR)
							pg_log_error("perhaps the backend died while processing");

						pg_log_error("client %d aborted while receiving the transaction status", st->id);
						st->state = CSTATE_ABORTED;
					}
					break;
				}

				/*
				 * Wait for the rollback command to complete
				 */
			case CSTATE_WAIT_ROLLBACK_RESULT:
				{
					PGresult   *res;

					pg_log_debug("client %d receiving", st->id);
					if (!PQconsumeInput(st->con))
					{
						pg_log_error("client %d aborted while rolling back the transaction after an error; perhaps the backend died while processing",
									 st->id);
						st->state = CSTATE_ABORTED;
						break;
					}
					if (PQisBusy(st->con))
						return; /* don't have the whole result yet */

					/*
					 * Read and discard the query result;
					 */
					res = PQgetResult(st->con);
					switch (PQresultStatus(res))
					{
						case PGRES_COMMAND_OK:
							/* OK */
							PQclear(res);
							/* null must be returned */
							res = PQgetResult(st->con);
							Assert(res == NULL);

							/*
							 * If time is over, we're done; otherwise, check
							 * if we can retry the error.
							 */
							st->state = timer_exceeded ? CSTATE_FINISHED :
								doRetry(st, &now) ? CSTATE_RETRY : CSTATE_FAILURE;
							break;
						default:
							pg_log_error("client %d aborted while rolling back the transaction after an error; %s",
										 st->id, PQerrorMessage(st->con));
							PQclear(res);
							st->state = CSTATE_ABORTED;
							break;
					}
					break;
				}

				/*
				 * Retry the transaction after an error.
				 */
			case CSTATE_RETRY:
				command = sql_script[st->use_file].commands[st->command];

				/*
				 * Inform that the transaction will be retried after the
				 * error.
				 */
				if (verbose_errors)
					printVerboseErrorMessages(st, &now, true);

				/* Count tries and retries */
				st->tries++;
				command->retries++;

				/*
				 * Reset the random state as they were at the beginning of the
				 * transaction.
				 */
				st->cs_func_rs = st->random_state;

				/* Process the first transaction command. */
				st->command = 0;
				st->estatus = ESTATUS_NO_ERROR;
				st->state = CSTATE_START_COMMAND;
				break;

				/*
				 * Record a failed transaction.
				 */
			case CSTATE_FAILURE:
				command = sql_script[st->use_file].commands[st->command];

				/* Accumulate the failure. */
				command->failures++;

				/*
				 * Inform that the failed transaction will not be retried.
				 */
				if (verbose_errors)
					printVerboseErrorMessages(st, &now, false);

				/* End the failed transaction. */
				st->state = CSTATE_END_TX;
				break;

				/*
				 * End of transaction (end of script, really).
				 */
			case CSTATE_END_TX:
				{
					TStatus		tstatus;

					/* transaction finished: calculate latency and do log */
					processXactStats(thread, st, &now, false, agg);

					/*
					 * missing \endif... cannot happen if CheckConditional was
					 * okay
					 */
					Assert(conditional_stack_empty(st->cstack));

					/*
					 * We must complete all the transaction blocks that were
					 * started in this script.
					 */
					tstatus = getTransactionStatus(st->con);
					if (tstatus == TSTATUS_IN_BLOCK)
					{
						pg_log_error("client %d aborted: end of script reached without completing the last transaction",
									 st->id);
						st->state = CSTATE_ABORTED;
						break;
					}
					else if (tstatus != TSTATUS_IDLE)
					{
						if (tstatus == TSTATUS_CONN_ERROR)
							pg_log_error("perhaps the backend died while processing");

						pg_log_error("client %d aborted while receiving the transaction status", st->id);
						st->state = CSTATE_ABORTED;
						break;
					}

					if (is_connect)
					{
						pg_time_usec_t start = now;

						pg_time_now_lazy(&start);
						finishCon(st);
						now = pg_time_now();
						thread->conn_duration += now - start;
					}

					if ((st->cnt >= nxacts && duration <= 0) || timer_exceeded)
					{
						/* script completed */
						st->state = CSTATE_FINISHED;
						break;
					}

					/* next transaction (script) */
					st->state = CSTATE_CHOOSE_SCRIPT;

					/*
					 * Ensure that we always return on this point, so as to
					 * avoid an infinite loop if the script only contains meta
					 * commands.
					 */
					return;
				}

				/*
				 * Final states.  Close the connection if it's still open.
				 */
			case CSTATE_ABORTED:
			case CSTATE_FINISHED:

				/*
				 * Don't measure the disconnection delays here even if in
				 * CSTATE_FINISHED and -C/--connect option is specified.
				 * Because in this case all the connections that this thread
				 * established are closed at the end of transactions and the
				 * disconnection delays should have already been measured at
				 * that moment.
				 *
				 * In CSTATE_ABORTED state, the measurement is no longer
				 * necessary because we cannot report complete results anyways
				 * in this case.
				 */
				finishCon(st);
				return;
		}
	}
}

/*
 * Subroutine for advanceConnectionState -- initiate or execute the current
 * meta command, and return the next state to set.
 *
 * *now is updated to the current time, unless the command is expected to
 * take no time to execute.
 */
static ConnectionStateEnum
executeMetaCommand(CState *st, pg_time_usec_t *now)
{
	Command    *command = sql_script[st->use_file].commands[st->command];
	int			argc;
	char	  **argv;

	Assert(command != NULL && command->type == META_COMMAND);

	argc = command->argc;
	argv = command->argv;

	if (unlikely(__pg_log_level <= PG_LOG_DEBUG))
	{
		PQExpBufferData buf;

		initPQExpBuffer(&buf);

		printfPQExpBuffer(&buf, "client %d executing \\%s", st->id, argv[0]);
		for (int i = 1; i < argc; i++)
			appendPQExpBuffer(&buf, " %s", argv[i]);

		pg_log_debug("%s", buf.data);

		termPQExpBuffer(&buf);
	}

	if (command->meta == META_SLEEP)
	{
		int			usec;

		/*
		 * A \sleep doesn't execute anything, we just get the delay from the
		 * argument, and enter the CSTATE_SLEEP state.  (The per-command
		 * latency will be recorded in CSTATE_SLEEP state, not here, after the
		 * delay has elapsed.)
		 */
		if (!evaluateSleep(&st->variables, argc, argv, &usec))
		{
			commandFailed(st, "sleep", "execution of meta-command failed");
			return CSTATE_ABORTED;
		}

		pg_time_now_lazy(now);
		st->sleep_until = (*now) + usec;
		return CSTATE_SLEEP;
	}
	else if (command->meta == META_SET)
	{
		PgBenchExpr *expr = command->expr;
		PgBenchValue result;

		if (!evaluateExpr(st, expr, &result))
		{
			commandFailed(st, argv[0], "evaluation of meta-command failed");
			return CSTATE_ABORTED;
		}

		if (!putVariableValue(&st->variables, argv[0], argv[1], &result))
		{
			commandFailed(st, "set", "assignment of meta-command failed");
			return CSTATE_ABORTED;
		}
	}
	else if (command->meta == META_IF)
	{
		/* backslash commands with an expression to evaluate */
		PgBenchExpr *expr = command->expr;
		PgBenchValue result;
		bool		cond;

		if (!evaluateExpr(st, expr, &result))
		{
			commandFailed(st, argv[0], "evaluation of meta-command failed");
			return CSTATE_ABORTED;
		}

		cond = valueTruth(&result);
		conditional_stack_push(st->cstack, cond ? IFSTATE_TRUE : IFSTATE_FALSE);
	}
	else if (command->meta == META_ELIF)
	{
		/* backslash commands with an expression to evaluate */
		PgBenchExpr *expr = command->expr;
		PgBenchValue result;
		bool		cond;

		if (conditional_stack_peek(st->cstack) == IFSTATE_TRUE)
		{
			/* elif after executed block, skip eval and wait for endif. */
			conditional_stack_poke(st->cstack, IFSTATE_IGNORED);
			return CSTATE_END_COMMAND;
		}

		if (!evaluateExpr(st, expr, &result))
		{
			commandFailed(st, argv[0], "evaluation of meta-command failed");
			return CSTATE_ABORTED;
		}

		cond = valueTruth(&result);
		Assert(conditional_stack_peek(st->cstack) == IFSTATE_FALSE);
		conditional_stack_poke(st->cstack, cond ? IFSTATE_TRUE : IFSTATE_FALSE);
	}
	else if (command->meta == META_ELSE)
	{
		switch (conditional_stack_peek(st->cstack))
		{
			case IFSTATE_TRUE:
				conditional_stack_poke(st->cstack, IFSTATE_ELSE_FALSE);
				break;
			case IFSTATE_FALSE: /* inconsistent if active */
			case IFSTATE_IGNORED:	/* inconsistent if active */
			case IFSTATE_NONE:	/* else without if */
			case IFSTATE_ELSE_TRUE: /* else after else */
			case IFSTATE_ELSE_FALSE:	/* else after else */
			default:
				/* dead code if conditional check is ok */
				Assert(false);
		}
	}
	else if (command->meta == META_ENDIF)
	{
		Assert(!conditional_stack_empty(st->cstack));
		conditional_stack_pop(st->cstack);
	}
	else if (command->meta == META_SETSHELL)
	{
		if (!runShellCommand(&st->variables, argv[1], argv + 2, argc - 2))
		{
			commandFailed(st, "setshell", "execution of meta-command failed");
			return CSTATE_ABORTED;
		}
	}
	else if (command->meta == META_SHELL)
	{
		if (!runShellCommand(&st->variables, NULL, argv + 1, argc - 1))
		{
			commandFailed(st, "shell", "execution of meta-command failed");
			return CSTATE_ABORTED;
		}
	}
	else if (command->meta == META_STARTPIPELINE)
	{
		/*
		 * In pipeline mode, we use a workflow based on libpq pipeline
		 * functions.
		 */
		if (querymode == QUERY_SIMPLE)
		{
			commandFailed(st, "startpipeline", "cannot use pipeline mode with the simple query protocol");
			return CSTATE_ABORTED;
		}

		/*
		 * If we're in prepared-query mode, we need to prepare all the
		 * commands that are inside the pipeline before we actually start the
		 * pipeline itself.  This solves the problem that running BEGIN
		 * ISOLATION LEVEL SERIALIZABLE in a pipeline would fail due to a
		 * snapshot having been acquired by the prepare within the pipeline.
		 */
		if (querymode == QUERY_PREPARED)
			prepareCommandsInPipeline(st);

		if (PQpipelineStatus(st->con) != PQ_PIPELINE_OFF)
		{
			commandFailed(st, "startpipeline", "already in pipeline mode");
			return CSTATE_ABORTED;
		}
		if (PQenterPipelineMode(st->con) == 0)
		{
			commandFailed(st, "startpipeline", "failed to enter pipeline mode");
			return CSTATE_ABORTED;
		}
	}
	else if (command->meta == META_SYNCPIPELINE)
	{
		if (PQpipelineStatus(st->con) != PQ_PIPELINE_ON)
		{
			commandFailed(st, "syncpipeline", "not in pipeline mode");
			return CSTATE_ABORTED;
		}
		if (PQsendPipelineSync(st->con) == 0)
		{
			commandFailed(st, "syncpipeline", "failed to send a pipeline sync");
			return CSTATE_ABORTED;
		}
		st->num_syncs++;
	}
	else if (command->meta == META_ENDPIPELINE)
	{
		if (PQpipelineStatus(st->con) != PQ_PIPELINE_ON)
		{
			commandFailed(st, "endpipeline", "not in pipeline mode");
			return CSTATE_ABORTED;
		}
		if (!PQpipelineSync(st->con))
		{
			commandFailed(st, "endpipeline", "failed to send a pipeline sync");
			return CSTATE_ABORTED;
		}
		st->num_syncs++;
		/* Now wait for the PGRES_PIPELINE_SYNC and exit pipeline mode there */
		/* collect pending results before getting out of pipeline mode */
		return CSTATE_WAIT_RESULT;
	}

	/*
	 * executing the expression or shell command might have taken a
	 * non-negligible amount of time, so reset 'now'
	 */
	*now = 0;

	return CSTATE_END_COMMAND;
}

/*
 * Return the number of failed transactions.
 */
static int64
getFailures(const StatsData *stats)
{
	return (stats->serialization_failures +
			stats->deadlock_failures);
}

/*
 * Return a string constant representing the result of a transaction
 * that is not successfully processed.
 */
static const char *
getResultString(bool skipped, EStatus estatus)
{
	if (skipped)
		return "skipped";
	else if (failures_detailed)
	{
		switch (estatus)
		{
			case ESTATUS_SERIALIZATION_ERROR:
				return "serialization";
			case ESTATUS_DEADLOCK_ERROR:
				return "deadlock";
			default:
				/* internal error which should never occur */
				pg_fatal("unexpected error status: %d", estatus);
		}
	}
	else
		return "failed";
}

/*
 * Print log entry after completing one transaction.
 *
 * We print Unix-epoch timestamps in the log, so that entries can be
 * correlated against other logs.
 *
 * XXX We could obtain the time from the caller and just shift it here, to
 * avoid the cost of an extra call to pg_time_now().
 */
static void
doLog(TState *thread, CState *st,
	  StatsData *agg, bool skipped, double latency, double lag)
{
	FILE	   *logfile = thread->logfile;
	pg_time_usec_t now = pg_time_now() + epoch_shift;

	Assert(use_log);

	/*
	 * Skip the log entry if sampling is enabled and this row doesn't belong
	 * to the random sample.
	 */
	if (sample_rate != 0.0 &&
		pg_prng_double(&thread->ts_sample_rs) > sample_rate)
		return;

	/* should we aggregate the results or not? */
	if (agg_interval > 0)
	{
		pg_time_usec_t next;

		/*
		 * Loop until we reach the interval of the current moment, and print
		 * any empty intervals in between (this may happen with very low tps,
		 * e.g. --rate=0.1).
		 */

		while ((next = agg->start_time + agg_interval * INT64CONST(1000000)) <= now)
		{
			double		lag_sum = 0.0;
			double		lag_sum2 = 0.0;
			double		lag_min = 0.0;
			double		lag_max = 0.0;
			int64		skipped = 0;
			int64		serialization_failures = 0;
			int64		deadlock_failures = 0;
			int64		retried = 0;
			int64		retries = 0;

			/* print aggregated report to logfile */
			fprintf(logfile, INT64_FORMAT " " INT64_FORMAT " %.0f %.0f %.0f %.0f",
					agg->start_time / 1000000,	/* seconds since Unix epoch */
					agg->cnt,
					agg->latency.sum,
					agg->latency.sum2,
					agg->latency.min,
					agg->latency.max);

			if (throttle_delay)
			{
				lag_sum = agg->lag.sum;
				lag_sum2 = agg->lag.sum2;
				lag_min = agg->lag.min;
				lag_max = agg->lag.max;
			}
			fprintf(logfile, " %.0f %.0f %.0f %.0f",
					lag_sum,
					lag_sum2,
					lag_min,
					lag_max);

			if (latency_limit)
				skipped = agg->skipped;
			fprintf(logfile, " " INT64_FORMAT, skipped);

			if (max_tries != 1)
			{
				retried = agg->retried;
				retries = agg->retries;
			}
			fprintf(logfile, " " INT64_FORMAT " " INT64_FORMAT, retried, retries);

			if (failures_detailed)
			{
				serialization_failures = agg->serialization_failures;
				deadlock_failures = agg->deadlock_failures;
			}
			fprintf(logfile, " " INT64_FORMAT " " INT64_FORMAT,
					serialization_failures,
					deadlock_failures);

			fputc('\n', logfile);

			/* reset data and move to next interval */
			initStats(agg, next);
		}

		/* accumulate the current transaction */
		accumStats(agg, skipped, latency, lag, st->estatus, st->tries);
	}
	else
	{
		/* no, print raw transactions */
		if (!skipped && st->estatus == ESTATUS_NO_ERROR)
			fprintf(logfile, "%d " INT64_FORMAT " %.0f %d " INT64_FORMAT " "
					INT64_FORMAT,
					st->id, st->cnt, latency, st->use_file,
					now / 1000000, now % 1000000);
		else
			fprintf(logfile, "%d " INT64_FORMAT " %s %d " INT64_FORMAT " "
					INT64_FORMAT,
					st->id, st->cnt, getResultString(skipped, st->estatus),
					st->use_file, now / 1000000, now % 1000000);

		if (throttle_delay)
			fprintf(logfile, " %.0f", lag);
		if (max_tries != 1)
			fprintf(logfile, " %u", st->tries - 1);
		fputc('\n', logfile);
	}
}

/*
 * Accumulate and report statistics at end of a transaction.
 *
 * (This is also called when a transaction is late and thus skipped.
 * Note that even skipped and failed transactions are counted in the CState
 * "cnt" field.)
 */
static void
processXactStats(TState *thread, CState *st, pg_time_usec_t *now,
				 bool skipped, StatsData *agg)
{
	double		latency = 0.0,
				lag = 0.0;
	bool		detailed = progress || throttle_delay || latency_limit ||
		use_log || per_script_stats;

	if (detailed && !skipped && st->estatus == ESTATUS_NO_ERROR)
	{
		pg_time_now_lazy(now);

		/* compute latency & lag */
		latency = (*now) - st->txn_scheduled;
		lag = st->txn_begin - st->txn_scheduled;
	}

	/* keep detailed thread stats */
	accumStats(&thread->stats, skipped, latency, lag, st->estatus, st->tries);

	/* count transactions over the latency limit, if needed */
	if (latency_limit && latency > latency_limit)
		thread->latency_late++;

	/* client stat is just counting */
	st->cnt++;

	if (use_log)
		doLog(thread, st, agg, skipped, latency, lag);

	/* XXX could use a mutex here, but we choose not to */
	if (per_script_stats)
		accumStats(&sql_script[st->use_file].stats, skipped, latency, lag,
				   st->estatus, st->tries);
}


/* discard connections */
static void
disconnect_all(CState *state, int length)
{
	int			i;

	for (i = 0; i < length; i++)
		finishCon(&state[i]);
}

/*
 * Remove old pgbench tables, if any exist
 */
static void
initDropTables(PGconn *con)
{
	fprintf(stderr, "dropping old tables...\n");

	/*
	 * We drop all the tables in one command, so that whether there are
	 * foreign key dependencies or not doesn't matter.
	 */
	executeStatement(con, "drop table if exists "
					 "pgbench_accounts, "
					 "pgbench_branches, "
					 "pgbench_history, "
					 "pgbench_tellers");
}

/*
 * Create "pgbench_accounts" partitions if needed.
 *
 * This is the larger table of pgbench default tpc-b like schema
 * with a known size, so we choose to partition it.
 */
static void
createPartitions(PGconn *con)
{
	PQExpBufferData query;

	/* we must have to create some partitions */
	Assert(partitions > 0);

	fprintf(stderr, "creating %d partitions...\n", partitions);

	initPQExpBuffer(&query);

	for (int p = 1; p <= partitions; p++)
	{
		if (partition_method == PART_RANGE)
		{
			int64		part_size = (naccounts * (int64) scale + partitions - 1) / partitions;

			printfPQExpBuffer(&query,
							  "create%s table pgbench_accounts_%d\n"
							  "  partition of pgbench_accounts\n"
							  "  for values from (",
							  unlogged_tables ? " unlogged" : "", p);

			/*
			 * For RANGE, we use open-ended partitions at the beginning and
			 * end to allow any valid value for the primary key.  Although the
			 * actual minimum and maximum values can be derived from the
			 * scale, it is more generic and the performance is better.
			 */
			if (p == 1)
				appendPQExpBufferStr(&query, "minvalue");
			else
				appendPQExpBuffer(&query, INT64_FORMAT, (p - 1) * part_size + 1);

			appendPQExpBufferStr(&query, ") to (");

			if (p < partitions)
				appendPQExpBuffer(&query, INT64_FORMAT, p * part_size + 1);
			else
				appendPQExpBufferStr(&query, "maxvalue");

			appendPQExpBufferChar(&query, ')');
		}
		else if (partition_method == PART_HASH)
			printfPQExpBuffer(&query,
							  "create%s table pgbench_accounts_%d\n"
							  "  partition of pgbench_accounts\n"
							  "  for values with (modulus %d, remainder %d)",
							  unlogged_tables ? " unlogged" : "", p,
							  partitions, p - 1);
		else					/* cannot get there */
			Assert(0);

		/*
		 * Per ddlinfo in initCreateTables, fillfactor is needed on table
		 * pgbench_accounts.
		 */
		appendPQExpBuffer(&query, " with (fillfactor=%d)", fillfactor);

		executeStatement(con, query.data);
	}

	termPQExpBuffer(&query);
}

/*
 * Create pgbench's standard tables
 */
static void
initCreateTables(PGconn *con)
{
	/*
	 * Note: TPC-B requires at least 100 bytes per row, and the "filler"
	 * fields in these table declarations were intended to comply with that.
	 * The pgbench_accounts table complies with that because the "filler"
	 * column is set to blank-padded empty string. But for all other tables
	 * the columns default to NULL and so don't actually take any space.  We
	 * could fix that by giving them non-null default values.  However, that
	 * would completely break comparability of pgbench results with prior
	 * versions. Since pgbench has never pretended to be fully TPC-B compliant
	 * anyway, we stick with the historical behavior.
	 */
	struct ddlinfo
	{
		const char *table;		/* table name */
		const char *smcols;		/* column decls if accountIDs are 32 bits */
		const char *bigcols;	/* column decls if accountIDs are 64 bits */
		int			declare_fillfactor;
	};
	static const struct ddlinfo DDLs[] = {
		{
			"pgbench_history",
			"tid int,bid int,aid    int,delta int,mtime timestamp,filler char(22)",
			"tid int,bid int,aid bigint,delta int,mtime timestamp,filler char(22)",
			0
		},
		{
			"pgbench_tellers",
			"tid int not null,bid int,tbalance int,filler char(84)",
			"tid int not null,bid int,tbalance int,filler char(84)",
			1
		},
		{
			"pgbench_accounts",
			"aid    int not null,bid int,abalance int,filler char(84)",
			"aid bigint not null,bid int,abalance int,filler char(84)",
			1
		},
		{
			"pgbench_branches",
			"bid int not null,bbalance int,filler char(88)",
			"bid int not null,bbalance int,filler char(88)",
			1
		}
	};
	int			i;
	PQExpBufferData query;

	fprintf(stderr, "creating tables...\n");

	initPQExpBuffer(&query);

	for (i = 0; i < lengthof(DDLs); i++)
	{
		const struct ddlinfo *ddl = &DDLs[i];

		/* Construct new create table statement. */
		printfPQExpBuffer(&query, "create%s table %s(%s)",
						  (unlogged_tables && partition_method == PART_NONE) ? " unlogged" : "",
						  ddl->table,
						  (scale >= SCALE_32BIT_THRESHOLD) ? ddl->bigcols : ddl->smcols);

		/* Partition pgbench_accounts table */
		if (partition_method != PART_NONE && strcmp(ddl->table, "pgbench_accounts") == 0)
			appendPQExpBuffer(&query,
							  " partition by %s (aid)", PARTITION_METHOD[partition_method]);
		else if (ddl->declare_fillfactor)
		{
			/* fillfactor is only expected on actual tables */
			appendPQExpBuffer(&query, " with (fillfactor=%d)", fillfactor);
		}

		if (tablespace != NULL)
		{
			char	   *escape_tablespace;

			escape_tablespace = PQescapeIdentifier(con, tablespace, strlen(tablespace));
			appendPQExpBuffer(&query, " tablespace %s", escape_tablespace);
			PQfreemem(escape_tablespace);
		}

		executeStatement(con, query.data);
	}

	termPQExpBuffer(&query);

	if (partition_method != PART_NONE)
		createPartitions(con);
}

/*
 * Truncate away any old data, in one command in case there are foreign keys
 */
static void
initTruncateTables(PGconn *con)
{
	executeStatement(con, "truncate table "
					 "pgbench_accounts, "
					 "pgbench_branches, "
					 "pgbench_history, "
					 "pgbench_tellers");
}

static void
initBranch(PQExpBufferData *sql, int64 curr)
{
	/* "filler" column uses NULL */
	printfPQExpBuffer(sql,
					  INT64_FORMAT "\t0\t\\N\n",
					  curr + 1);
}

static void
initTeller(PQExpBufferData *sql, int64 curr)
{
	/* "filler" column uses NULL */
	printfPQExpBuffer(sql,
					  INT64_FORMAT "\t" INT64_FORMAT "\t0\t\\N\n",
					  curr + 1, curr / ntellers + 1);
}

static void
initAccount(PQExpBufferData *sql, int64 curr)
{
	/* "filler" column defaults to blank padded empty string */
	printfPQExpBuffer(sql,
					  INT64_FORMAT "\t" INT64_FORMAT "\t0\t\n",
					  curr + 1, curr / naccounts + 1);
}

static void
initPopulateTable(PGconn *con, const char *table, int64 base,
				  initRowMethod init_row)
{
	int			n;
	int64		k;
	int			chars = 0;
	int			prev_chars = 0;
	PGresult   *res;
	PQExpBufferData sql;
	char		copy_statement[256];
	const char *copy_statement_fmt = "copy %s from stdin";
	int64		total = base * scale;

	/* used to track elapsed time and estimate of the remaining time */
	pg_time_usec_t start;
	int			log_interval = 1;

	/* Stay on the same line if reporting to a terminal */
	char		eol = isatty(fileno(stderr)) ? '\r' : '\n';

	initPQExpBuffer(&sql);

	/* Use COPY with FREEZE on v14 and later for all ordinary tables */
	if ((PQserverVersion(con) >= 140000) &&
		get_table_relkind(con, table) == RELKIND_RELATION)
		copy_statement_fmt = "copy %s from stdin with (freeze on)";


	n = pg_snprintf(copy_statement, sizeof(copy_statement), copy_statement_fmt, table);
	if (n >= sizeof(copy_statement))
		pg_fatal("invalid buffer size: must be at least %d characters long", n);
	else if (n == -1)
		pg_fatal("invalid format string");

	res = PQexec(con, copy_statement);

	if (PQresultStatus(res) != PGRES_COPY_IN)
		pg_fatal("unexpected copy in result: %s", PQerrorMessage(con));
	PQclear(res);

	start = pg_time_now();

	for (k = 0; k < total; k++)
	{
		int64		j = k + 1;

		init_row(&sql, k);
		if (PQputline(con, sql.data))
			pg_fatal("PQputline failed");

		if (CancelRequested)
			break;

		/*
		 * If we want to stick with the original logging, print a message each
		 * 100k inserted rows.
		 */
		if ((!use_quiet) && (j % 100000 == 0))
		{
			double		elapsed_sec = PG_TIME_GET_DOUBLE(pg_time_now() - start);
			double		remaining_sec = ((double) total - j) * elapsed_sec / j;

			chars = fprintf(stderr, INT64_FORMAT " of " INT64_FORMAT " tuples (%d%%) of %s done (elapsed %.2f s, remaining %.2f s)",
							j, total,
							(int) ((j * 100) / total),
							table, elapsed_sec, remaining_sec);

			/*
			 * If the previous progress message is longer than the current
			 * one, add spaces to the current line to fully overwrite any
			 * remaining characters from the previous message.
			 */
			if (prev_chars > chars)
				fprintf(stderr, "%*c", prev_chars - chars, ' ');
			fputc(eol, stderr);
			prev_chars = chars;
		}
		/* let's not call the timing for each row, but only each 100 rows */
		else if (use_quiet && (j % 100 == 0))
		{
			double		elapsed_sec = PG_TIME_GET_DOUBLE(pg_time_now() - start);
			double		remaining_sec = ((double) total - j) * elapsed_sec / j;

			/* have we reached the next interval (or end)? */
			if ((j == total) || (elapsed_sec >= log_interval * LOG_STEP_SECONDS))
			{
				chars = fprintf(stderr, INT64_FORMAT " of " INT64_FORMAT " tuples (%d%%) of %s done (elapsed %.2f s, remaining %.2f s)",
								j, total,
								(int) ((j * 100) / total),
								table, elapsed_sec, remaining_sec);

				/*
				 * If the previous progress message is longer than the current
				 * one, add spaces to the current line to fully overwrite any
				 * remaining characters from the previous message.
				 */
				if (prev_chars > chars)
					fprintf(stderr, "%*c", prev_chars - chars, ' ');
				fputc(eol, stderr);
				prev_chars = chars;

				/* skip to the next interval */
				log_interval = (int) ceil(elapsed_sec / LOG_STEP_SECONDS);
			}
		}
	}

	if (chars != 0 && eol != '\n')
		fprintf(stderr, "%*c\r", chars, ' ');	/* Clear the current line */

	if (PQputline(con, "\\.\n"))
		pg_fatal("very last PQputline failed");
	if (PQendcopy(con))
		pg_fatal("PQendcopy failed");

	termPQExpBuffer(&sql);
}

/*
 * Fill the standard tables with some data generated and sent from the client.
 *
 * The filler column is NULL in pgbench_branches and pgbench_tellers, and is
 * a blank-padded string in pgbench_accounts.
 */
static void
initGenerateDataClientSide(PGconn *con)
{
	fprintf(stderr, "generating data (client-side)...\n");

	/*
	 * we do all of this in one transaction to enable the backend's
	 * data-loading optimizations
	 */
	executeStatement(con, "begin");

	/* truncate away any old data */
	initTruncateTables(con);

	/*
	 * fill branches, tellers, accounts in that order in case foreign keys
	 * already exist
	 */
	initPopulateTable(con, "pgbench_branches", nbranches, initBranch);
	initPopulateTable(con, "pgbench_tellers", ntellers, initTeller);
	initPopulateTable(con, "pgbench_accounts", naccounts, initAccount);

	executeStatement(con, "commit");
}

/*
 * Fill the standard tables with some data generated on the server
 *
 * As already the case with the client-side data generation, the filler
 * column defaults to NULL in pgbench_branches and pgbench_tellers,
 * and is a blank-padded string in pgbench_accounts.
 */
static void
initGenerateDataServerSide(PGconn *con)
{
	PQExpBufferData sql;

	fprintf(stderr, "generating data (server-side)...\n");

	/*
	 * we do all of this in one transaction to enable the backend's
	 * data-loading optimizations
	 */
	executeStatement(con, "begin");

	/* truncate away any old data */
	initTruncateTables(con);

	initPQExpBuffer(&sql);

	printfPQExpBuffer(&sql,
					  "insert into pgbench_branches(bid,bbalance) "
					  "select bid, 0 "
					  "from generate_series(1, %d) as bid", nbranches * scale);
	executeStatement(con, sql.data);

	printfPQExpBuffer(&sql,
					  "insert into pgbench_tellers(tid,bid,tbalance) "
					  "select tid, (tid - 1) / %d + 1, 0 "
					  "from generate_series(1, %d) as tid", ntellers, ntellers * scale);
	executeStatement(con, sql.data);

	printfPQExpBuffer(&sql,
					  "insert into pgbench_accounts(aid,bid,abalance,filler) "
					  "select aid, (aid - 1) / %d + 1, 0, '' "
					  "from generate_series(1, " INT64_FORMAT ") as aid",
					  naccounts, (int64) naccounts * scale);
	executeStatement(con, sql.data);

	termPQExpBuffer(&sql);

	executeStatement(con, "commit");
}

/*
 * Invoke vacuum on the standard tables
 */
static void
initVacuum(PGconn *con)
{
	fprintf(stderr, "vacuuming...\n");
	executeStatement(con, "vacuum analyze pgbench_branches");
	executeStatement(con, "vacuum analyze pgbench_tellers");
	executeStatement(con, "vacuum analyze pgbench_accounts");
	executeStatement(con, "vacuum analyze pgbench_history");
}

/*
 * Create primary keys on the standard tables
 */
static void
initCreatePKeys(PGconn *con)
{
	static const char *const DDLINDEXes[] = {
		"alter table pgbench_branches add primary key (bid)",
		"alter table pgbench_tellers add primary key (tid)",
		"alter table pgbench_accounts add primary key (aid)"
	};
	int			i;
	PQExpBufferData query;

	fprintf(stderr, "creating primary keys...\n");
	initPQExpBuffer(&query);

	for (i = 0; i < lengthof(DDLINDEXes); i++)
	{
		resetPQExpBuffer(&query);
		appendPQExpBufferStr(&query, DDLINDEXes[i]);

		if (index_tablespace != NULL)
		{
			char	   *escape_tablespace;

			escape_tablespace = PQescapeIdentifier(con, index_tablespace,
												   strlen(index_tablespace));
			appendPQExpBuffer(&query, " using index tablespace %s", escape_tablespace);
			PQfreemem(escape_tablespace);
		}

		executeStatement(con, query.data);
	}

	termPQExpBuffer(&query);
}

/*
 * Create foreign key constraints between the standard tables
 */
static void
initCreateFKeys(PGconn *con)
{
	static const char *const DDLKEYs[] = {
		"alter table pgbench_tellers add constraint pgbench_tellers_bid_fkey foreign key (bid) references pgbench_branches",
		"alter table pgbench_accounts add constraint pgbench_accounts_bid_fkey foreign key (bid) references pgbench_branches",
		"alter table pgbench_history add constraint pgbench_history_bid_fkey foreign key (bid) references pgbench_branches",
		"alter table pgbench_history add constraint pgbench_history_tid_fkey foreign key (tid) references pgbench_tellers",
		"alter table pgbench_history add constraint pgbench_history_aid_fkey foreign key (aid) references pgbench_accounts"
	};
	int			i;

	fprintf(stderr, "creating foreign keys...\n");
	for (i = 0; i < lengthof(DDLKEYs); i++)
	{
		executeStatement(con, DDLKEYs[i]);
	}
}

/*
 * Validate an initialization-steps string
 *
 * (We could just leave it to runInitSteps() to fail if there are wrong
 * characters, but since initialization can take awhile, it seems friendlier
 * to check during option parsing.)
 */
static void
checkInitSteps(const char *initialize_steps)
{
	if (initialize_steps[0] == '\0')
		pg_fatal("no initialization steps specified");

	for (const char *step = initialize_steps; *step != '\0'; step++)
	{
		if (strchr(ALL_INIT_STEPS " ", *step) == NULL)
		{
			pg_log_error("unrecognized initialization step \"%c\"", *step);
			pg_log_error_detail("Allowed step characters are: \"" ALL_INIT_STEPS "\".");
			exit(1);
		}
	}
}

/*
 * Invoke each initialization step in the given string
 */
static void
runInitSteps(const char *initialize_steps)
{
	PQExpBufferData stats;
	PGconn	   *con;
	const char *step;
	double		run_time = 0.0;
	bool		first = true;

	initPQExpBuffer(&stats);

	if ((con = doConnect()) == NULL)
		pg_fatal("could not create connection for initialization");

	setup_cancel_handler(NULL);
	SetCancelConn(con);

	for (step = initialize_steps; *step != '\0'; step++)
	{
		char	   *op = NULL;
		pg_time_usec_t start = pg_time_now();

		switch (*step)
		{
			case 'd':
				op = "drop tables";
				initDropTables(con);
				break;
			case 't':
				op = "create tables";
				initCreateTables(con);
				break;
			case 'g':
				op = "client-side generate";
				initGenerateDataClientSide(con);
				break;
			case 'G':
				op = "server-side generate";
				initGenerateDataServerSide(con);
				break;
			case 'v':
				op = "vacuum";
				initVacuum(con);
				break;
			case 'p':
				op = "primary keys";
				initCreatePKeys(con);
				break;
			case 'f':
				op = "foreign keys";
				initCreateFKeys(con);
				break;
			case ' ':
				break;			/* ignore */
			default:
				pg_log_error("unrecognized initialization step \"%c\"", *step);
				PQfinish(con);
				exit(1);
		}

		if (op != NULL)
		{
			double		elapsed_sec = PG_TIME_GET_DOUBLE(pg_time_now() - start);

			if (!first)
				appendPQExpBufferStr(&stats, ", ");
			else
				first = false;

			appendPQExpBuffer(&stats, "%s %.2f s", op, elapsed_sec);

			run_time += elapsed_sec;
		}
	}

	fprintf(stderr, "done in %.2f s (%s).\n", run_time, stats.data);
	ResetCancelConn();
	PQfinish(con);
	termPQExpBuffer(&stats);
}

/*
 * Extract pgbench table information into global variables scale,
 * partition_method and partitions.
 */
static void
GetTableInfo(PGconn *con, bool scale_given)
{
	PGresult   *res;

	/*
	 * get the scaling factor that should be same as count(*) from
	 * pgbench_branches if this is not a custom query
	 */
	res = PQexec(con, "select count(*) from pgbench_branches");
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		char	   *sqlState = PQresultErrorField(res, PG_DIAG_SQLSTATE);

		pg_log_error("could not count number of branches: %s", PQerrorMessage(con));

		if (sqlState && strcmp(sqlState, ERRCODE_UNDEFINED_TABLE) == 0)
			pg_log_error_hint("Perhaps you need to do initialization (\"pgbench -i\") in database \"%s\".",
							  PQdb(con));

		exit(1);
	}
	scale = atoi(PQgetvalue(res, 0, 0));
	if (scale < 0)
		pg_fatal("invalid count(*) from pgbench_branches: \"%s\"",
				 PQgetvalue(res, 0, 0));
	PQclear(res);

	/* warn if we override user-given -s switch */
	if (scale_given)
		pg_log_warning("scale option ignored, using count from pgbench_branches table (%d)",
					   scale);

	/*
	 * Get the partition information for the first "pgbench_accounts" table
	 * found in search_path.
	 *
	 * The result is empty if no "pgbench_accounts" is found.
	 *
	 * Otherwise, it always returns one row even if the table is not
	 * partitioned (in which case the partition strategy is NULL).
	 *
	 * The number of partitions can be 0 even for partitioned tables, if no
	 * partition is attached.
	 *
	 * We assume no partitioning on any failure, so as to avoid failing on an
	 * old version without "pg_partitioned_table".
	 */
	res = PQexec(con,
				 "select o.n, p.partstrat, pg_catalog.count(i.inhparent) "
				 "from pg_catalog.pg_class as c "
				 "join pg_catalog.pg_namespace as n on (n.oid = c.relnamespace) "
				 "cross join lateral (select pg_catalog.array_position(pg_catalog.current_schemas(true), n.nspname)) as o(n) "
				 "left join pg_catalog.pg_partitioned_table as p on (p.partrelid = c.oid) "
				 "left join pg_catalog.pg_inherits as i on (c.oid = i.inhparent) "
				 "where c.relname = 'pgbench_accounts' and o.n is not null "
				 "group by 1, 2 "
				 "order by 1 asc "
				 "limit 1");

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		/* probably an older version, coldly assume no partitioning */
		partition_method = PART_NONE;
		partitions = 0;
	}
	else if (PQntuples(res) == 0)
	{
		/*
		 * This case is unlikely as pgbench already found "pgbench_branches"
		 * above to compute the scale.
		 */
		pg_log_error("no pgbench_accounts table found in \"search_path\"");
		pg_log_error_hint("Perhaps you need to do initialization (\"pgbench -i\") in database \"%s\".", PQdb(con));
		exit(1);
	}
	else						/* PQntuples(res) == 1 */
	{
		/* normal case, extract partition information */
		if (PQgetisnull(res, 0, 1))
			partition_method = PART_NONE;
		else
		{
			char	   *ps = PQgetvalue(res, 0, 1);

			/* column must be there */
			Assert(ps != NULL);

			if (strcmp(ps, "r") == 0)
				partition_method = PART_RANGE;
			else if (strcmp(ps, "h") == 0)
				partition_method = PART_HASH;
			else
			{
				/* possibly a newer version with new partition method */
				pg_fatal("unexpected partition method: \"%s\"", ps);
			}
		}

		partitions = atoi(PQgetvalue(res, 0, 2));
	}

	PQclear(res);
}

/*
 * Replace :param with $n throughout the command's SQL text, which
 * is a modifiable string in cmd->lines.
 */
static bool
parseQuery(Command *cmd)
{
	char	   *sql,
			   *p;

	cmd->argc = 1;

	p = sql = pg_strdup(cmd->lines.data);
	while ((p = strchr(p, ':')) != NULL)
	{
		char		var[13];
		char	   *name;
		int			eaten;

		name = parseVariable(p, &eaten);
		if (name == NULL)
		{
			while (*p == ':')
			{
				p++;
			}
			continue;
		}

		/*
		 * cmd->argv[0] is the SQL statement itself, so the max number of
		 * arguments is one less than MAX_ARGS
		 */
		if (cmd->argc >= MAX_ARGS)
		{
			pg_log_error("statement has too many arguments (maximum is %d): %s",
						 MAX_ARGS - 1, cmd->lines.data);
			pg_free(name);
			return false;
		}

		sprintf(var, "$%d", cmd->argc);
		p = replaceVariable(&sql, p, eaten, var);

		cmd->argv[cmd->argc] = name;
		cmd->argc++;
	}

	Assert(cmd->argv[0] == NULL);
	cmd->argv[0] = sql;
	return true;
}

/*
 * syntax error while parsing a script (in practice, while parsing a
 * backslash command, because we don't detect syntax errors in SQL)
 *
 * source: source of script (filename or builtin-script ID)
 * lineno: line number within script (count from 1)
 * line: whole line of backslash command, if available
 * command: backslash command name, if available
 * msg: the actual error message
 * more: optional extra message
 * column: zero-based column number, or -1 if unknown
 */
void
syntax_error(const char *source, int lineno,
			 const char *line, const char *command,
			 const char *msg, const char *more, int column)
{
	PQExpBufferData buf;

	initPQExpBuffer(&buf);

	printfPQExpBuffer(&buf, "%s:%d: %s", source, lineno, msg);
	if (more != NULL)
		appendPQExpBuffer(&buf, " (%s)", more);
	if (column >= 0 && line == NULL)
		appendPQExpBuffer(&buf, " at column %d", column + 1);
	if (command != NULL)
		appendPQExpBuffer(&buf, " in command \"%s\"", command);

	pg_log_error("%s", buf.data);

	termPQExpBuffer(&buf);

	if (line != NULL)
	{
		fprintf(stderr, "%s\n", line);
		if (column >= 0)
			fprintf(stderr, "%*c error found here\n", column + 1, '^');
	}

	exit(1);
}

/*
 * Return a pointer to the start of the SQL command, after skipping over
 * whitespace and "--" comments.
 * If the end of the string is reached, return NULL.
 */
static char *
skip_sql_comments(char *sql_command)
{
	char	   *p = sql_command;

	/* Skip any leading whitespace, as well as "--" style comments */
	for (;;)
	{
		if (isspace((unsigned char) *p))
			p++;
		else if (strncmp(p, "--", 2) == 0)
		{
			p = strchr(p, '\n');
			if (p == NULL)
				return NULL;
			p++;
		}
		else
			break;
	}

	/* NULL if there's nothing but whitespace and comments */
	if (*p == '\0')
		return NULL;

	return p;
}

/*
 * Parse a SQL command; return a Command struct, or NULL if it's a comment
 *
 * On entry, psqlscan.l has collected the command into "buf", so we don't
 * really need to do much here except check for comments and set up a Command
 * struct.
 */
static Command *
create_sql_command(PQExpBuffer buf, const char *source)
{
	Command    *my_command;
	char	   *p = skip_sql_comments(buf->data);

	if (p == NULL)
		return NULL;

	/* Allocate and initialize Command structure */
	my_command = (Command *) pg_malloc(sizeof(Command));
	initPQExpBuffer(&my_command->lines);
	appendPQExpBufferStr(&my_command->lines, p);
	my_command->first_line = NULL;	/* this is set later */
	my_command->type = SQL_COMMAND;
	my_command->meta = META_NONE;
	my_command->argc = 0;
	my_command->retries = 0;
	my_command->failures = 0;
	memset(my_command->argv, 0, sizeof(my_command->argv));
	my_command->varprefix = NULL;	/* allocated later, if needed */
	my_command->expr = NULL;
	initSimpleStats(&my_command->stats);
	my_command->prepname = NULL;	/* set later, if needed */

	return my_command;
}

/* Free a Command structure and associated data */
static void
free_command(Command *command)
{
	termPQExpBuffer(&command->lines);
	pg_free(command->first_line);
	for (int i = 0; i < command->argc; i++)
		pg_free(command->argv[i]);
	pg_free(command->varprefix);

	/*
	 * It should also free expr recursively, but this is currently not needed
	 * as only gset commands (which do not have an expression) are freed.
	 */
	pg_free(command);
}

/*
 * Once an SQL command is fully parsed, possibly by accumulating several
 * parts, complete other fields of the Command structure.
 */
static void
postprocess_sql_command(Command *my_command)
{
	char		buffer[128];
	static int	prepnum = 0;

	Assert(my_command->type == SQL_COMMAND);

	/* Save the first line for error display. */
	strlcpy(buffer, my_command->lines.data, sizeof(buffer));
	buffer[strcspn(buffer, "\n\r")] = '\0';
	my_command->first_line = pg_strdup(buffer);

	/* Parse query and generate prepared statement name, if necessary */
	switch (querymode)
	{
		case QUERY_SIMPLE:
			my_command->argv[0] = my_command->lines.data;
			my_command->argc++;
			break;
		case QUERY_PREPARED:
			my_command->prepname = psprintf("P_%d", prepnum++);
			/* fall through */
		case QUERY_EXTENDED:
			if (!parseQuery(my_command))
				exit(1);
			break;
		default:
			exit(1);
	}
}

/*
 * Parse a backslash command; return a Command struct, or NULL if comment
 *
 * At call, we have scanned only the initial backslash.
 */
static Command *
process_backslash_command(PsqlScanState sstate, const char *source,
						  int lineno, int start_offset)
{
	Command    *my_command;
	PQExpBufferData word_buf;
	int			word_offset;
	int			offsets[MAX_ARGS];	/* offsets of argument words */
	int			j;

	initPQExpBuffer(&word_buf);

	/* Collect first word of command */
	if (!expr_lex_one_word(sstate, &word_buf, &word_offset))
	{
		termPQExpBuffer(&word_buf);
		return NULL;
	}

	/* Allocate and initialize Command structure */
	my_command = (Command *) pg_malloc0(sizeof(Command));
	my_command->type = META_COMMAND;
	my_command->argc = 0;
	initSimpleStats(&my_command->stats);

	/* Save first word (command name) */
	j = 0;
	offsets[j] = word_offset;
	my_command->argv[j++] = pg_strdup(word_buf.data);
	my_command->argc++;

	/* ... and convert it to enum form */
	my_command->meta = getMetaCommand(my_command->argv[0]);

	if (my_command->meta == META_SET ||
		my_command->meta == META_IF ||
		my_command->meta == META_ELIF)
	{
		yyscan_t	yyscanner;

		/* For \set, collect var name */
		if (my_command->meta == META_SET)
		{
			if (!expr_lex_one_word(sstate, &word_buf, &word_offset))
				syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
							 "missing argument", NULL, -1);

			offsets[j] = word_offset;
			my_command->argv[j++] = pg_strdup(word_buf.data);
			my_command->argc++;
		}

		/* then for all parse the expression */
		yyscanner = expr_scanner_init(sstate, source, lineno, start_offset,
									  my_command->argv[0]);

		if (expr_yyparse(&my_command->expr, yyscanner) != 0)
		{
			/* dead code: exit done from syntax_error called by yyerror */
			exit(1);
		}

		/* Save line, trimming any trailing newline */
		my_command->first_line =
			expr_scanner_get_substring(sstate,
									   start_offset,
									   true);

		expr_scanner_finish(yyscanner);

		termPQExpBuffer(&word_buf);

		return my_command;
	}

	/* For all other commands, collect remaining words. */
	while (expr_lex_one_word(sstate, &word_buf, &word_offset))
	{
		/*
		 * my_command->argv[0] is the command itself, so the max number of
		 * arguments is one less than MAX_ARGS
		 */
		if (j >= MAX_ARGS)
			syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
						 "too many arguments", NULL, -1);

		offsets[j] = word_offset;
		my_command->argv[j++] = pg_strdup(word_buf.data);
		my_command->argc++;
	}

	/* Save line, trimming any trailing newline */
	my_command->first_line =
		expr_scanner_get_substring(sstate,
								   start_offset,
								   true);

	if (my_command->meta == META_SLEEP)
	{
		if (my_command->argc < 2)
			syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
						 "missing argument", NULL, -1);

		if (my_command->argc > 3)
			syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
						 "too many arguments", NULL,
						 offsets[3] - start_offset);

		/*
		 * Split argument into number and unit to allow "sleep 1ms" etc. We
		 * don't have to terminate the number argument with null because it
		 * will be parsed with atoi, which ignores trailing non-digit
		 * characters.
		 */
		if (my_command->argv[1][0] != ':')
		{
			char	   *c = my_command->argv[1];
			bool		have_digit = false;

			/* Skip sign */
			if (*c == '+' || *c == '-')
				c++;

			/* Require at least one digit */
			if (*c && isdigit((unsigned char) *c))
				have_digit = true;

			/* Eat all digits */
			while (*c && isdigit((unsigned char) *c))
				c++;

			if (*c)
			{
				if (my_command->argc == 2 && have_digit)
				{
					my_command->argv[2] = c;
					offsets[2] = offsets[1] + (c - my_command->argv[1]);
					my_command->argc = 3;
				}
				else
				{
					/*
					 * Raise an error if argument starts with non-digit
					 * character (after sign).
					 */
					syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
								 "invalid sleep time, must be an integer",
								 my_command->argv[1], offsets[1] - start_offset);
				}
			}
		}

		if (my_command->argc == 3)
		{
			if (pg_strcasecmp(my_command->argv[2], "us") != 0 &&
				pg_strcasecmp(my_command->argv[2], "ms") != 0 &&
				pg_strcasecmp(my_command->argv[2], "s") != 0)
				syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
							 "unrecognized time unit, must be us, ms or s",
							 my_command->argv[2], offsets[2] - start_offset);
		}
	}
	else if (my_command->meta == META_SETSHELL)
	{
		if (my_command->argc < 3)
			syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
						 "missing argument", NULL, -1);
	}
	else if (my_command->meta == META_SHELL)
	{
		if (my_command->argc < 2)
			syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
						 "missing command", NULL, -1);
	}
	else if (my_command->meta == META_ELSE || my_command->meta == META_ENDIF ||
			 my_command->meta == META_STARTPIPELINE ||
			 my_command->meta == META_ENDPIPELINE ||
			 my_command->meta == META_SYNCPIPELINE)
	{
		if (my_command->argc != 1)
			syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
						 "unexpected argument", NULL, -1);
	}
	else if (my_command->meta == META_GSET || my_command->meta == META_ASET)
	{
		if (my_command->argc > 2)
			syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
						 "too many arguments", NULL, -1);
	}
	else
	{
		/* my_command->meta == META_NONE */
		syntax_error(source, lineno, my_command->first_line, my_command->argv[0],
					 "invalid command", NULL, -1);
	}

	termPQExpBuffer(&word_buf);

	return my_command;
}

static void
ConditionError(const char *desc, int cmdn, const char *msg)
{
	pg_fatal("condition error in script \"%s\" command %d: %s",
			 desc, cmdn, msg);
}

/*
 * Partial evaluation of conditionals before recording and running the script.
 */
static void
CheckConditional(const ParsedScript *ps)
{
	/* statically check conditional structure */
	ConditionalStack cs = conditional_stack_create();
	int			i;

	for (i = 0; ps->commands[i] != NULL; i++)
	{
		Command    *cmd = ps->commands[i];

		if (cmd->type == META_COMMAND)
		{
			switch (cmd->meta)
			{
				case META_IF:
					conditional_stack_push(cs, IFSTATE_FALSE);
					break;
				case META_ELIF:
					if (conditional_stack_empty(cs))
						ConditionError(ps->desc, i + 1, "\\elif without matching \\if");
					if (conditional_stack_peek(cs) == IFSTATE_ELSE_FALSE)
						ConditionError(ps->desc, i + 1, "\\elif after \\else");
					break;
				case META_ELSE:
					if (conditional_stack_empty(cs))
						ConditionError(ps->desc, i + 1, "\\else without matching \\if");
					if (conditional_stack_peek(cs) == IFSTATE_ELSE_FALSE)
						ConditionError(ps->desc, i + 1, "\\else after \\else");
					conditional_stack_poke(cs, IFSTATE_ELSE_FALSE);
					break;
				case META_ENDIF:
					if (!conditional_stack_pop(cs))
						ConditionError(ps->desc, i + 1, "\\endif without matching \\if");
					break;
				default:
					/* ignore anything else... */
					break;
			}
		}
	}
	if (!conditional_stack_empty(cs))
		ConditionError(ps->desc, i + 1, "\\if without matching \\endif");
	conditional_stack_destroy(cs);
}

/*
 * Parse a script (either the contents of a file, or a built-in script)
 * and add it to the list of scripts.
 */
static void
ParseScript(const char *script, const char *desc, int weight)
{
	ParsedScript ps;
	PsqlScanState sstate;
	PQExpBufferData line_buf;
	int			alloc_num;
	int			index;

#define COMMANDS_ALLOC_NUM 128
	alloc_num = COMMANDS_ALLOC_NUM;

	/* Initialize all fields of ps */
	ps.desc = desc;
	ps.weight = weight;
	ps.commands = (Command **) pg_malloc(sizeof(Command *) * alloc_num);
	initStats(&ps.stats, 0);

	/* Prepare to parse script */
	sstate = psql_scan_create(&pgbench_callbacks);

	/*
	 * Ideally, we'd scan scripts using the encoding and stdstrings settings
	 * we get from a DB connection.  However, without major rearrangement of
	 * pgbench's argument parsing, we can't have a DB connection at the time
	 * we parse scripts.  Using SQL_ASCII (encoding 0) should work well enough
	 * with any backend-safe encoding, though conceivably we could be fooled
	 * if a script file uses a client-only encoding.  We also assume that
	 * stdstrings should be true, which is a bit riskier.
	 */
	psql_scan_setup(sstate, script, strlen(script), 0, true);

	initPQExpBuffer(&line_buf);

	index = 0;

	for (;;)
	{
		PsqlScanResult sr;
		promptStatus_t prompt;
		Command    *command = NULL;

		resetPQExpBuffer(&line_buf);

		sr = psql_scan(sstate, &line_buf, &prompt);

		/* If we collected a new SQL command, process that */
		command = create_sql_command(&line_buf, desc);

		/* store new command */
		if (command)
			ps.commands[index++] = command;

		/* If we reached a backslash, process that */
		if (sr == PSCAN_BACKSLASH)
		{
			int			lineno;
			int			start_offset;

			/* Capture location of the backslash */
			psql_scan_get_location(sstate, &lineno, &start_offset);
			start_offset--;

			command = process_backslash_command(sstate, desc,
												lineno, start_offset);

			if (command)
			{
				/*
				 * If this is gset or aset, merge into the preceding command.
				 * (We don't use a command slot in this case).
				 */
				if (command->meta == META_GSET || command->meta == META_ASET)
				{
					Command    *cmd;

					if (index == 0)
						syntax_error(desc, lineno, NULL, NULL,
									 "\\gset must follow an SQL command",
									 NULL, -1);

					cmd = ps.commands[index - 1];

					if (cmd->type != SQL_COMMAND ||
						cmd->varprefix != NULL)
						syntax_error(desc, lineno, NULL, NULL,
									 "\\gset must follow an SQL command",
									 cmd->first_line, -1);

					/* get variable prefix */
					if (command->argc <= 1 || command->argv[1][0] == '\0')
						cmd->varprefix = pg_strdup("");
					else
						cmd->varprefix = pg_strdup(command->argv[1]);

					/* update the sql command meta */
					cmd->meta = command->meta;

					/* cleanup unused command */
					free_command(command);

					continue;
				}

				/* Attach any other backslash command as a new command */
				ps.commands[index++] = command;
			}
		}

		/*
		 * Since we used a command slot, allocate more if needed.  Note we
		 * always allocate one more in order to accommodate the NULL
		 * terminator below.
		 */
		if (index >= alloc_num)
		{
			alloc_num += COMMANDS_ALLOC_NUM;
			ps.commands = (Command **)
				pg_realloc(ps.commands, sizeof(Command *) * alloc_num);
		}

		/* Done if we reached EOF */
		if (sr == PSCAN_INCOMPLETE || sr == PSCAN_EOL)
			break;
	}

	ps.commands[index] = NULL;

	addScript(&ps);

	termPQExpBuffer(&line_buf);
	psql_scan_finish(sstate);
	psql_scan_destroy(sstate);
}

/*
 * Read the entire contents of file fd, and return it in a malloc'd buffer.
 *
 * The buffer will typically be larger than necessary, but we don't care
 * in this program, because we'll free it as soon as we've parsed the script.
 */
static char *
read_file_contents(FILE *fd)
{
	char	   *buf;
	size_t		buflen = BUFSIZ;
	size_t		used = 0;

	buf = (char *) pg_malloc(buflen);

	for (;;)
	{
		size_t		nread;

		nread = fread(buf + used, 1, BUFSIZ, fd);
		used += nread;
		/* If fread() read less than requested, must be EOF or error */
		if (nread < BUFSIZ)
			break;
		/* Enlarge buf so we can read some more */
		buflen += BUFSIZ;
		buf = (char *) pg_realloc(buf, buflen);
	}
	/* There is surely room for a terminator */
	buf[used] = '\0';

	return buf;
}

/*
 * Given a file name, read it and add its script to the list.
 * "-" means to read stdin.
 * NB: filename must be storage that won't disappear.
 */
static void
process_file(const char *filename, int weight)
{
	FILE	   *fd;
	char	   *buf;

	/* Slurp the file contents into "buf" */
	if (strcmp(filename, "-") == 0)
		fd = stdin;
	else if ((fd = fopen(filename, "r")) == NULL)
		pg_fatal("could not open file \"%s\": %m", filename);

	buf = read_file_contents(fd);

	if (ferror(fd))
		pg_fatal("could not read file \"%s\": %m", filename);

	if (fd != stdin)
		fclose(fd);

	ParseScript(buf, filename, weight);

	free(buf);
}

/* Parse the given builtin script and add it to the list. */
static void
process_builtin(const BuiltinScript *bi, int weight)
{
	ParseScript(bi->script, bi->desc, weight);
}

/* show available builtin scripts */
static void
listAvailableScripts(void)
{
	int			i;

	fprintf(stderr, "Available builtin scripts:\n");
	for (i = 0; i < lengthof(builtin_script); i++)
		fprintf(stderr, "  %13s: %s\n", builtin_script[i].name, builtin_script[i].desc);
	fprintf(stderr, "\n");
}

/* return builtin script "name" if unambiguous, fails if not found */
static const BuiltinScript *
findBuiltin(const char *name)
{
	int			i,
				found = 0,
				len = strlen(name);
	const BuiltinScript *result = NULL;

	for (i = 0; i < lengthof(builtin_script); i++)
	{
		if (strncmp(builtin_script[i].name, name, len) == 0)
		{
			result = &builtin_script[i];
			found++;
		}
	}

	/* ok, unambiguous result */
	if (found == 1)
		return result;

	/* error cases */
	if (found == 0)
		pg_log_error("no builtin script found for name \"%s\"", name);
	else						/* found > 1 */
		pg_log_error("ambiguous builtin name: %d builtin scripts found for prefix \"%s\"", found, name);

	listAvailableScripts();
	exit(1);
}

/*
 * Determine the weight specification from a script option (-b, -f), if any,
 * and return it as an integer (1 is returned if there's no weight).  The
 * script name is returned in *script as a malloc'd string.
 */
static int
parseScriptWeight(const char *option, char **script)
{
	char	   *sep;
	int			weight;

	if ((sep = strrchr(option, WSEP)))
	{
		int			namelen = sep - option;
		long		wtmp;
		char	   *badp;

		/* generate the script name */
		*script = pg_malloc(namelen + 1);
		strncpy(*script, option, namelen);
		(*script)[namelen] = '\0';

		/* process digits of the weight spec */
		errno = 0;
		wtmp = strtol(sep + 1, &badp, 10);
		if (errno != 0 || badp == sep + 1 || *badp != '\0')
			pg_fatal("invalid weight specification: %s", sep);
		if (wtmp > INT_MAX || wtmp < 0)
			pg_fatal("weight specification out of range (0 .. %d): %lld",
					 INT_MAX, (long long) wtmp);
		weight = wtmp;
	}
	else
	{
		*script = pg_strdup(option);
		weight = 1;
	}

	return weight;
}

/* append a script to the list of scripts to process */
static void
addScript(const ParsedScript *script)
{
	if (script->commands == NULL || script->commands[0] == NULL)
		pg_fatal("empty command list for script \"%s\"", script->desc);

	if (num_scripts >= MAX_SCRIPTS)
		pg_fatal("at most %d SQL scripts are allowed", MAX_SCRIPTS);

	CheckConditional(script);

	sql_script[num_scripts] = *script;
	num_scripts++;
}

/*
 * Print progress report.
 *
 * On entry, *last and *last_report contain the statistics and time of last
 * progress report.  On exit, they are updated with the new stats.
 */
static void
printProgressReport(TState *threads, int64 test_start, pg_time_usec_t now,
					StatsData *last, int64 *last_report)
{
	/* generate and show report */
	pg_time_usec_t run = now - *last_report;
	int64		cnt,
				failures,
				retried;
	double		tps,
				total_run,
				latency,
				sqlat,
				lag,
				stdev;
	char		tbuf[315];
	StatsData	cur;

	/*
	 * Add up the statistics of all threads.
	 *
	 * XXX: No locking.  There is no guarantee that we get an atomic snapshot
	 * of the transaction count and latencies, so these figures can well be
	 * off by a small amount.  The progress report's purpose is to give a
	 * quick overview of how the test is going, so that shouldn't matter too
	 * much.  (If a read from a 64-bit integer is not atomic, you might get a
	 * "torn" read and completely bogus latencies though!)
	 */
	initStats(&cur, 0);
	for (int i = 0; i < nthreads; i++)
	{
		mergeSimpleStats(&cur.latency, &threads[i].stats.latency);
		mergeSimpleStats(&cur.lag, &threads[i].stats.lag);
		cur.cnt += threads[i].stats.cnt;
		cur.skipped += threads[i].stats.skipped;
		cur.retries += threads[i].stats.retries;
		cur.retried += threads[i].stats.retried;
		cur.serialization_failures +=
			threads[i].stats.serialization_failures;
		cur.deadlock_failures += threads[i].stats.deadlock_failures;
	}

	/* we count only actually executed transactions */
	cnt = cur.cnt - last->cnt;
	total_run = (now - test_start) / 1000000.0;
	tps = 1000000.0 * cnt / run;
	if (cnt > 0)
	{
		latency = 0.001 * (cur.latency.sum - last->latency.sum) / cnt;
		sqlat = 1.0 * (cur.latency.sum2 - last->latency.sum2) / cnt;
		stdev = 0.001 * sqrt(sqlat - 1000000.0 * latency * latency);
		lag = 0.001 * (cur.lag.sum - last->lag.sum) / cnt;
	}
	else
	{
		latency = sqlat = stdev = lag = 0;
	}
	failures = getFailures(&cur) - getFailures(last);
	retried = cur.retried - last->retried;

	if (progress_timestamp)
	{
		snprintf(tbuf, sizeof(tbuf), "%.3f s",
				 PG_TIME_GET_DOUBLE(now + epoch_shift));
	}
	else
	{
		/* round seconds are expected, but the thread may be late */
		snprintf(tbuf, sizeof(tbuf), "%.1f s", total_run);
	}

	fprintf(stderr,
			"progress: %s, %.1f tps, lat %.3f ms stddev %.3f, " INT64_FORMAT " failed",
			tbuf, tps, latency, stdev, failures);

	if (throttle_delay)
	{
		fprintf(stderr, ", lag %.3f ms", lag);
		if (latency_limit)
			fprintf(stderr, ", " INT64_FORMAT " skipped",
					cur.skipped - last->skipped);
	}

	/* it can be non-zero only if max_tries is not equal to one */
	if (max_tries != 1)
		fprintf(stderr,
				", " INT64_FORMAT " retried, " INT64_FORMAT " retries",
				retried, cur.retries - last->retries);
	fprintf(stderr, "\n");

	*last = cur;
	*last_report = now;
}

static void
printSimpleStats(const char *prefix, SimpleStats *ss)
{
	if (ss->count > 0)
	{
		double		latency = ss->sum / ss->count;
		double		stddev = sqrt(ss->sum2 / ss->count - latency * latency);

		printf("%s average = %.3f ms\n", prefix, 0.001 * latency);
		printf("%s stddev = %.3f ms\n", prefix, 0.001 * stddev);
	}
}

/* print version banner */
static void
printVersion(PGconn *con)
{
	int			server_ver = PQserverVersion(con);
	int			client_ver = PG_VERSION_NUM;

	if (server_ver != client_ver)
	{
		const char *server_version;
		char		sverbuf[32];

		/* Try to get full text form, might include "devel" etc */
		server_version = PQparameterStatus(con, "server_version");
		/* Otherwise fall back on server_ver */
		if (!server_version)
		{
			formatPGVersionNumber(server_ver, true,
								  sverbuf, sizeof(sverbuf));
			server_version = sverbuf;
		}

		printf(_("%s (%s, server %s)\n"),
			   "pgbench", PG_VERSION, server_version);
	}
	/* For version match, only print pgbench version */
	else
		printf("%s (%s)\n", "pgbench", PG_VERSION);
	fflush(stdout);
}

/* print out results */
static void
printResults(StatsData *total,
			 pg_time_usec_t total_duration, /* benchmarking time */
			 pg_time_usec_t conn_total_duration,	/* is_connect */
			 pg_time_usec_t conn_elapsed_duration,	/* !is_connect */
			 int64 latency_late)
{
	/* tps is about actually executed transactions during benchmarking */
	int64		failures = getFailures(total);
	int64		total_cnt = total->cnt + total->skipped + failures;
	double		bench_duration = PG_TIME_GET_DOUBLE(total_duration);
	double		tps = total->cnt / bench_duration;

	/* Report test parameters. */
	printf("transaction type: %s\n",
		   num_scripts == 1 ? sql_script[0].desc : "multiple scripts");
	printf("scaling factor: %d\n", scale);
	/* only print partitioning information if some partitioning was detected */
	if (partition_method != PART_NONE)
		printf("partition method: %s\npartitions: %d\n",
			   PARTITION_METHOD[partition_method], partitions);
	printf("query mode: %s\n", QUERYMODE[querymode]);
	printf("number of clients: %d\n", nclients);
	printf("number of threads: %d\n", nthreads);

	if (max_tries)
		printf("maximum number of tries: %u\n", max_tries);

	if (duration <= 0)
	{
		printf("number of transactions per client: %d\n", nxacts);
		printf("number of transactions actually processed: " INT64_FORMAT "/%d\n",
			   total->cnt, nxacts * nclients);
	}
	else
	{
		printf("duration: %d s\n", duration);
		printf("number of transactions actually processed: " INT64_FORMAT "\n",
			   total->cnt);
	}

	/*
	 * Remaining stats are nonsensical if we failed to execute any xacts due
	 * to others than serialization or deadlock errors
	 */
	if (total_cnt <= 0)
		return;

	printf("number of failed transactions: " INT64_FORMAT " (%.3f%%)\n",
		   failures, 100.0 * failures / total_cnt);

	if (failures_detailed)
	{
		printf("number of serialization failures: " INT64_FORMAT " (%.3f%%)\n",
			   total->serialization_failures,
			   100.0 * total->serialization_failures / total_cnt);
		printf("number of deadlock failures: " INT64_FORMAT " (%.3f%%)\n",
			   total->deadlock_failures,
			   100.0 * total->deadlock_failures / total_cnt);
	}

	/* it can be non-zero only if max_tries is not equal to one */
	if (max_tries != 1)
	{
		printf("number of transactions retried: " INT64_FORMAT " (%.3f%%)\n",
			   total->retried, 100.0 * total->retried / total_cnt);
		printf("total number of retries: " INT64_FORMAT "\n", total->retries);
	}

	if (throttle_delay && latency_limit)
		printf("number of transactions skipped: " INT64_FORMAT " (%.3f%%)\n",
			   total->skipped, 100.0 * total->skipped / total_cnt);

	if (latency_limit)
		printf("number of transactions above the %.1f ms latency limit: " INT64_FORMAT "/" INT64_FORMAT " (%.3f%%)\n",
			   latency_limit / 1000.0, latency_late, total->cnt,
			   (total->cnt > 0) ? 100.0 * latency_late / total->cnt : 0.0);

	if (throttle_delay || progress || latency_limit)
		printSimpleStats("latency", &total->latency);
	else
	{
		/* no measurement, show average latency computed from run time */
		printf("latency average = %.3f ms%s\n",
			   0.001 * total_duration * nclients / total_cnt,
			   failures > 0 ? " (including failures)" : "");
	}

	if (throttle_delay)
	{
		/*
		 * Report average transaction lag under rate limit throttling.  This
		 * is the delay between scheduled and actual start times for the
		 * transaction.  The measured lag may be caused by thread/client load,
		 * the database load, or the Poisson throttling process.
		 */
		printf("rate limit schedule lag: avg %.3f (max %.3f) ms\n",
			   0.001 * total->lag.sum / total->cnt, 0.001 * total->lag.max);
	}

	/*
	 * Under -C/--connect, each transaction incurs a significant connection
	 * cost, it would not make much sense to ignore it in tps, and it would
	 * not be tps anyway.
	 *
	 * Otherwise connections are made just once at the beginning of the run
	 * and should not impact performance but for very short run, so they are
	 * (right)fully ignored in tps.
	 */
	if (is_connect)
	{
		printf("average connection time = %.3f ms\n", 0.001 * conn_total_duration / (total->cnt + failures));
		printf("tps = %f (including reconnection times)\n", tps);
	}
	else
	{
		printf("initial connection time = %.3f ms\n", 0.001 * conn_elapsed_duration);
		printf("tps = %f (without initial connection time)\n", tps);
	}

	/* Report per-script/command statistics */
	if (per_script_stats || report_per_command)
	{
		int			i;

		for (i = 0; i < num_scripts; i++)
		{
			if (per_script_stats)
			{
				StatsData  *sstats = &sql_script[i].stats;
				int64		script_failures = getFailures(sstats);
				int64		script_total_cnt =
					sstats->cnt + sstats->skipped + script_failures;

				printf("SQL script %d: %s\n"
					   " - weight: %d (targets %.1f%% of total)\n"
					   " - " INT64_FORMAT " transactions (%.1f%% of total)\n",
					   i + 1, sql_script[i].desc,
					   sql_script[i].weight,
					   100.0 * sql_script[i].weight / total_weight,
					   script_total_cnt,
					   100.0 * script_total_cnt / total_cnt);

				if (script_total_cnt > 0)
				{
					printf(" - number of transactions actually processed: " INT64_FORMAT " (tps = %f)\n",
						   sstats->cnt, sstats->cnt / bench_duration);

					printf(" - number of failed transactions: " INT64_FORMAT " (%.3f%%)\n",
						   script_failures,
						   100.0 * script_failures / script_total_cnt);

					if (failures_detailed)
					{
						printf(" - number of serialization failures: " INT64_FORMAT " (%.3f%%)\n",
							   sstats->serialization_failures,
							   (100.0 * sstats->serialization_failures /
								script_total_cnt));
						printf(" - number of deadlock failures: " INT64_FORMAT " (%.3f%%)\n",
							   sstats->deadlock_failures,
							   (100.0 * sstats->deadlock_failures /
								script_total_cnt));
					}

					/*
					 * it can be non-zero only if max_tries is not equal to
					 * one
					 */
					if (max_tries != 1)
					{
						printf(" - number of transactions retried: " INT64_FORMAT " (%.3f%%)\n",
							   sstats->retried,
							   100.0 * sstats->retried / script_total_cnt);
						printf(" - total number of retries: " INT64_FORMAT "\n",
							   sstats->retries);
					}

					if (throttle_delay && latency_limit)
						printf(" - number of transactions skipped: " INT64_FORMAT " (%.3f%%)\n",
							   sstats->skipped,
							   100.0 * sstats->skipped / script_total_cnt);

				}
				printSimpleStats(" - latency", &sstats->latency);
			}

			/*
			 * Report per-command statistics: latencies, retries after errors,
			 * failures (errors without retrying).
			 */
			if (report_per_command)
			{
				Command   **commands;

				printf("%sstatement latencies in milliseconds%s:\n",
					   per_script_stats ? " - " : "",
					   (max_tries == 1 ?
						" and failures" :
						", failures and retries"));

				for (commands = sql_script[i].commands;
					 *commands != NULL;
					 commands++)
				{
					SimpleStats *cstats = &(*commands)->stats;

					if (max_tries == 1)
						printf("   %11.3f  %10" PRId64 " %s\n",
							   (cstats->count > 0) ?
							   1000.0 * cstats->sum / cstats->count : 0.0,
							   (*commands)->failures,
							   (*commands)->first_line);
					else
						printf("   %11.3f  %10" PRId64 " %10" PRId64 " %s\n",
							   (cstats->count > 0) ?
							   1000.0 * cstats->sum / cstats->count : 0.0,
							   (*commands)->failures,
							   (*commands)->retries,
							   (*commands)->first_line);
				}
			}
		}
	}
}

/*
 * Set up a random seed according to seed parameter (NULL means default),
 * and initialize base_random_sequence for use in initializing other sequences.
 */
static bool
set_random_seed(const char *seed)
{
	uint64		iseed;

	if (seed == NULL || strcmp(seed, "time") == 0)
	{
		/* rely on current time */
		iseed = pg_time_now();
	}
	else if (strcmp(seed, "rand") == 0)
	{
		/* use some "strong" random source */
		if (!pg_strong_random(&iseed, sizeof(iseed)))
		{
			pg_log_error("could not generate random seed");
			return false;
		}
	}
	else
	{
		/* parse unsigned-int seed value */
		unsigned long ulseed;
		char		garbage;

		/* Don't try to use UINT64_FORMAT here; it might not work for sscanf */
		if (sscanf(seed, "%lu%c", &ulseed, &garbage) != 1)
		{
			pg_log_error("unrecognized random seed option \"%s\"", seed);
			pg_log_error_detail("Expecting an unsigned integer, \"time\" or \"rand\".");
			return false;
		}
		iseed = (uint64) ulseed;
	}

	if (seed != NULL)
		pg_log_info("setting random seed to %llu", (unsigned long long) iseed);

	random_seed = iseed;

	/* Initialize base_random_sequence using seed */
	pg_prng_seed(&base_random_sequence, (uint64) iseed);

	return true;
}

int
main(int argc, char **argv)
{
	static struct option long_options[] = {
		/* systematic long/short named options */
		{"builtin", required_argument, NULL, 'b'},
		{"client", required_argument, NULL, 'c'},
		{"connect", no_argument, NULL, 'C'},
		{"dbname", required_argument, NULL, 'd'},
		{"define", required_argument, NULL, 'D'},
		{"file", required_argument, NULL, 'f'},
		{"fillfactor", required_argument, NULL, 'F'},
		{"host", required_argument, NULL, 'h'},
		{"initialize", no_argument, NULL, 'i'},
		{"init-steps", required_argument, NULL, 'I'},
		{"jobs", required_argument, NULL, 'j'},
		{"log", no_argument, NULL, 'l'},
		{"latency-limit", required_argument, NULL, 'L'},
		{"no-vacuum", no_argument, NULL, 'n'},
		{"port", required_argument, NULL, 'p'},
		{"progress", required_argument, NULL, 'P'},
		{"protocol", required_argument, NULL, 'M'},
		{"quiet", no_argument, NULL, 'q'},
		{"report-per-command", no_argument, NULL, 'r'},
		{"rate", required_argument, NULL, 'R'},
		{"scale", required_argument, NULL, 's'},
		{"select-only", no_argument, NULL, 'S'},
		{"skip-some-updates", no_argument, NULL, 'N'},
		{"time", required_argument, NULL, 'T'},
		{"transactions", required_argument, NULL, 't'},
		{"username", required_argument, NULL, 'U'},
		{"vacuum-all", no_argument, NULL, 'v'},
		/* long-named only options */
		{"unlogged-tables", no_argument, NULL, 1},
		{"tablespace", required_argument, NULL, 2},
		{"index-tablespace", required_argument, NULL, 3},
		{"sampling-rate", required_argument, NULL, 4},
		{"aggregate-interval", required_argument, NULL, 5},
		{"progress-timestamp", no_argument, NULL, 6},
		{"log-prefix", required_argument, NULL, 7},
		{"foreign-keys", no_argument, NULL, 8},
		{"random-seed", required_argument, NULL, 9},
		{"show-script", required_argument, NULL, 10},
		{"partitions", required_argument, NULL, 11},
		{"partition-method", required_argument, NULL, 12},
		{"failures-detailed", no_argument, NULL, 13},
		{"max-tries", required_argument, NULL, 14},
		{"verbose-errors", no_argument, NULL, 15},
		{"exit-on-abort", no_argument, NULL, 16},
		{"debug", no_argument, NULL, 17},
		{NULL, 0, NULL, 0}
	};

	int			c;
	bool		is_init_mode = false;	/* initialize mode? */
	char	   *initialize_steps = NULL;
	bool		foreign_keys = false;
	bool		is_no_vacuum = false;
	bool		do_vacuum_accounts = false; /* vacuum accounts table? */
	int			optindex;
	bool		scale_given = false;

	bool		benchmarking_option_set = false;
	bool		initialization_option_set = false;
	bool		internal_script_used = false;

	CState	   *state;			/* status of clients */
	TState	   *threads;		/* array of thread */

	pg_time_usec_t
				start_time,		/* start up time */
				bench_start = 0,	/* first recorded benchmarking time */
				conn_total_duration;	/* cumulated connection time in
										 * threads */
	int64		latency_late = 0;
	StatsData	stats;
	int			weight;

	int			i;
	int			nclients_dealt;

#ifdef HAVE_GETRLIMIT
	struct rlimit rlim;
#endif

	PGconn	   *con;
	char	   *env;

	int			exit_code = 0;
	struct timeval tv;

	/*
	 * Record difference between Unix time and instr_time time.  We'll use
	 * this for logging and aggregation.
	 */
	gettimeofday(&tv, NULL);
	epoch_shift = tv.tv_sec * INT64CONST(1000000) + tv.tv_usec - pg_time_now();

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pgbench (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	state = (CState *) pg_malloc0(sizeof(CState));

	/* set random seed early, because it may be used while parsing scripts. */
	if (!set_random_seed(getenv("PGBENCH_RANDOM_SEED")))
		pg_fatal("error while setting random seed from PGBENCH_RANDOM_SEED environment variable");

	while ((c = getopt_long(argc, argv, "b:c:Cd:D:f:F:h:iI:j:lL:M:nNp:P:qrR:s:St:T:U:v", long_options, &optindex)) != -1)
	{
		char	   *script;

		switch (c)
		{
			case 'b':
				if (strcmp(optarg, "list") == 0)
				{
					listAvailableScripts();
					exit(0);
				}
				weight = parseScriptWeight(optarg, &script);
				process_builtin(findBuiltin(script), weight);
				benchmarking_option_set = true;
				internal_script_used = true;
				break;
			case 'c':
				benchmarking_option_set = true;
				if (!option_parse_int(optarg, "-c/--clients", 1, INT_MAX,
									  &nclients))
				{
					exit(1);
				}
#ifdef HAVE_GETRLIMIT
				if (getrlimit(RLIMIT_NOFILE, &rlim) == -1)
					pg_fatal("getrlimit failed: %m");

				if (rlim.rlim_max < nclients + 3)
				{
					pg_log_error("need at least %d open files, but system limit is %ld",
								 nclients + 3, (long) rlim.rlim_max);
					pg_log_error_hint("Reduce number of clients, or use limit/ulimit to increase the system limit.");
					exit(1);
				}

				if (rlim.rlim_cur < nclients + 3)
				{
					rlim.rlim_cur = nclients + 3;
					if (setrlimit(RLIMIT_NOFILE, &rlim) == -1)
					{
						pg_log_error("need at least %d open files, but couldn't raise the limit: %m",
									 nclients + 3);
						pg_log_error_hint("Reduce number of clients, or use limit/ulimit to increase the system limit.");
						exit(1);
					}
				}
#endif							/* HAVE_GETRLIMIT */
				break;
			case 'C':
				benchmarking_option_set = true;
				is_connect = true;
				break;
			case 'd':
				dbName = pg_strdup(optarg);
				break;
			case 'D':
				{
					char	   *p;

					benchmarking_option_set = true;

					if ((p = strchr(optarg, '=')) == NULL || p == optarg || *(p + 1) == '\0')
						pg_fatal("invalid variable definition: \"%s\"", optarg);

					*p++ = '\0';
					if (!putVariable(&state[0].variables, "option", optarg, p))
						exit(1);
				}
				break;
			case 'f':
				weight = parseScriptWeight(optarg, &script);
				process_file(script, weight);
				benchmarking_option_set = true;
				break;
			case 'F':
				initialization_option_set = true;
				if (!option_parse_int(optarg, "-F/--fillfactor", 10, 100,
									  &fillfactor))
					exit(1);
				break;
			case 'h':
				pghost = pg_strdup(optarg);
				break;
			case 'i':
				is_init_mode = true;
				break;
			case 'I':
				pg_free(initialize_steps);
				initialize_steps = pg_strdup(optarg);
				checkInitSteps(initialize_steps);
				initialization_option_set = true;
				break;
			case 'j':			/* jobs */
				benchmarking_option_set = true;
				if (!option_parse_int(optarg, "-j/--jobs", 1, INT_MAX,
									  &nthreads))
				{
					exit(1);
				}
				break;
			case 'l':
				benchmarking_option_set = true;
				use_log = true;
				break;
			case 'L':
				{
					double		limit_ms = atof(optarg);

					if (limit_ms <= 0.0)
						pg_fatal("invalid latency limit: \"%s\"", optarg);
					benchmarking_option_set = true;
					latency_limit = (int64) (limit_ms * 1000);
				}
				break;
			case 'M':
				benchmarking_option_set = true;
				for (querymode = 0; querymode < NUM_QUERYMODE; querymode++)
					if (strcmp(optarg, QUERYMODE[querymode]) == 0)
						break;
				if (querymode >= NUM_QUERYMODE)
					pg_fatal("invalid query mode (-M): \"%s\"", optarg);
				break;
			case 'n':
				is_no_vacuum = true;
				break;
			case 'N':
				process_builtin(findBuiltin("simple-update"), 1);
				benchmarking_option_set = true;
				internal_script_used = true;
				break;
			case 'p':
				pgport = pg_strdup(optarg);
				break;
			case 'P':
				benchmarking_option_set = true;
				if (!option_parse_int(optarg, "-P/--progress", 1, INT_MAX,
									  &progress))
					exit(1);
				break;
			case 'q':
				initialization_option_set = true;
				use_quiet = true;
				break;
			case 'r':
				benchmarking_option_set = true;
				report_per_command = true;
				break;
			case 'R':
				{
					/* get a double from the beginning of option value */
					double		throttle_value = atof(optarg);

					benchmarking_option_set = true;

					if (throttle_value <= 0.0)
						pg_fatal("invalid rate limit: \"%s\"", optarg);
					/* Invert rate limit into per-transaction delay in usec */
					throttle_delay = 1000000.0 / throttle_value;
				}
				break;
			case 's':
				scale_given = true;
				if (!option_parse_int(optarg, "-s/--scale", 1, INT_MAX,
									  &scale))
					exit(1);
				break;
			case 'S':
				process_builtin(findBuiltin("select-only"), 1);
				benchmarking_option_set = true;
				internal_script_used = true;
				break;
			case 't':
				benchmarking_option_set = true;
				if (!option_parse_int(optarg, "-t/--transactions", 1, INT_MAX,
									  &nxacts))
					exit(1);
				break;
			case 'T':
				benchmarking_option_set = true;
				if (!option_parse_int(optarg, "-T/--time", 1, INT_MAX,
									  &duration))
					exit(1);
				break;
			case 'U':
				username = pg_strdup(optarg);
				break;
			case 'v':
				benchmarking_option_set = true;
				do_vacuum_accounts = true;
				break;
			case 1:				/* unlogged-tables */
				initialization_option_set = true;
				unlogged_tables = true;
				break;
			case 2:				/* tablespace */
				initialization_option_set = true;
				tablespace = pg_strdup(optarg);
				break;
			case 3:				/* index-tablespace */
				initialization_option_set = true;
				index_tablespace = pg_strdup(optarg);
				break;
			case 4:				/* sampling-rate */
				benchmarking_option_set = true;
				sample_rate = atof(optarg);
				if (sample_rate <= 0.0 || sample_rate > 1.0)
					pg_fatal("invalid sampling rate: \"%s\"", optarg);
				break;
			case 5:				/* aggregate-interval */
				benchmarking_option_set = true;
				if (!option_parse_int(optarg, "--aggregate-interval", 1, INT_MAX,
									  &agg_interval))
					exit(1);
				break;
			case 6:				/* progress-timestamp */
				progress_timestamp = true;
				benchmarking_option_set = true;
				break;
			case 7:				/* log-prefix */
				benchmarking_option_set = true;
				logfile_prefix = pg_strdup(optarg);
				break;
			case 8:				/* foreign-keys */
				initialization_option_set = true;
				foreign_keys = true;
				break;
			case 9:				/* random-seed */
				benchmarking_option_set = true;
				if (!set_random_seed(optarg))
					pg_fatal("error while setting random seed from --random-seed option");
				break;
			case 10:			/* list */
				{
					const BuiltinScript *s = findBuiltin(optarg);

					fprintf(stderr, "-- %s: %s\n%s\n", s->name, s->desc, s->script);
					exit(0);
				}
				break;
			case 11:			/* partitions */
				initialization_option_set = true;
				if (!option_parse_int(optarg, "--partitions", 0, INT_MAX,
									  &partitions))
					exit(1);
				break;
			case 12:			/* partition-method */
				initialization_option_set = true;
				if (pg_strcasecmp(optarg, "range") == 0)
					partition_method = PART_RANGE;
				else if (pg_strcasecmp(optarg, "hash") == 0)
					partition_method = PART_HASH;
				else
					pg_fatal("invalid partition method, expecting \"range\" or \"hash\", got: \"%s\"",
							 optarg);
				break;
			case 13:			/* failures-detailed */
				benchmarking_option_set = true;
				failures_detailed = true;
				break;
			case 14:			/* max-tries */
				{
					int32		max_tries_arg = atoi(optarg);

					if (max_tries_arg < 0)
						pg_fatal("invalid number of maximum tries: \"%s\"", optarg);

					benchmarking_option_set = true;
					max_tries = (uint32) max_tries_arg;
				}
				break;
			case 15:			/* verbose-errors */
				benchmarking_option_set = true;
				verbose_errors = true;
				break;
			case 16:			/* exit-on-abort */
				benchmarking_option_set = true;
				exit_on_abort = true;
				break;
			case 17:			/* debug */
				pg_logging_increase_verbosity();
				break;
			default:
				/* getopt_long already emitted a complaint */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(1);
		}
	}

	/* set default script if none */
	if (num_scripts == 0 && !is_init_mode)
	{
		process_builtin(findBuiltin("tpcb-like"), 1);
		benchmarking_option_set = true;
		internal_script_used = true;
	}

	/* complete SQL command initialization and compute total weight */
	for (i = 0; i < num_scripts; i++)
	{
		Command   **commands = sql_script[i].commands;

		for (int j = 0; commands[j] != NULL; j++)
			if (commands[j]->type == SQL_COMMAND)
				postprocess_sql_command(commands[j]);

		/* cannot overflow: weight is 32b, total_weight 64b */
		total_weight += sql_script[i].weight;
	}

	if (total_weight == 0 && !is_init_mode)
		pg_fatal("total script weight must not be zero");

	/* show per script stats if several scripts are used */
	if (num_scripts > 1)
		per_script_stats = true;

	/*
	 * Don't need more threads than there are clients.  (This is not merely an
	 * optimization; throttle_delay is calculated incorrectly below if some
	 * threads have no clients assigned to them.)
	 */
	if (nthreads > nclients)
		nthreads = nclients;

	/*
	 * Convert throttle_delay to a per-thread delay time.  Note that this
	 * might be a fractional number of usec, but that's OK, since it's just
	 * the center of a Poisson distribution of delays.
	 */
	throttle_delay *= nthreads;

	if (dbName == NULL)
	{
		if (argc > optind)
			dbName = argv[optind++];
		else
		{
			if ((env = getenv("PGDATABASE")) != NULL && *env != '\0')
				dbName = env;
			else if ((env = getenv("PGUSER")) != NULL && *env != '\0')
				dbName = env;
			else
				dbName = get_user_name_or_exit(progname);
		}
	}

	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (is_init_mode)
	{
		if (benchmarking_option_set)
			pg_fatal("some of the specified options cannot be used in initialization (-i) mode");

		if (partitions == 0 && partition_method != PART_NONE)
			pg_fatal("--partition-method requires greater than zero --partitions");

		/* set default method */
		if (partitions > 0 && partition_method == PART_NONE)
			partition_method = PART_RANGE;

		if (initialize_steps == NULL)
			initialize_steps = pg_strdup(DEFAULT_INIT_STEPS);

		if (is_no_vacuum)
		{
			/* Remove any vacuum step in initialize_steps */
			char	   *p;

			while ((p = strchr(initialize_steps, 'v')) != NULL)
				*p = ' ';
		}

		if (foreign_keys)
		{
			/* Add 'f' to end of initialize_steps, if not already there */
			if (strchr(initialize_steps, 'f') == NULL)
			{
				initialize_steps = (char *)
					pg_realloc(initialize_steps,
							   strlen(initialize_steps) + 2);
				strcat(initialize_steps, "f");
			}
		}

		runInitSteps(initialize_steps);
		exit(0);
	}
	else
	{
		if (initialization_option_set)
			pg_fatal("some of the specified options cannot be used in benchmarking mode");
	}

	if (nxacts > 0 && duration > 0)
		pg_fatal("specify either a number of transactions (-t) or a duration (-T), not both");

	/* Use DEFAULT_NXACTS if neither nxacts nor duration is specified. */
	if (nxacts <= 0 && duration <= 0)
		nxacts = DEFAULT_NXACTS;

	/* --sampling-rate may be used only with -l */
	if (sample_rate > 0.0 && !use_log)
		pg_fatal("log sampling (--sampling-rate) is allowed only when logging transactions (-l)");

	/* --sampling-rate may not be used with --aggregate-interval */
	if (sample_rate > 0.0 && agg_interval > 0)
		pg_fatal("log sampling (--sampling-rate) and aggregation (--aggregate-interval) cannot be used at the same time");

	if (agg_interval > 0 && !use_log)
		pg_fatal("log aggregation is allowed only when actually logging transactions");

	if (!use_log && logfile_prefix)
		pg_fatal("log file prefix (--log-prefix) is allowed only when logging transactions (-l)");

	if (duration > 0 && agg_interval > duration)
		pg_fatal("number of seconds for aggregation (%d) must not be higher than test duration (%d)", agg_interval, duration);

	if (duration > 0 && agg_interval > 0 && duration % agg_interval != 0)
		pg_fatal("duration (%d) must be a multiple of aggregation interval (%d)", duration, agg_interval);

	if (progress_timestamp && progress == 0)
		pg_fatal("--progress-timestamp is allowed only under --progress");

	if (!max_tries)
	{
		if (!latency_limit && duration <= 0)
			pg_fatal("an unlimited number of transaction tries can only be used with --latency-limit or a duration (-T)");
	}

	/*
	 * save main process id in the global variable because process id will be
	 * changed after fork.
	 */
	main_pid = (int) getpid();

	if (nclients > 1)
	{
		state = (CState *) pg_realloc(state, sizeof(CState) * nclients);
		memset(state + 1, 0, sizeof(CState) * (nclients - 1));

		/* copy any -D switch values to all clients */
		for (i = 1; i < nclients; i++)
		{
			int			j;

			state[i].id = i;
			for (j = 0; j < state[0].variables.nvars; j++)
			{
				Variable   *var = &state[0].variables.vars[j];

				if (var->value.type != PGBT_NO_VALUE)
				{
					if (!putVariableValue(&state[i].variables, "startup",
										  var->name, &var->value))
						exit(1);
				}
				else
				{
					if (!putVariable(&state[i].variables, "startup",
									 var->name, var->svalue))
						exit(1);
				}
			}
		}
	}

	/* other CState initializations */
	for (i = 0; i < nclients; i++)
	{
		state[i].cstack = conditional_stack_create();
		initRandomState(&state[i].cs_func_rs);
	}

	/* opening connection... */
	con = doConnect();
	if (con == NULL)
		pg_fatal("could not create connection for setup");

	/* report pgbench and server versions */
	printVersion(con);

	pg_log_debug("pghost: %s pgport: %s nclients: %d %s: %d dbName: %s",
				 PQhost(con), PQport(con), nclients,
				 duration <= 0 ? "nxacts" : "duration",
				 duration <= 0 ? nxacts : duration, PQdb(con));

	if (internal_script_used)
		GetTableInfo(con, scale_given);

	/*
	 * :scale variables normally get -s or database scale, but don't override
	 * an explicit -D switch
	 */
	if (lookupVariable(&state[0].variables, "scale") == NULL)
	{
		for (i = 0; i < nclients; i++)
		{
			if (!putVariableInt(&state[i].variables, "startup", "scale", scale))
				exit(1);
		}
	}

	/*
	 * Define a :client_id variable that is unique per connection. But don't
	 * override an explicit -D switch.
	 */
	if (lookupVariable(&state[0].variables, "client_id") == NULL)
	{
		for (i = 0; i < nclients; i++)
			if (!putVariableInt(&state[i].variables, "startup", "client_id", i))
				exit(1);
	}

	/* set default seed for hash functions */
	if (lookupVariable(&state[0].variables, "default_seed") == NULL)
	{
		uint64		seed = pg_prng_uint64(&base_random_sequence);

		for (i = 0; i < nclients; i++)
			if (!putVariableInt(&state[i].variables, "startup", "default_seed",
								(int64) seed))
				exit(1);
	}

	/* set random seed unless overwritten */
	if (lookupVariable(&state[0].variables, "random_seed") == NULL)
	{
		for (i = 0; i < nclients; i++)
			if (!putVariableInt(&state[i].variables, "startup", "random_seed",
								random_seed))
				exit(1);
	}

	if (!is_no_vacuum)
	{
		fprintf(stderr, "starting vacuum...");
		tryExecuteStatement(con, "vacuum pgbench_branches");
		tryExecuteStatement(con, "vacuum pgbench_tellers");
		tryExecuteStatement(con, "truncate pgbench_history");
		fprintf(stderr, "end.\n");

		if (do_vacuum_accounts)
		{
			fprintf(stderr, "starting vacuum pgbench_accounts...");
			tryExecuteStatement(con, "vacuum analyze pgbench_accounts");
			fprintf(stderr, "end.\n");
		}
	}
	PQfinish(con);

	/* set up thread data structures */
	threads = (TState *) pg_malloc(sizeof(TState) * nthreads);
	nclients_dealt = 0;

	for (i = 0; i < nthreads; i++)
	{
		TState	   *thread = &threads[i];

		thread->tid = i;
		thread->state = &state[nclients_dealt];
		thread->nstate =
			(nclients - nclients_dealt + nthreads - i - 1) / (nthreads - i);
		initRandomState(&thread->ts_choose_rs);
		initRandomState(&thread->ts_throttle_rs);
		initRandomState(&thread->ts_sample_rs);
		thread->logfile = NULL; /* filled in later */
		thread->latency_late = 0;
		initStats(&thread->stats, 0);

		nclients_dealt += thread->nstate;
	}

	/* all clients must be assigned to a thread */
	Assert(nclients_dealt == nclients);

	/* get start up time for the whole computation */
	start_time = pg_time_now();

	/* set alarm if duration is specified. */
	if (duration > 0)
		setalarm(duration);

	errno = THREAD_BARRIER_INIT(&barrier, nthreads);
	if (errno != 0)
		pg_fatal("could not initialize barrier: %m");

	/* start all threads but thread 0 which is executed directly later */
	for (i = 1; i < nthreads; i++)
	{
		TState	   *thread = &threads[i];

		thread->create_time = pg_time_now();
		errno = THREAD_CREATE(&thread->thread, threadRun, thread);

		if (errno != 0)
			pg_fatal("could not create thread: %m");
	}

	/* compute when to stop */
	threads[0].create_time = pg_time_now();
	if (duration > 0)
		end_time = threads[0].create_time + (int64) 1000000 * duration;

	/* run thread 0 directly */
	(void) threadRun(&threads[0]);

	/* wait for other threads and accumulate results */
	initStats(&stats, 0);
	conn_total_duration = 0;

	for (i = 0; i < nthreads; i++)
	{
		TState	   *thread = &threads[i];

		if (i > 0)
			THREAD_JOIN(thread->thread);

		for (int j = 0; j < thread->nstate; j++)
			if (thread->state[j].state != CSTATE_FINISHED)
				exit_code = 2;

		/* aggregate thread level stats */
		mergeSimpleStats(&stats.latency, &thread->stats.latency);
		mergeSimpleStats(&stats.lag, &thread->stats.lag);
		stats.cnt += thread->stats.cnt;
		stats.skipped += thread->stats.skipped;
		stats.retries += thread->stats.retries;
		stats.retried += thread->stats.retried;
		stats.serialization_failures += thread->stats.serialization_failures;
		stats.deadlock_failures += thread->stats.deadlock_failures;
		latency_late += thread->latency_late;
		conn_total_duration += thread->conn_duration;

		/* first recorded benchmarking start time */
		if (bench_start == 0 || thread->bench_start < bench_start)
			bench_start = thread->bench_start;
	}

	/*
	 * All connections should be already closed in threadRun(), so this
	 * disconnect_all() will be a no-op, but clean up the connections just to
	 * be sure. We don't need to measure the disconnection delays here.
	 */
	disconnect_all(state, nclients);

	/*
	 * Beware that performance of short benchmarks with many threads and
	 * possibly long transactions can be deceptive because threads do not
	 * start and finish at the exact same time. The total duration computed
	 * here encompasses all transactions so that tps shown is somehow slightly
	 * underestimated.
	 */
	printResults(&stats, pg_time_now() - bench_start, conn_total_duration,
				 bench_start - start_time, latency_late);

	THREAD_BARRIER_DESTROY(&barrier);

	if (exit_code != 0)
		pg_log_error("Run was aborted; the above results are incomplete.");

	return exit_code;
}

static THREAD_FUNC_RETURN_TYPE THREAD_FUNC_CC
threadRun(void *arg)
{
	TState	   *thread = (TState *) arg;
	CState	   *state = thread->state;
	pg_time_usec_t start;
	int			nstate = thread->nstate;
	int			remains = nstate;	/* number of remaining clients */
	socket_set *sockets = alloc_socket_set(nstate);
	int64		thread_start,
				last_report,
				next_report;
	StatsData	last,
				aggs;

	/* open log file if requested */
	if (use_log)
	{
		char		logpath[MAXPGPATH];
		char	   *prefix = logfile_prefix ? logfile_prefix : "pgbench_log";

		if (thread->tid == 0)
			snprintf(logpath, sizeof(logpath), "%s.%d", prefix, main_pid);
		else
			snprintf(logpath, sizeof(logpath), "%s.%d.%d", prefix, main_pid, thread->tid);

		thread->logfile = fopen(logpath, "w");

		if (thread->logfile == NULL)
			pg_fatal("could not open logfile \"%s\": %m", logpath);
	}

	/* explicitly initialize the state machines */
	for (int i = 0; i < nstate; i++)
		state[i].state = CSTATE_CHOOSE_SCRIPT;

	/* READY */
	THREAD_BARRIER_WAIT(&barrier);

	thread_start = pg_time_now();
	thread->started_time = thread_start;
	thread->conn_duration = 0;
	last_report = thread_start;
	next_report = last_report + (int64) 1000000 * progress;

	/* STEADY */
	if (!is_connect)
	{
		/* make connections to the database before starting */
		for (int i = 0; i < nstate; i++)
		{
			if ((state[i].con = doConnect()) == NULL)
			{
				/* coldly abort on initial connection failure */
				pg_fatal("could not create connection for client %d",
						 state[i].id);
			}
		}
	}

	/* GO */
	THREAD_BARRIER_WAIT(&barrier);

	start = pg_time_now();
	thread->bench_start = start;
	thread->throttle_trigger = start;

	/*
	 * The log format currently has Unix epoch timestamps with whole numbers
	 * of seconds.  Round the first aggregate's start time down to the nearest
	 * Unix epoch second (the very first aggregate might really have started a
	 * fraction of a second later, but later aggregates are measured from the
	 * whole number time that is actually logged).
	 */
	initStats(&aggs, (start + epoch_shift) / 1000000 * 1000000);
	last = aggs;

	/* loop till all clients have terminated */
	while (remains > 0)
	{
		int			nsocks;		/* number of sockets to be waited for */
		pg_time_usec_t min_usec;
		pg_time_usec_t now = 0; /* set this only if needed */

		/*
		 * identify which client sockets should be checked for input, and
		 * compute the nearest time (if any) at which we need to wake up.
		 */
		clear_socket_set(sockets);
		nsocks = 0;
		min_usec = PG_INT64_MAX;
		for (int i = 0; i < nstate; i++)
		{
			CState	   *st = &state[i];

			if (st->state == CSTATE_SLEEP || st->state == CSTATE_THROTTLE)
			{
				/* a nap from the script, or under throttling */
				pg_time_usec_t this_usec;

				/* get current time if needed */
				pg_time_now_lazy(&now);

				/* min_usec should be the minimum delay across all clients */
				this_usec = (st->state == CSTATE_SLEEP ?
							 st->sleep_until : st->txn_scheduled) - now;
				if (min_usec > this_usec)
					min_usec = this_usec;
			}
			else if (st->state == CSTATE_WAIT_RESULT ||
					 st->state == CSTATE_WAIT_ROLLBACK_RESULT)
			{
				/*
				 * waiting for result from server - nothing to do unless the
				 * socket is readable
				 */
				int			sock = PQsocket(st->con);

				if (sock < 0)
				{
					pg_log_error("invalid socket: %s", PQerrorMessage(st->con));
					goto done;
				}

				add_socket_to_set(sockets, sock, nsocks++);
			}
			else if (st->state != CSTATE_ABORTED &&
					 st->state != CSTATE_FINISHED)
			{
				/*
				 * This client thread is ready to do something, so we don't
				 * want to wait.  No need to examine additional clients.
				 */
				min_usec = 0;
				break;
			}
		}

		/* also wake up to print the next progress report on time */
		if (progress && min_usec > 0 && thread->tid == 0)
		{
			pg_time_now_lazy(&now);

			if (now >= next_report)
				min_usec = 0;
			else if ((next_report - now) < min_usec)
				min_usec = next_report - now;
		}

		/*
		 * If no clients are ready to execute actions, sleep until we receive
		 * data on some client socket or the timeout (if any) elapses.
		 */
		if (min_usec > 0)
		{
			int			rc = 0;

			if (min_usec != PG_INT64_MAX)
			{
				if (nsocks > 0)
				{
					rc = wait_on_socket_set(sockets, min_usec);
				}
				else			/* nothing active, simple sleep */
				{
					pg_usleep(min_usec);
				}
			}
			else				/* no explicit delay, wait without timeout */
			{
				rc = wait_on_socket_set(sockets, 0);
			}

			if (rc < 0)
			{
				if (errno == EINTR)
				{
					/* On EINTR, go back to top of loop */
					continue;
				}
				/* must be something wrong */
				pg_log_error("%s() failed: %m", SOCKET_WAIT_METHOD);
				goto done;
			}
		}
		else
		{
			/* min_usec <= 0, i.e. something needs to be executed now */

			/* If we didn't wait, don't try to read any data */
			clear_socket_set(sockets);
		}

		/* ok, advance the state machine of each connection */
		nsocks = 0;
		for (int i = 0; i < nstate; i++)
		{
			CState	   *st = &state[i];

			if (st->state == CSTATE_WAIT_RESULT ||
				st->state == CSTATE_WAIT_ROLLBACK_RESULT)
			{
				/* don't call advanceConnectionState unless data is available */
				int			sock = PQsocket(st->con);

				if (sock < 0)
				{
					pg_log_error("invalid socket: %s", PQerrorMessage(st->con));
					goto done;
				}

				if (!socket_has_input(sockets, sock, nsocks++))
					continue;
			}
			else if (st->state == CSTATE_FINISHED ||
					 st->state == CSTATE_ABORTED)
			{
				/* this client is done, no need to consider it anymore */
				continue;
			}

			advanceConnectionState(thread, st, &aggs);

			/*
			 * If --exit-on-abort is used, the program is going to exit when
			 * any client is aborted.
			 */
			if (exit_on_abort && st->state == CSTATE_ABORTED)
				goto done;

			/*
			 * If advanceConnectionState changed client to finished state,
			 * that's one fewer client that remains.
			 */
			else if (st->state == CSTATE_FINISHED ||
					 st->state == CSTATE_ABORTED)
				remains--;
		}

		/* progress report is made by thread 0 for all threads */
		if (progress && thread->tid == 0)
		{
			pg_time_usec_t now2 = pg_time_now();

			if (now2 >= next_report)
			{
				/*
				 * Horrible hack: this relies on the thread pointer we are
				 * passed to be equivalent to threads[0], that is the first
				 * entry of the threads array.  That is why this MUST be done
				 * by thread 0 and not any other.
				 */
				printProgressReport(thread, thread_start, now2,
									&last, &last_report);

				/*
				 * Ensure that the next report is in the future, in case
				 * pgbench/postgres got stuck somewhere.
				 */
				do
				{
					next_report += (int64) 1000000 * progress;
				} while (now2 >= next_report);
			}
		}
	}

done:
	if (exit_on_abort)
	{
		/*
		 * Abort if any client is not finished, meaning some error occurred.
		 */
		for (int i = 0; i < nstate; i++)
		{
			if (state[i].state != CSTATE_FINISHED)
			{
				pg_log_error("Run was aborted due to an error in thread %d",
							 thread->tid);
				exit(2);
			}
		}
	}

	disconnect_all(state, nstate);

	if (thread->logfile)
	{
		if (agg_interval > 0)
		{
			/* log aggregated but not yet reported transactions */
			doLog(thread, state, &aggs, false, 0, 0);
		}
		fclose(thread->logfile);
		thread->logfile = NULL;
	}
	free_socket_set(sockets);
	THREAD_FUNC_RETURN;
}

static void
finishCon(CState *st)
{
	if (st->con != NULL)
	{
		PQfinish(st->con);
		st->con = NULL;
	}
}

/*
 * Support for duration option: set timer_exceeded after so many seconds.
 */

#ifndef WIN32

static void
handle_sig_alarm(SIGNAL_ARGS)
{
	timer_exceeded = true;
}

static void
setalarm(int seconds)
{
	pqsignal(SIGALRM, handle_sig_alarm);
	alarm(seconds);
}

#else							/* WIN32 */

static VOID CALLBACK
win32_timer_callback(PVOID lpParameter, BOOLEAN TimerOrWaitFired)
{
	timer_exceeded = true;
}

static void
setalarm(int seconds)
{
	HANDLE		queue;
	HANDLE		timer;

	/* This function will be called at most once, so we can cheat a bit. */
	queue = CreateTimerQueue();
	if (seconds > ((DWORD) -1) / 1000 ||
		!CreateTimerQueueTimer(&timer, queue,
							   win32_timer_callback, NULL, seconds * 1000, 0,
							   WT_EXECUTEINTIMERTHREAD | WT_EXECUTEONLYONCE))
		pg_fatal("failed to set timer");
}

#endif							/* WIN32 */


/*
 * These functions provide an abstraction layer that hides the syscall
 * we use to wait for input on a set of sockets.
 *
 * Currently there are two implementations, based on ppoll(2) and select(2).
 * ppoll() is preferred where available due to its typically higher ceiling
 * on the number of usable sockets.  We do not use the more-widely-available
 * poll(2) because it only offers millisecond timeout resolution, which could
 * be problematic with high --rate settings.
 *
 * Function APIs:
 *
 * alloc_socket_set: allocate an empty socket set with room for up to
 *		"count" sockets.
 *
 * free_socket_set: deallocate a socket set.
 *
 * clear_socket_set: reset a socket set to empty.
 *
 * add_socket_to_set: add socket with indicated FD to slot "idx" in the
 *		socket set.  Slots must be filled in order, starting with 0.
 *
 * wait_on_socket_set: wait for input on any socket in set, or for timeout
 *		to expire.  timeout is measured in microseconds; 0 means wait forever.
 *		Returns result code of underlying syscall (>=0 if OK, else see errno).
 *
 * socket_has_input: after waiting, call this to see if given socket has
 *		input.  fd and idx parameters should match some previous call to
 *		add_socket_to_set.
 *
 * Note that wait_on_socket_set destructively modifies the state of the
 * socket set.  After checking for input, caller must apply clear_socket_set
 * and add_socket_to_set again before waiting again.
 */

#ifdef POLL_USING_PPOLL

static socket_set *
alloc_socket_set(int count)
{
	socket_set *sa;

	sa = (socket_set *) pg_malloc0(offsetof(socket_set, pollfds) +
								   sizeof(struct pollfd) * count);
	sa->maxfds = count;
	sa->curfds = 0;
	return sa;
}

static void
free_socket_set(socket_set *sa)
{
	pg_free(sa);
}

static void
clear_socket_set(socket_set *sa)
{
	sa->curfds = 0;
}

static void
add_socket_to_set(socket_set *sa, int fd, int idx)
{
	Assert(idx < sa->maxfds && idx == sa->curfds);
	sa->pollfds[idx].fd = fd;
	sa->pollfds[idx].events = POLLIN;
	sa->pollfds[idx].revents = 0;
	sa->curfds++;
}

static int
wait_on_socket_set(socket_set *sa, int64 usecs)
{
	if (usecs > 0)
	{
		struct timespec timeout;

		timeout.tv_sec = usecs / 1000000;
		timeout.tv_nsec = (usecs % 1000000) * 1000;
		return ppoll(sa->pollfds, sa->curfds, &timeout, NULL);
	}
	else
	{
		return ppoll(sa->pollfds, sa->curfds, NULL, NULL);
	}
}

static bool
socket_has_input(socket_set *sa, int fd, int idx)
{
	/*
	 * In some cases, threadRun will apply clear_socket_set and then try to
	 * apply socket_has_input anyway with arguments that it used before that,
	 * or might've used before that except that it exited its setup loop
	 * early.  Hence, if the socket set is empty, silently return false
	 * regardless of the parameters.  If it's not empty, we can Assert that
	 * the parameters match a previous call.
	 */
	if (sa->curfds == 0)
		return false;

	Assert(idx < sa->curfds && sa->pollfds[idx].fd == fd);
	return (sa->pollfds[idx].revents & POLLIN) != 0;
}

#endif							/* POLL_USING_PPOLL */

#ifdef POLL_USING_SELECT

static socket_set *
alloc_socket_set(int count)
{
	return (socket_set *) pg_malloc0(sizeof(socket_set));
}

static void
free_socket_set(socket_set *sa)
{
	pg_free(sa);
}

static void
clear_socket_set(socket_set *sa)
{
	FD_ZERO(&sa->fds);
	sa->maxfd = -1;
}

static void
add_socket_to_set(socket_set *sa, int fd, int idx)
{
	/* See connect_slot() for background on this code. */
#ifdef WIN32
	if (sa->fds.fd_count + 1 >= FD_SETSIZE)
	{
		pg_log_error("too many concurrent database clients for this platform: %d",
					 sa->fds.fd_count + 1);
		exit(1);
	}
#else
	if (fd < 0 || fd >= FD_SETSIZE)
	{
		pg_log_error("socket file descriptor out of range for select(): %d",
					 fd);
		pg_log_error_hint("Try fewer concurrent database clients.");
		exit(1);
	}
#endif
	FD_SET(fd, &sa->fds);
	if (fd > sa->maxfd)
		sa->maxfd = fd;
}

static int
wait_on_socket_set(socket_set *sa, int64 usecs)
{
	if (usecs > 0)
	{
		struct timeval timeout;

		timeout.tv_sec = usecs / 1000000;
		timeout.tv_usec = usecs % 1000000;
		return select(sa->maxfd + 1, &sa->fds, NULL, NULL, &timeout);
	}
	else
	{
		return select(sa->maxfd + 1, &sa->fds, NULL, NULL, NULL);
	}
}

static bool
socket_has_input(socket_set *sa, int fd, int idx)
{
	return (FD_ISSET(fd, &sa->fds) != 0);
}

#endif							/* POLL_USING_SELECT */
