/*-------------------------------------------------------------------------
 *
 * pg_stat_statements.c
 *		Track statement planning and execution times as well as resource
 *		usage across a whole database cluster.
 *
 * Execution costs are totaled for each distinct source query, and kept in
 * a shared hashtable.  (We track only as many distinct queries as will fit
 * in the designated amount of shared memory.)
 *
 * Starting in Postgres 9.2, this module normalized query entries.  As of
 * Postgres 14, the normalization is done by the core if compute_query_id is
 * enabled, or optionally by third-party modules.
 *
 * To facilitate presenting entries to users, we create "representative" query
 * strings in which constants are replaced with parameter symbols ($n), to
 * make it clearer what a normalized entry can represent.  To save on shared
 * memory, and to avoid having to truncate oversized query strings, we store
 * these strings in a temporary external query-texts file.  Offsets into this
 * file are kept in shared memory.
 *
 * Note about locking issues: to create or delete an entry in the shared
 * hashtable, one must hold pgss->lock exclusively.  Modifying any field
 * in an entry except the counters requires the same.  To look up an entry,
 * one must hold the lock shared.  To read or update the counters within
 * an entry, one must hold the lock shared or exclusive (so the entry doesn't
 * disappear!) and also take the entry's mutex spinlock.
 * The shared state variable pgss->extent (the next free spot in the external
 * query-text file) should be accessed only while holding either the
 * pgss->mutex spinlock, or exclusive lock on pgss->lock.  We use the mutex to
 * allow reserving file space while holding only shared lock on pgss->lock.
 * Rewriting the entire external query-text file, eg for garbage collection,
 * requires holding pgss->lock exclusively; this allows individual entries
 * in the file to be read or written while holding only shared lock.
 *
 *
 * Copyright (c) 2008-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pg_stat_statements/pg_stat_statements.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/parallel.h"
#include "catalog/pg_authid.h"
#include "common/hashfn.h"
#include "executor/instrument.h"
#include "funcapi.h"
#include "jit/jit.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "optimizer/planner.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "parser/scanner.h"
#include "parser/scansup.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/queryjumble.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

PG_MODULE_MAGIC;

/* Location of permanent stats file (valid when database is shut down) */
#define PGSS_DUMP_FILE	PGSTAT_STAT_PERMANENT_DIRECTORY "/pg_stat_statements.stat"

/*
 * Location of external query text file.
 */
#define PGSS_TEXT_FILE	PG_STAT_TMP_DIR "/pgss_query_texts.stat"

/* Magic number identifying the stats file format */
static const uint32 PGSS_FILE_HEADER = 0x20220408;

/* PostgreSQL major version number, changes in which invalidate all entries */
static const uint32 PGSS_PG_MAJOR_VERSION = PG_VERSION_NUM / 100;

/* XXX: Should USAGE_EXEC reflect execution time and/or buffer usage? */
#define USAGE_EXEC(duration)	(1.0)
#define USAGE_INIT				(1.0)	/* including initial planning */
#define ASSUMED_MEDIAN_INIT		(10.0)	/* initial assumed median usage */
#define ASSUMED_LENGTH_INIT		1024	/* initial assumed mean query length */
#define USAGE_DECREASE_FACTOR	(0.99)	/* decreased every entry_dealloc */
#define STICKY_DECREASE_FACTOR	(0.50)	/* factor for sticky entries */
#define USAGE_DEALLOC_PERCENT	5	/* free this % of entries at once */
#define IS_STICKY(c)	((c.calls[PGSS_PLAN] + c.calls[PGSS_EXEC]) == 0)

/*
 * Utility statements that pgss_ProcessUtility and pgss_post_parse_analyze
 * ignores.
 */
#define PGSS_HANDLED_UTILITY(n)		(!IsA(n, ExecuteStmt) && \
									!IsA(n, PrepareStmt) && \
									!IsA(n, DeallocateStmt))

/*
 * Extension version number, for supporting older extension versions' objects
 */
typedef enum pgssVersion
{
	PGSS_V1_0 = 0,
	PGSS_V1_1,
	PGSS_V1_2,
	PGSS_V1_3,
	PGSS_V1_8,
	PGSS_V1_9,
	PGSS_V1_10
} pgssVersion;

typedef enum pgssStoreKind
{
	PGSS_INVALID = -1,

	/*
	 * PGSS_PLAN and PGSS_EXEC must be respectively 0 and 1 as they're used to
	 * reference the underlying values in the arrays in the Counters struct,
	 * and this order is required in pg_stat_statements_internal().
	 */
	PGSS_PLAN = 0,
	PGSS_EXEC,

	PGSS_NUMKIND				/* Must be last value of this enum */
} pgssStoreKind;

/*
 * Hashtable key that defines the identity of a hashtable entry.  We separate
 * queries by user and by database even if they are otherwise identical.
 *
 * If you add a new key to this struct, make sure to teach pgss_store() to
 * zero the padding bytes.  Otherwise, things will break, because pgss_hash is
 * created using HASH_BLOBS, and thus tag_hash is used to hash this.

 */
typedef struct pgssHashKey
{
	Oid			userid;			/* user OID */
	Oid			dbid;			/* database OID */
	uint64		queryid;		/* query identifier */
	bool		toplevel;		/* query executed at top level */
} pgssHashKey;

/*
 * The actual stats counters kept within pgssEntry.
 */
typedef struct Counters
{
	int64		calls[PGSS_NUMKIND];	/* # of times planned/executed */
	double		total_time[PGSS_NUMKIND];	/* total planning/execution time,
											 * in msec */
	double		min_time[PGSS_NUMKIND]; /* minimum planning/execution time in
										 * msec */
	double		max_time[PGSS_NUMKIND]; /* maximum planning/execution time in
										 * msec */
	double		mean_time[PGSS_NUMKIND];	/* mean planning/execution time in
											 * msec */
	double		sum_var_time[PGSS_NUMKIND]; /* sum of variances in
											 * planning/execution time in msec */
	int64		rows;			/* total # of retrieved or affected rows */
	int64		shared_blks_hit;	/* # of shared buffer hits */
	int64		shared_blks_read;	/* # of shared disk blocks read */
	int64		shared_blks_dirtied;	/* # of shared disk blocks dirtied */
	int64		shared_blks_written;	/* # of shared disk blocks written */
	int64		local_blks_hit; /* # of local buffer hits */
	int64		local_blks_read;	/* # of local disk blocks read */
	int64		local_blks_dirtied; /* # of local disk blocks dirtied */
	int64		local_blks_written; /* # of local disk blocks written */
	int64		temp_blks_read; /* # of temp blocks read */
	int64		temp_blks_written;	/* # of temp blocks written */
	double		blk_read_time;	/* time spent reading blocks, in msec */
	double		blk_write_time; /* time spent writing blocks, in msec */
	double		temp_blk_read_time; /* time spent reading temp blocks, in msec */
	double		temp_blk_write_time;	/* time spent writing temp blocks, in
										 * msec */
	double		usage;			/* usage factor */
	int64		wal_records;	/* # of WAL records generated */
	int64		wal_fpi;		/* # of WAL full page images generated */
	uint64		wal_bytes;		/* total amount of WAL generated in bytes */
	int64		jit_functions;	/* total number of JIT functions emitted */
	double		jit_generation_time;	/* total time to generate jit code */
	int64		jit_inlining_count; /* number of times inlining time has been
									 * > 0 */
	double		jit_inlining_time;	/* total time to inline jit code */
	int64		jit_optimization_count; /* number of times optimization time
										 * has been > 0 */
	double		jit_optimization_time;	/* total time to optimize jit code */
	int64		jit_emission_count; /* number of times emission time has been
									 * > 0 */
	double		jit_emission_time;	/* total time to emit jit code */
} Counters;

/*
 * Global statistics for pg_stat_statements
 */
typedef struct pgssGlobalStats
{
	int64		dealloc;		/* # of times entries were deallocated */
	TimestampTz stats_reset;	/* timestamp with all stats reset */
} pgssGlobalStats;

/*
 * Statistics per statement
 *
 * Note: in event of a failure in garbage collection of the query text file,
 * we reset query_offset to zero and query_len to -1.  This will be seen as
 * an invalid state by qtext_fetch().
 */
typedef struct pgssEntry
{
	pgssHashKey key;			/* hash key of entry - MUST BE FIRST */
	Counters	counters;		/* the statistics for this query */
	Size		query_offset;	/* query text offset in external file */
	int			query_len;		/* # of valid bytes in query string, or -1 */
	int			encoding;		/* query text encoding */
	slock_t		mutex;			/* protects the counters only */
} pgssEntry;

/*
 * Global shared state
 */
typedef struct pgssSharedState
{
	LWLock	   *lock;			/* protects hashtable search/modification */
	double		cur_median_usage;	/* current median usage in hashtable */
	Size		mean_query_len; /* current mean entry text length */
	slock_t		mutex;			/* protects following fields only: */
	Size		extent;			/* current extent of query file */
	int			n_writers;		/* number of active writers to query file */
	int			gc_count;		/* query file garbage collection cycle count */
	pgssGlobalStats stats;		/* global statistics for pgss */
} pgssSharedState;

/*---- Local variables ----*/

/* Current nesting depth of ExecutorRun+ProcessUtility calls */
static int	exec_nested_level = 0;

/* Current nesting depth of planner calls */
static int	plan_nested_level = 0;

/* Saved hook values in case of unload */
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static planner_hook_type prev_planner_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;
static ProcessUtility_hook_type prev_ProcessUtility = NULL;

/* Links to shared memory state */
static pgssSharedState *pgss = NULL;
static HTAB *pgss_hash = NULL;

/*---- GUC variables ----*/

typedef enum
{
	PGSS_TRACK_NONE,			/* track no statements */
	PGSS_TRACK_TOP,				/* only top level statements */
	PGSS_TRACK_ALL				/* all statements, including nested ones */
}			PGSSTrackLevel;

static const struct config_enum_entry track_options[] =
{
	{"none", PGSS_TRACK_NONE, false},
	{"top", PGSS_TRACK_TOP, false},
	{"all", PGSS_TRACK_ALL, false},
	{NULL, 0, false}
};

static int	pgss_max;			/* max # statements to track */
static int	pgss_track;			/* tracking level */
static bool pgss_track_utility; /* whether to track utility commands */
static bool pgss_track_planning;	/* whether to track planning duration */
static bool pgss_save;			/* whether to save stats across shutdown */


#define pgss_enabled(level) \
	(!IsParallelWorker() && \
	(pgss_track == PGSS_TRACK_ALL || \
	(pgss_track == PGSS_TRACK_TOP && (level) == 0)))

#define record_gc_qtexts() \
	do { \
		volatile pgssSharedState *s = (volatile pgssSharedState *) pgss; \
		SpinLockAcquire(&s->mutex); \
		s->gc_count++; \
		SpinLockRelease(&s->mutex); \
	} while(0)

/*---- Function declarations ----*/

void		_PG_init(void);

PG_FUNCTION_INFO_V1(pg_stat_statements_reset);
PG_FUNCTION_INFO_V1(pg_stat_statements_reset_1_7);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_2);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_3);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_8);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_9);
PG_FUNCTION_INFO_V1(pg_stat_statements_1_10);
PG_FUNCTION_INFO_V1(pg_stat_statements);
PG_FUNCTION_INFO_V1(pg_stat_statements_info);

static void pgss_shmem_request(void);
static void pgss_shmem_startup(void);
static void pgss_shmem_shutdown(int code, Datum arg);
static void pgss_post_parse_analyze(ParseState *pstate, Query *query,
									JumbleState *jstate);
static PlannedStmt *pgss_planner(Query *parse,
								 const char *query_string,
								 int cursorOptions,
								 ParamListInfo boundParams);
static void pgss_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void pgss_ExecutorRun(QueryDesc *queryDesc,
							 ScanDirection direction,
							 uint64 count, bool execute_once);
static void pgss_ExecutorFinish(QueryDesc *queryDesc);
static void pgss_ExecutorEnd(QueryDesc *queryDesc);
static void pgss_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
								bool readOnlyTree,
								ProcessUtilityContext context, ParamListInfo params,
								QueryEnvironment *queryEnv,
								DestReceiver *dest, QueryCompletion *qc);
static void pgss_store(const char *query, uint64 queryId,
					   int query_location, int query_len,
					   pgssStoreKind kind,
					   double total_time, uint64 rows,
					   const BufferUsage *bufusage,
					   const WalUsage *walusage,
					   const struct JitInstrumentation *jitusage,
					   JumbleState *jstate);
static void pg_stat_statements_internal(FunctionCallInfo fcinfo,
										pgssVersion api_version,
										bool showtext);
static Size pgss_memsize(void);
static pgssEntry *entry_alloc(pgssHashKey *key, Size query_offset, int query_len,
							  int encoding, bool sticky);
static void entry_dealloc(void);
static bool qtext_store(const char *query, int query_len,
						Size *query_offset, int *gc_count);
static char *qtext_load_file(Size *buffer_size);
static char *qtext_fetch(Size query_offset, int query_len,
						 char *buffer, Size buffer_size);
static bool need_gc_qtexts(void);
static void gc_qtexts(void);
static void entry_reset(Oid userid, Oid dbid, uint64 queryid);
static char *generate_normalized_query(JumbleState *jstate, const char *query,
									   int query_loc, int *query_len_p);
static void fill_in_constant_lengths(JumbleState *jstate, const char *query,
									 int query_loc);
static int	comp_location(const void *a, const void *b);


/*
 * Module load callback
 */
void
_PG_init(void)
{
	/*
	 * In order to create our shared memory area, we have to be loaded via
	 * shared_preload_libraries.  If not, fall out without hooking into any of
	 * the main system.  (We don't throw error here because it seems useful to
	 * allow the pg_stat_statements functions to be created even when the
	 * module isn't active.  The functions must protect themselves against
	 * being called then, however.)
	 */
	if (!process_shared_preload_libraries_in_progress)
		return;

	/*
	 * Inform the postmaster that we want to enable query_id calculation if
	 * compute_query_id is set to auto.
	 */
	EnableQueryId();

	/*
	 * Define (or redefine) custom GUC variables.
	 */
	DefineCustomIntVariable("pg_stat_statements.max",
							"Sets the maximum number of statements tracked by pg_stat_statements.",
							NULL,
							&pgss_max,
							5000,
							100,
							INT_MAX / 2,
							PGC_POSTMASTER,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomEnumVariable("pg_stat_statements.track",
							 "Selects which statements are tracked by pg_stat_statements.",
							 NULL,
							 &pgss_track,
							 PGSS_TRACK_TOP,
							 track_options,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_stat_statements.track_utility",
							 "Selects whether utility commands are tracked by pg_stat_statements.",
							 NULL,
							 &pgss_track_utility,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_stat_statements.track_planning",
							 "Selects whether planning duration is tracked by pg_stat_statements.",
							 NULL,
							 &pgss_track_planning,
							 false,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("pg_stat_statements.save",
							 "Save pg_stat_statements statistics across server shutdowns.",
							 NULL,
							 &pgss_save,
							 true,
							 PGC_SIGHUP,
							 0,
							 NULL,
							 NULL,
							 NULL);

	MarkGUCPrefixReserved("pg_stat_statements");

	/*
	 * Install hooks.
	 */
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = pgss_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pgss_shmem_startup;
	prev_post_parse_analyze_hook = post_parse_analyze_hook;
	post_parse_analyze_hook = pgss_post_parse_analyze;
	prev_planner_hook = planner_hook;
	planner_hook = pgss_planner;
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = pgss_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = pgss_ExecutorRun;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = pgss_ExecutorFinish;
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = pgss_ExecutorEnd;
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = pgss_ProcessUtility;
}

/*
 * shmem_request hook: request additional shared resources.  We'll allocate or
 * attach to the shared resources in pgss_shmem_startup().
 */
static void
pgss_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(pgss_memsize());
	RequestNamedLWLockTranche("pg_stat_statements", 1);
}

/*
 * shmem_startup hook: allocate or attach to shared memory,
 * then load any pre-existing statistics from file.
 * Also create and load the query-texts file, which is expected to exist
 * (even if empty) while the module is enabled.
 */
static void
pgss_shmem_startup(void)
{
	bool		found;
	HASHCTL		info;
	FILE	   *file = NULL;
	FILE	   *qfile = NULL;
	uint32		header;
	int32		num;
	int32		pgver;
	int32		i;
	int			buffer_size;
	char	   *buffer = NULL;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	/* reset in case this is a restart within the postmaster */
	pgss = NULL;
	pgss_hash = NULL;

	/*
	 * Create or attach to the shared memory state, including hash table
	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	pgss = ShmemInitStruct("pg_stat_statements",
						   sizeof(pgssSharedState),
						   &found);

	if (!found)
	{
		/* First time through ... */
		pgss->lock = &(GetNamedLWLockTranche("pg_stat_statements"))->lock;
		pgss->cur_median_usage = ASSUMED_MEDIAN_INIT;
		pgss->mean_query_len = ASSUMED_LENGTH_INIT;
		SpinLockInit(&pgss->mutex);
		pgss->extent = 0;
		pgss->n_writers = 0;
		pgss->gc_count = 0;
		pgss->stats.dealloc = 0;
		pgss->stats.stats_reset = GetCurrentTimestamp();
	}

	info.keysize = sizeof(pgssHashKey);
	info.entrysize = sizeof(pgssEntry);
	pgss_hash = ShmemInitHash("pg_stat_statements hash",
							  pgss_max, pgss_max,
							  &info,
							  HASH_ELEM | HASH_BLOBS);

	LWLockRelease(AddinShmemInitLock);

	/*
	 * If we're in the postmaster (or a standalone backend...), set up a shmem
	 * exit hook to dump the statistics to disk.
	 */
	if (!IsUnderPostmaster)
		on_shmem_exit(pgss_shmem_shutdown, (Datum) 0);

	/*
	 * Done if some other process already completed our initialization.
	 */
	if (found)
		return;

	/*
	 * Note: we don't bother with locks here, because there should be no other
	 * processes running when this code is reached.
	 */

	/* Unlink query text file possibly left over from crash */
	unlink(PGSS_TEXT_FILE);

	/* Allocate new query text temp file */
	qfile = AllocateFile(PGSS_TEXT_FILE, PG_BINARY_W);
	if (qfile == NULL)
		goto write_error;

	/*
	 * If we were told not to load old statistics, we're done.  (Note we do
	 * not try to unlink any old dump file in this case.  This seems a bit
	 * questionable but it's the historical behavior.)
	 */
	if (!pgss_save)
	{
		FreeFile(qfile);
		return;
	}

	/*
	 * Attempt to load old statistics from the dump file.
	 */
	file = AllocateFile(PGSS_DUMP_FILE, PG_BINARY_R);
	if (file == NULL)
	{
		if (errno != ENOENT)
			goto read_error;
		/* No existing persisted stats file, so we're done */
		FreeFile(qfile);
		return;
	}

	buffer_size = 2048;
	buffer = (char *) palloc(buffer_size);

	if (fread(&header, sizeof(uint32), 1, file) != 1 ||
		fread(&pgver, sizeof(uint32), 1, file) != 1 ||
		fread(&num, sizeof(int32), 1, file) != 1)
		goto read_error;

	if (header != PGSS_FILE_HEADER ||
		pgver != PGSS_PG_MAJOR_VERSION)
		goto data_error;

	for (i = 0; i < num; i++)
	{
		pgssEntry	temp;
		pgssEntry  *entry;
		Size		query_offset;

		if (fread(&temp, sizeof(pgssEntry), 1, file) != 1)
			goto read_error;

		/* Encoding is the only field we can easily sanity-check */
		if (!PG_VALID_BE_ENCODING(temp.encoding))
			goto data_error;

		/* Resize buffer as needed */
		if (temp.query_len >= buffer_size)
		{
			buffer_size = Max(buffer_size * 2, temp.query_len + 1);
			buffer = repalloc(buffer, buffer_size);
		}

		if (fread(buffer, 1, temp.query_len + 1, file) != temp.query_len + 1)
			goto read_error;

		/* Should have a trailing null, but let's make sure */
		buffer[temp.query_len] = '\0';

		/* Skip loading "sticky" entries */
		if (IS_STICKY(temp.counters))
			continue;

		/* Store the query text */
		query_offset = pgss->extent;
		if (fwrite(buffer, 1, temp.query_len + 1, qfile) != temp.query_len + 1)
			goto write_error;
		pgss->extent += temp.query_len + 1;

		/* make the hashtable entry (discards old entries if too many) */
		entry = entry_alloc(&temp.key, query_offset, temp.query_len,
							temp.encoding,
							false);

		/* copy in the actual stats */
		entry->counters = temp.counters;
	}

	/* Read global statistics for pg_stat_statements */
	if (fread(&pgss->stats, sizeof(pgssGlobalStats), 1, file) != 1)
		goto read_error;

	pfree(buffer);
	FreeFile(file);
	FreeFile(qfile);

	/*
	 * Remove the persisted stats file so it's not included in
	 * backups/replication standbys, etc.  A new file will be written on next
	 * shutdown.
	 *
	 * Note: it's okay if the PGSS_TEXT_FILE is included in a basebackup,
	 * because we remove that file on startup; it acts inversely to
	 * PGSS_DUMP_FILE, in that it is only supposed to be around when the
	 * server is running, whereas PGSS_DUMP_FILE is only supposed to be around
	 * when the server is not running.  Leaving the file creates no danger of
	 * a newly restored database having a spurious record of execution costs,
	 * which is what we're really concerned about here.
	 */
	unlink(PGSS_DUMP_FILE);

	return;

read_error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not read file \"%s\": %m",
					PGSS_DUMP_FILE)));
	goto fail;
data_error:
	ereport(LOG,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("ignoring invalid data in file \"%s\"",
					PGSS_DUMP_FILE)));
	goto fail;
write_error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not write file \"%s\": %m",
					PGSS_TEXT_FILE)));
fail:
	if (buffer)
		pfree(buffer);
	if (file)
		FreeFile(file);
	if (qfile)
		FreeFile(qfile);
	/* If possible, throw away the bogus file; ignore any error */
	unlink(PGSS_DUMP_FILE);

	/*
	 * Don't unlink PGSS_TEXT_FILE here; it should always be around while the
	 * server is running with pg_stat_statements enabled
	 */
}

/*
 * shmem_shutdown hook: Dump statistics into file.
 *
 * Note: we don't bother with acquiring lock, because there should be no
 * other processes running when this is called.
 */
static void
pgss_shmem_shutdown(int code, Datum arg)
{
	FILE	   *file;
	char	   *qbuffer = NULL;
	Size		qbuffer_size = 0;
	HASH_SEQ_STATUS hash_seq;
	int32		num_entries;
	pgssEntry  *entry;

	/* Don't try to dump during a crash. */
	if (code)
		return;

	/* Safety check ... shouldn't get here unless shmem is set up. */
	if (!pgss || !pgss_hash)
		return;

	/* Don't dump if told not to. */
	if (!pgss_save)
		return;

	file = AllocateFile(PGSS_DUMP_FILE ".tmp", PG_BINARY_W);
	if (file == NULL)
		goto error;

	if (fwrite(&PGSS_FILE_HEADER, sizeof(uint32), 1, file) != 1)
		goto error;
	if (fwrite(&PGSS_PG_MAJOR_VERSION, sizeof(uint32), 1, file) != 1)
		goto error;
	num_entries = hash_get_num_entries(pgss_hash);
	if (fwrite(&num_entries, sizeof(int32), 1, file) != 1)
		goto error;

	qbuffer = qtext_load_file(&qbuffer_size);
	if (qbuffer == NULL)
		goto error;

	/*
	 * When serializing to disk, we store query texts immediately after their
	 * entry data.  Any orphaned query texts are thereby excluded.
	 */
	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		int			len = entry->query_len;
		char	   *qstr = qtext_fetch(entry->query_offset, len,
									   qbuffer, qbuffer_size);

		if (qstr == NULL)
			continue;			/* Ignore any entries with bogus texts */

		if (fwrite(entry, sizeof(pgssEntry), 1, file) != 1 ||
			fwrite(qstr, 1, len + 1, file) != len + 1)
		{
			/* note: we assume hash_seq_term won't change errno */
			hash_seq_term(&hash_seq);
			goto error;
		}
	}

	/* Dump global statistics for pg_stat_statements */
	if (fwrite(&pgss->stats, sizeof(pgssGlobalStats), 1, file) != 1)
		goto error;

	free(qbuffer);
	qbuffer = NULL;

	if (FreeFile(file))
	{
		file = NULL;
		goto error;
	}

	/*
	 * Rename file into place, so we atomically replace any old one.
	 */
	(void) durable_rename(PGSS_DUMP_FILE ".tmp", PGSS_DUMP_FILE, LOG);

	/* Unlink query-texts file; it's not needed while shutdown */
	unlink(PGSS_TEXT_FILE);

	return;

error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not write file \"%s\": %m",
					PGSS_DUMP_FILE ".tmp")));
	if (qbuffer)
		free(qbuffer);
	if (file)
		FreeFile(file);
	unlink(PGSS_DUMP_FILE ".tmp");
	unlink(PGSS_TEXT_FILE);
}

/*
 * Post-parse-analysis hook: mark query with a queryId
 */
static void
pgss_post_parse_analyze(ParseState *pstate, Query *query, JumbleState *jstate)
{
	if (prev_post_parse_analyze_hook)
		prev_post_parse_analyze_hook(pstate, query, jstate);

	/* Safety check... */
	if (!pgss || !pgss_hash || !pgss_enabled(exec_nested_level))
		return;

	/*
	 * Clear queryId for prepared statements related utility, as those will
	 * inherit from the underlying statement's one (except DEALLOCATE which is
	 * entirely untracked).
	 */
	if (query->utilityStmt)
	{
		if (pgss_track_utility && !PGSS_HANDLED_UTILITY(query->utilityStmt))
			query->queryId = UINT64CONST(0);
		return;
	}

	/*
	 * If query jumbling were able to identify any ignorable constants, we
	 * immediately create a hash table entry for the query, so that we can
	 * record the normalized form of the query string.  If there were no such
	 * constants, the normalized string would be the same as the query text
	 * anyway, so there's no need for an early entry.
	 */
	if (jstate && jstate->clocations_count > 0)
		pgss_store(pstate->p_sourcetext,
				   query->queryId,
				   query->stmt_location,
				   query->stmt_len,
				   PGSS_INVALID,
				   0,
				   0,
				   NULL,
				   NULL,
				   NULL,
				   jstate);
}

/*
 * Planner hook: forward to regular planner, but measure planning time
 * if needed.
 */
static PlannedStmt *
pgss_planner(Query *parse,
			 const char *query_string,
			 int cursorOptions,
			 ParamListInfo boundParams)
{
	PlannedStmt *result;

	/*
	 * We can't process the query if no query_string is provided, as
	 * pgss_store needs it.  We also ignore query without queryid, as it would
	 * be treated as a utility statement, which may not be the case.
	 *
	 * Note that planner_hook can be called from the planner itself, so we
	 * have a specific nesting level for the planner.  However, utility
	 * commands containing optimizable statements can also call the planner,
	 * same for regular DML (for instance for underlying foreign key queries).
	 * So testing the planner nesting level only is not enough to detect real
	 * top level planner call.
	 */
	if (pgss_enabled(plan_nested_level + exec_nested_level)
		&& pgss_track_planning && query_string
		&& parse->queryId != UINT64CONST(0))
	{
		instr_time	start;
		instr_time	duration;
		BufferUsage bufusage_start,
					bufusage;
		WalUsage	walusage_start,
					walusage;

		/* We need to track buffer usage as the planner can access them. */
		bufusage_start = pgBufferUsage;

		/*
		 * Similarly the planner could write some WAL records in some cases
		 * (e.g. setting a hint bit with those being WAL-logged)
		 */
		walusage_start = pgWalUsage;
		INSTR_TIME_SET_CURRENT(start);

		plan_nested_level++;
		PG_TRY();
		{
			if (prev_planner_hook)
				result = prev_planner_hook(parse, query_string, cursorOptions,
										   boundParams);
			else
				result = standard_planner(parse, query_string, cursorOptions,
										  boundParams);
		}
		PG_FINALLY();
		{
			plan_nested_level--;
		}
		PG_END_TRY();

		INSTR_TIME_SET_CURRENT(duration);
		INSTR_TIME_SUBTRACT(duration, start);

		/* calc differences of buffer counters. */
		memset(&bufusage, 0, sizeof(BufferUsage));
		BufferUsageAccumDiff(&bufusage, &pgBufferUsage, &bufusage_start);

		/* calc differences of WAL counters. */
		memset(&walusage, 0, sizeof(WalUsage));
		WalUsageAccumDiff(&walusage, &pgWalUsage, &walusage_start);

		pgss_store(query_string,
				   parse->queryId,
				   parse->stmt_location,
				   parse->stmt_len,
				   PGSS_PLAN,
				   INSTR_TIME_GET_MILLISEC(duration),
				   0,
				   &bufusage,
				   &walusage,
				   NULL,
				   NULL);
	}
	else
	{
		if (prev_planner_hook)
			result = prev_planner_hook(parse, query_string, cursorOptions,
									   boundParams);
		else
			result = standard_planner(parse, query_string, cursorOptions,
									  boundParams);
	}

	return result;
}

/*
 * ExecutorStart hook: start up tracking if needed
 */
static void
pgss_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	if (prev_ExecutorStart)
		prev_ExecutorStart(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

	/*
	 * If query has queryId zero, don't track it.  This prevents double
	 * counting of optimizable statements that are directly contained in
	 * utility statements.
	 */
	if (pgss_enabled(exec_nested_level) && queryDesc->plannedstmt->queryId != UINT64CONST(0))
	{
		/*
		 * Set up to track total elapsed time in ExecutorRun.  Make sure the
		 * space is allocated in the per-query context so it will go away at
		 * ExecutorEnd.
		 */
		if (queryDesc->totaltime == NULL)
		{
			MemoryContext oldcxt;

			oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
			queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL, false);
			MemoryContextSwitchTo(oldcxt);
		}
	}
}

/*
 * ExecutorRun hook: all we need do is track nesting depth
 */
static void
pgss_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count,
				 bool execute_once)
{
	exec_nested_level++;
	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count, execute_once);
		else
			standard_ExecutorRun(queryDesc, direction, count, execute_once);
	}
	PG_FINALLY();
	{
		exec_nested_level--;
	}
	PG_END_TRY();
}

/*
 * ExecutorFinish hook: all we need do is track nesting depth
 */
static void
pgss_ExecutorFinish(QueryDesc *queryDesc)
{
	exec_nested_level++;
	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
	}
	PG_FINALLY();
	{
		exec_nested_level--;
	}
	PG_END_TRY();
}

/*
 * ExecutorEnd hook: store results if needed
 */
static void
pgss_ExecutorEnd(QueryDesc *queryDesc)
{
	uint64		queryId = queryDesc->plannedstmt->queryId;

	if (queryId != UINT64CONST(0) && queryDesc->totaltime &&
		pgss_enabled(exec_nested_level))
	{
		/*
		 * Make sure stats accumulation is done.  (Note: it's okay if several
		 * levels of hook all do this.)
		 */
		InstrEndLoop(queryDesc->totaltime);

		pgss_store(queryDesc->sourceText,
				   queryId,
				   queryDesc->plannedstmt->stmt_location,
				   queryDesc->plannedstmt->stmt_len,
				   PGSS_EXEC,
				   queryDesc->totaltime->total * 1000.0,	/* convert to msec */
				   queryDesc->estate->es_processed,
				   &queryDesc->totaltime->bufusage,
				   &queryDesc->totaltime->walusage,
				   queryDesc->estate->es_jit ? &queryDesc->estate->es_jit->instr : NULL,
				   NULL);
	}

	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/*
 * ProcessUtility hook
 */
static void
pgss_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
					bool readOnlyTree,
					ProcessUtilityContext context,
					ParamListInfo params, QueryEnvironment *queryEnv,
					DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	uint64		saved_queryId = pstmt->queryId;

	/*
	 * Force utility statements to get queryId zero.  We do this even in cases
	 * where the statement contains an optimizable statement for which a
	 * queryId could be derived (such as EXPLAIN or DECLARE CURSOR).  For such
	 * cases, runtime control will first go through ProcessUtility and then
	 * the executor, and we don't want the executor hooks to do anything,
	 * since we are already measuring the statement's costs at the utility
	 * level.
	 *
	 * Note that this is only done if pg_stat_statements is enabled and
	 * configured to track utility statements, in the unlikely possibility
	 * that user configured another extension to handle utility statements
	 * only.
	 */
	if (pgss_enabled(exec_nested_level) && pgss_track_utility)
		pstmt->queryId = UINT64CONST(0);

	/*
	 * If it's an EXECUTE statement, we don't track it and don't increment the
	 * nesting level.  This allows the cycles to be charged to the underlying
	 * PREPARE instead (by the Executor hooks), which is much more useful.
	 *
	 * We also don't track execution of PREPARE.  If we did, we would get one
	 * hash table entry for the PREPARE (with hash calculated from the query
	 * string), and then a different one with the same query string (but hash
	 * calculated from the query tree) would be used to accumulate costs of
	 * ensuing EXECUTEs.  This would be confusing, and inconsistent with other
	 * cases where planning time is not included at all.
	 *
	 * Likewise, we don't track execution of DEALLOCATE.
	 */
	if (pgss_track_utility && pgss_enabled(exec_nested_level) &&
		PGSS_HANDLED_UTILITY(parsetree))
	{
		instr_time	start;
		instr_time	duration;
		uint64		rows;
		BufferUsage bufusage_start,
					bufusage;
		WalUsage	walusage_start,
					walusage;

		bufusage_start = pgBufferUsage;
		walusage_start = pgWalUsage;
		INSTR_TIME_SET_CURRENT(start);

		exec_nested_level++;
		PG_TRY();
		{
			if (prev_ProcessUtility)
				prev_ProcessUtility(pstmt, queryString, readOnlyTree,
									context, params, queryEnv,
									dest, qc);
			else
				standard_ProcessUtility(pstmt, queryString, readOnlyTree,
										context, params, queryEnv,
										dest, qc);
		}
		PG_FINALLY();
		{
			exec_nested_level--;
		}
		PG_END_TRY();

		INSTR_TIME_SET_CURRENT(duration);
		INSTR_TIME_SUBTRACT(duration, start);

		/*
		 * Track the total number of rows retrieved or affected by the utility
		 * statements of COPY, FETCH, CREATE TABLE AS, CREATE MATERIALIZED
		 * VIEW, REFRESH MATERIALIZED VIEW and SELECT INTO.
		 */
		rows = (qc && (qc->commandTag == CMDTAG_COPY ||
					   qc->commandTag == CMDTAG_FETCH ||
					   qc->commandTag == CMDTAG_SELECT ||
					   qc->commandTag == CMDTAG_REFRESH_MATERIALIZED_VIEW)) ?
			qc->nprocessed : 0;

		/* calc differences of buffer counters. */
		memset(&bufusage, 0, sizeof(BufferUsage));
		BufferUsageAccumDiff(&bufusage, &pgBufferUsage, &bufusage_start);

		/* calc differences of WAL counters. */
		memset(&walusage, 0, sizeof(WalUsage));
		WalUsageAccumDiff(&walusage, &pgWalUsage, &walusage_start);

		pgss_store(queryString,
				   saved_queryId,
				   pstmt->stmt_location,
				   pstmt->stmt_len,
				   PGSS_EXEC,
				   INSTR_TIME_GET_MILLISEC(duration),
				   rows,
				   &bufusage,
				   &walusage,
				   NULL,
				   NULL);
	}
	else
	{
		if (prev_ProcessUtility)
			prev_ProcessUtility(pstmt, queryString, readOnlyTree,
								context, params, queryEnv,
								dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree,
									context, params, queryEnv,
									dest, qc);
	}
}

/*
 * Store some statistics for a statement.
 *
 * If jstate is not NULL then we're trying to create an entry for which
 * we have no statistics as yet; we just want to record the normalized
 * query string.  total_time, rows, bufusage and walusage are ignored in this
 * case.
 *
 * If kind is PGSS_PLAN or PGSS_EXEC, its value is used as the array position
 * for the arrays in the Counters field.
 */
static void
pgss_store(const char *query, uint64 queryId,
		   int query_location, int query_len,
		   pgssStoreKind kind,
		   double total_time, uint64 rows,
		   const BufferUsage *bufusage,
		   const WalUsage *walusage,
		   const struct JitInstrumentation *jitusage,
		   JumbleState *jstate)
{
	pgssHashKey key;
	pgssEntry  *entry;
	char	   *norm_query = NULL;
	int			encoding = GetDatabaseEncoding();

	Assert(query != NULL);

	/* Safety check... */
	if (!pgss || !pgss_hash)
		return;

	/*
	 * Nothing to do if compute_query_id isn't enabled and no other module
	 * computed a query identifier.
	 */
	if (queryId == UINT64CONST(0))
		return;

	/*
	 * Confine our attention to the relevant part of the string, if the query
	 * is a portion of a multi-statement source string, and update query
	 * location and length if needed.
	 */
	query = CleanQuerytext(query, &query_location, &query_len);

	/* Set up key for hashtable search */

	/* memset() is required when pgssHashKey is without padding only */
	memset(&key, 0, sizeof(pgssHashKey));

	key.userid = GetUserId();
	key.dbid = MyDatabaseId;
	key.queryid = queryId;
	key.toplevel = (exec_nested_level == 0);

	/* Lookup the hash table entry with shared lock. */
	LWLockAcquire(pgss->lock, LW_SHARED);

	entry = (pgssEntry *) hash_search(pgss_hash, &key, HASH_FIND, NULL);

	/* Create new entry, if not present */
	if (!entry)
	{
		Size		query_offset;
		int			gc_count;
		bool		stored;
		bool		do_gc;

		/*
		 * Create a new, normalized query string if caller asked.  We don't
		 * need to hold the lock while doing this work.  (Note: in any case,
		 * it's possible that someone else creates a duplicate hashtable entry
		 * in the interval where we don't hold the lock below.  That case is
		 * handled by entry_alloc.)
		 */
		if (jstate)
		{
			LWLockRelease(pgss->lock);
			norm_query = generate_normalized_query(jstate, query,
												   query_location,
												   &query_len);
			LWLockAcquire(pgss->lock, LW_SHARED);
		}

		/* Append new query text to file with only shared lock held */
		stored = qtext_store(norm_query ? norm_query : query, query_len,
							 &query_offset, &gc_count);

		/*
		 * Determine whether we need to garbage collect external query texts
		 * while the shared lock is still held.  This micro-optimization
		 * avoids taking the time to decide this while holding exclusive lock.
		 */
		do_gc = need_gc_qtexts();

		/* Need exclusive lock to make a new hashtable entry - promote */
		LWLockRelease(pgss->lock);
		LWLockAcquire(pgss->lock, LW_EXCLUSIVE);

		/*
		 * A garbage collection may have occurred while we weren't holding the
		 * lock.  In the unlikely event that this happens, the query text we
		 * stored above will have been garbage collected, so write it again.
		 * This should be infrequent enough that doing it while holding
		 * exclusive lock isn't a performance problem.
		 */
		if (!stored || pgss->gc_count != gc_count)
			stored = qtext_store(norm_query ? norm_query : query, query_len,
								 &query_offset, NULL);

		/* If we failed to write to the text file, give up */
		if (!stored)
			goto done;

		/* OK to create a new hashtable entry */
		entry = entry_alloc(&key, query_offset, query_len, encoding,
							jstate != NULL);

		/* If needed, perform garbage collection while exclusive lock held */
		if (do_gc)
			gc_qtexts();
	}

	/* Increment the counts, except when jstate is not NULL */
	if (!jstate)
	{
		/*
		 * Grab the spinlock while updating the counters (see comment about
		 * locking rules at the head of the file)
		 */
		volatile pgssEntry *e = (volatile pgssEntry *) entry;

		Assert(kind == PGSS_PLAN || kind == PGSS_EXEC);

		SpinLockAcquire(&e->mutex);

		/* "Unstick" entry if it was previously sticky */
		if (IS_STICKY(e->counters))
			e->counters.usage = USAGE_INIT;

		e->counters.calls[kind] += 1;
		e->counters.total_time[kind] += total_time;

		if (e->counters.calls[kind] == 1)
		{
			e->counters.min_time[kind] = total_time;
			e->counters.max_time[kind] = total_time;
			e->counters.mean_time[kind] = total_time;
		}
		else
		{
			/*
			 * Welford's method for accurately computing variance. See
			 * <http://www.johndcook.com/blog/standard_deviation/>
			 */
			double		old_mean = e->counters.mean_time[kind];

			e->counters.mean_time[kind] +=
				(total_time - old_mean) / e->counters.calls[kind];
			e->counters.sum_var_time[kind] +=
				(total_time - old_mean) * (total_time - e->counters.mean_time[kind]);

			/* calculate min and max time */
			if (e->counters.min_time[kind] > total_time)
				e->counters.min_time[kind] = total_time;
			if (e->counters.max_time[kind] < total_time)
				e->counters.max_time[kind] = total_time;
		}
		e->counters.rows += rows;
		e->counters.shared_blks_hit += bufusage->shared_blks_hit;
		e->counters.shared_blks_read += bufusage->shared_blks_read;
		e->counters.shared_blks_dirtied += bufusage->shared_blks_dirtied;
		e->counters.shared_blks_written += bufusage->shared_blks_written;
		e->counters.local_blks_hit += bufusage->local_blks_hit;
		e->counters.local_blks_read += bufusage->local_blks_read;
		e->counters.local_blks_dirtied += bufusage->local_blks_dirtied;
		e->counters.local_blks_written += bufusage->local_blks_written;
		e->counters.temp_blks_read += bufusage->temp_blks_read;
		e->counters.temp_blks_written += bufusage->temp_blks_written;
		e->counters.blk_read_time += INSTR_TIME_GET_MILLISEC(bufusage->blk_read_time);
		e->counters.blk_write_time += INSTR_TIME_GET_MILLISEC(bufusage->blk_write_time);
		e->counters.temp_blk_read_time += INSTR_TIME_GET_MILLISEC(bufusage->temp_blk_read_time);
		e->counters.temp_blk_write_time += INSTR_TIME_GET_MILLISEC(bufusage->temp_blk_write_time);
		e->counters.usage += USAGE_EXEC(total_time);
		e->counters.wal_records += walusage->wal_records;
		e->counters.wal_fpi += walusage->wal_fpi;
		e->counters.wal_bytes += walusage->wal_bytes;
		if (jitusage)
		{
			e->counters.jit_functions += jitusage->created_functions;
			e->counters.jit_generation_time += INSTR_TIME_GET_MILLISEC(jitusage->generation_counter);

			if (INSTR_TIME_GET_MILLISEC(jitusage->inlining_counter))
				e->counters.jit_inlining_count++;
			e->counters.jit_inlining_time += INSTR_TIME_GET_MILLISEC(jitusage->inlining_counter);

			if (INSTR_TIME_GET_MILLISEC(jitusage->optimization_counter))
				e->counters.jit_optimization_count++;
			e->counters.jit_optimization_time += INSTR_TIME_GET_MILLISEC(jitusage->optimization_counter);

			if (INSTR_TIME_GET_MILLISEC(jitusage->emission_counter))
				e->counters.jit_emission_count++;
			e->counters.jit_emission_time += INSTR_TIME_GET_MILLISEC(jitusage->emission_counter);
		}

		SpinLockRelease(&e->mutex);
	}

done:
	LWLockRelease(pgss->lock);

	/* We postpone this clean-up until we're out of the lock */
	if (norm_query)
		pfree(norm_query);
}

/*
 * Reset statement statistics corresponding to userid, dbid, and queryid.
 */
Datum
pg_stat_statements_reset_1_7(PG_FUNCTION_ARGS)
{
	Oid			userid;
	Oid			dbid;
	uint64		queryid;

	userid = PG_GETARG_OID(0);
	dbid = PG_GETARG_OID(1);
	queryid = (uint64) PG_GETARG_INT64(2);

	entry_reset(userid, dbid, queryid);

	PG_RETURN_VOID();
}

/*
 * Reset statement statistics.
 */
Datum
pg_stat_statements_reset(PG_FUNCTION_ARGS)
{
	entry_reset(0, 0, 0);

	PG_RETURN_VOID();
}

/* Number of output arguments (columns) for various API versions */
#define PG_STAT_STATEMENTS_COLS_V1_0	14
#define PG_STAT_STATEMENTS_COLS_V1_1	18
#define PG_STAT_STATEMENTS_COLS_V1_2	19
#define PG_STAT_STATEMENTS_COLS_V1_3	23
#define PG_STAT_STATEMENTS_COLS_V1_8	32
#define PG_STAT_STATEMENTS_COLS_V1_9	33
#define PG_STAT_STATEMENTS_COLS_V1_10	43
#define PG_STAT_STATEMENTS_COLS			43	/* maximum of above */

/*
 * Retrieve statement statistics.
 *
 * The SQL API of this function has changed multiple times, and will likely
 * do so again in future.  To support the case where a newer version of this
 * loadable module is being used with an old SQL declaration of the function,
 * we continue to support the older API versions.  For 1.2 and later, the
 * expected API version is identified by embedding it in the C name of the
 * function.  Unfortunately we weren't bright enough to do that for 1.1.
 */
Datum
pg_stat_statements_1_10(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_10, showtext);

	return (Datum) 0;
}

Datum
pg_stat_statements_1_9(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_9, showtext);

	return (Datum) 0;
}

Datum
pg_stat_statements_1_8(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_8, showtext);

	return (Datum) 0;
}

Datum
pg_stat_statements_1_3(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_3, showtext);

	return (Datum) 0;
}

Datum
pg_stat_statements_1_2(PG_FUNCTION_ARGS)
{
	bool		showtext = PG_GETARG_BOOL(0);

	pg_stat_statements_internal(fcinfo, PGSS_V1_2, showtext);

	return (Datum) 0;
}

/*
 * Legacy entry point for pg_stat_statements() API versions 1.0 and 1.1.
 * This can be removed someday, perhaps.
 */
Datum
pg_stat_statements(PG_FUNCTION_ARGS)
{
	/* If it's really API 1.1, we'll figure that out below */
	pg_stat_statements_internal(fcinfo, PGSS_V1_0, true);

	return (Datum) 0;
}

/* Common code for all versions of pg_stat_statements() */
static void
pg_stat_statements_internal(FunctionCallInfo fcinfo,
							pgssVersion api_version,
							bool showtext)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			userid = GetUserId();
	bool		is_allowed_role = false;
	char	   *qbuffer = NULL;
	Size		qbuffer_size = 0;
	Size		extent = 0;
	int			gc_count = 0;
	HASH_SEQ_STATUS hash_seq;
	pgssEntry  *entry;

	/*
	 * Superusers or roles with the privileges of pg_read_all_stats members
	 * are allowed
	 */
	is_allowed_role = has_privs_of_role(userid, ROLE_PG_READ_ALL_STATS);

	/* hash table must exist already */
	if (!pgss || !pgss_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_statements must be loaded via shared_preload_libraries")));

	SetSingleFuncCall(fcinfo, 0);

	/*
	 * Check we have the expected number of output arguments.  Aside from
	 * being a good safety check, we need a kluge here to detect API version
	 * 1.1, which was wedged into the code in an ill-considered way.
	 */
	switch (rsinfo->setDesc->natts)
	{
		case PG_STAT_STATEMENTS_COLS_V1_0:
			if (api_version != PGSS_V1_0)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_1:
			/* pg_stat_statements() should have told us 1.0 */
			if (api_version != PGSS_V1_0)
				elog(ERROR, "incorrect number of output arguments");
			api_version = PGSS_V1_1;
			break;
		case PG_STAT_STATEMENTS_COLS_V1_2:
			if (api_version != PGSS_V1_2)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_3:
			if (api_version != PGSS_V1_3)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_8:
			if (api_version != PGSS_V1_8)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_9:
			if (api_version != PGSS_V1_9)
				elog(ERROR, "incorrect number of output arguments");
			break;
		case PG_STAT_STATEMENTS_COLS_V1_10:
			if (api_version != PGSS_V1_10)
				elog(ERROR, "incorrect number of output arguments");
			break;
		default:
			elog(ERROR, "incorrect number of output arguments");
	}

	/*
	 * We'd like to load the query text file (if needed) while not holding any
	 * lock on pgss->lock.  In the worst case we'll have to do this again
	 * after we have the lock, but it's unlikely enough to make this a win
	 * despite occasional duplicated work.  We need to reload if anybody
	 * writes to the file (either a retail qtext_store(), or a garbage
	 * collection) between this point and where we've gotten shared lock.  If
	 * a qtext_store is actually in progress when we look, we might as well
	 * skip the speculative load entirely.
	 */
	if (showtext)
	{
		int			n_writers;

		/* Take the mutex so we can examine variables */
		{
			volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;

			SpinLockAcquire(&s->mutex);
			extent = s->extent;
			n_writers = s->n_writers;
			gc_count = s->gc_count;
			SpinLockRelease(&s->mutex);
		}

		/* No point in loading file now if there are active writers */
		if (n_writers == 0)
			qbuffer = qtext_load_file(&qbuffer_size);
	}

	/*
	 * Get shared lock, load or reload the query text file if we must, and
	 * iterate over the hashtable entries.
	 *
	 * With a large hash table, we might be holding the lock rather longer
	 * than one could wish.  However, this only blocks creation of new hash
	 * table entries, and the larger the hash table the less likely that is to
	 * be needed.  So we can hope this is okay.  Perhaps someday we'll decide
	 * we need to partition the hash table to limit the time spent holding any
	 * one lock.
	 */
	LWLockAcquire(pgss->lock, LW_SHARED);

	if (showtext)
	{
		/*
		 * Here it is safe to examine extent and gc_count without taking the
		 * mutex.  Note that although other processes might change
		 * pgss->extent just after we look at it, the strings they then write
		 * into the file cannot yet be referenced in the hashtable, so we
		 * don't care whether we see them or not.
		 *
		 * If qtext_load_file fails, we just press on; we'll return NULL for
		 * every query text.
		 */
		if (qbuffer == NULL ||
			pgss->extent != extent ||
			pgss->gc_count != gc_count)
		{
			if (qbuffer)
				free(qbuffer);
			qbuffer = qtext_load_file(&qbuffer_size);
		}
	}

	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		Datum		values[PG_STAT_STATEMENTS_COLS];
		bool		nulls[PG_STAT_STATEMENTS_COLS];
		int			i = 0;
		Counters	tmp;
		double		stddev;
		int64		queryid = entry->key.queryid;

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		values[i++] = ObjectIdGetDatum(entry->key.userid);
		values[i++] = ObjectIdGetDatum(entry->key.dbid);
		if (api_version >= PGSS_V1_9)
			values[i++] = BoolGetDatum(entry->key.toplevel);

		if (is_allowed_role || entry->key.userid == userid)
		{
			if (api_version >= PGSS_V1_2)
				values[i++] = Int64GetDatumFast(queryid);

			if (showtext)
			{
				char	   *qstr = qtext_fetch(entry->query_offset,
											   entry->query_len,
											   qbuffer,
											   qbuffer_size);

				if (qstr)
				{
					char	   *enc;

					enc = pg_any_to_server(qstr,
										   entry->query_len,
										   entry->encoding);

					values[i++] = CStringGetTextDatum(enc);

					if (enc != qstr)
						pfree(enc);
				}
				else
				{
					/* Just return a null if we fail to find the text */
					nulls[i++] = true;
				}
			}
			else
			{
				/* Query text not requested */
				nulls[i++] = true;
			}
		}
		else
		{
			/* Don't show queryid */
			if (api_version >= PGSS_V1_2)
				nulls[i++] = true;

			/*
			 * Don't show query text, but hint as to the reason for not doing
			 * so if it was requested
			 */
			if (showtext)
				values[i++] = CStringGetTextDatum("<insufficient privilege>");
			else
				nulls[i++] = true;
		}

		/* copy counters to a local variable to keep locking time short */
		{
			volatile pgssEntry *e = (volatile pgssEntry *) entry;

			SpinLockAcquire(&e->mutex);
			tmp = e->counters;
			SpinLockRelease(&e->mutex);
		}

		/* Skip entry if unexecuted (ie, it's a pending "sticky" entry) */
		if (IS_STICKY(tmp))
			continue;

		/* Note that we rely on PGSS_PLAN being 0 and PGSS_EXEC being 1. */
		for (int kind = 0; kind < PGSS_NUMKIND; kind++)
		{
			if (kind == PGSS_EXEC || api_version >= PGSS_V1_8)
			{
				values[i++] = Int64GetDatumFast(tmp.calls[kind]);
				values[i++] = Float8GetDatumFast(tmp.total_time[kind]);
			}

			if ((kind == PGSS_EXEC && api_version >= PGSS_V1_3) ||
				api_version >= PGSS_V1_8)
			{
				values[i++] = Float8GetDatumFast(tmp.min_time[kind]);
				values[i++] = Float8GetDatumFast(tmp.max_time[kind]);
				values[i++] = Float8GetDatumFast(tmp.mean_time[kind]);

				/*
				 * Note we are calculating the population variance here, not
				 * the sample variance, as we have data for the whole
				 * population, so Bessel's correction is not used, and we
				 * don't divide by tmp.calls - 1.
				 */
				if (tmp.calls[kind] > 1)
					stddev = sqrt(tmp.sum_var_time[kind] / tmp.calls[kind]);
				else
					stddev = 0.0;
				values[i++] = Float8GetDatumFast(stddev);
			}
		}
		values[i++] = Int64GetDatumFast(tmp.rows);
		values[i++] = Int64GetDatumFast(tmp.shared_blks_hit);
		values[i++] = Int64GetDatumFast(tmp.shared_blks_read);
		if (api_version >= PGSS_V1_1)
			values[i++] = Int64GetDatumFast(tmp.shared_blks_dirtied);
		values[i++] = Int64GetDatumFast(tmp.shared_blks_written);
		values[i++] = Int64GetDatumFast(tmp.local_blks_hit);
		values[i++] = Int64GetDatumFast(tmp.local_blks_read);
		if (api_version >= PGSS_V1_1)
			values[i++] = Int64GetDatumFast(tmp.local_blks_dirtied);
		values[i++] = Int64GetDatumFast(tmp.local_blks_written);
		values[i++] = Int64GetDatumFast(tmp.temp_blks_read);
		values[i++] = Int64GetDatumFast(tmp.temp_blks_written);
		if (api_version >= PGSS_V1_1)
		{
			values[i++] = Float8GetDatumFast(tmp.blk_read_time);
			values[i++] = Float8GetDatumFast(tmp.blk_write_time);
		}
		if (api_version >= PGSS_V1_10)
		{
			values[i++] = Float8GetDatumFast(tmp.temp_blk_read_time);
			values[i++] = Float8GetDatumFast(tmp.temp_blk_write_time);
		}
		if (api_version >= PGSS_V1_8)
		{
			char		buf[256];
			Datum		wal_bytes;

			values[i++] = Int64GetDatumFast(tmp.wal_records);
			values[i++] = Int64GetDatumFast(tmp.wal_fpi);

			snprintf(buf, sizeof buf, UINT64_FORMAT, tmp.wal_bytes);

			/* Convert to numeric. */
			wal_bytes = DirectFunctionCall3(numeric_in,
											CStringGetDatum(buf),
											ObjectIdGetDatum(0),
											Int32GetDatum(-1));
			values[i++] = wal_bytes;
		}
		if (api_version >= PGSS_V1_10)
		{
			values[i++] = Int64GetDatumFast(tmp.jit_functions);
			values[i++] = Float8GetDatumFast(tmp.jit_generation_time);
			values[i++] = Int64GetDatumFast(tmp.jit_inlining_count);
			values[i++] = Float8GetDatumFast(tmp.jit_inlining_time);
			values[i++] = Int64GetDatumFast(tmp.jit_optimization_count);
			values[i++] = Float8GetDatumFast(tmp.jit_optimization_time);
			values[i++] = Int64GetDatumFast(tmp.jit_emission_count);
			values[i++] = Float8GetDatumFast(tmp.jit_emission_time);
		}

		Assert(i == (api_version == PGSS_V1_0 ? PG_STAT_STATEMENTS_COLS_V1_0 :
					 api_version == PGSS_V1_1 ? PG_STAT_STATEMENTS_COLS_V1_1 :
					 api_version == PGSS_V1_2 ? PG_STAT_STATEMENTS_COLS_V1_2 :
					 api_version == PGSS_V1_3 ? PG_STAT_STATEMENTS_COLS_V1_3 :
					 api_version == PGSS_V1_8 ? PG_STAT_STATEMENTS_COLS_V1_8 :
					 api_version == PGSS_V1_9 ? PG_STAT_STATEMENTS_COLS_V1_9 :
					 api_version == PGSS_V1_10 ? PG_STAT_STATEMENTS_COLS_V1_10 :
					 -1 /* fail if you forget to update this assert */ ));

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	LWLockRelease(pgss->lock);

	if (qbuffer)
		free(qbuffer);
}

/* Number of output arguments (columns) for pg_stat_statements_info */
#define PG_STAT_STATEMENTS_INFO_COLS	2

/*
 * Return statistics of pg_stat_statements.
 */
Datum
pg_stat_statements_info(PG_FUNCTION_ARGS)
{
	pgssGlobalStats stats;
	TupleDesc	tupdesc;
	Datum		values[PG_STAT_STATEMENTS_INFO_COLS];
	bool		nulls[PG_STAT_STATEMENTS_INFO_COLS];

	if (!pgss || !pgss_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_statements must be loaded via shared_preload_libraries")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	MemSet(values, 0, sizeof(values));
	MemSet(nulls, 0, sizeof(nulls));

	/* Read global statistics for pg_stat_statements */
	{
		volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;

		SpinLockAcquire(&s->mutex);
		stats = s->stats;
		SpinLockRelease(&s->mutex);
	}

	values[0] = Int64GetDatum(stats.dealloc);
	values[1] = TimestampTzGetDatum(stats.stats_reset);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * Estimate shared memory space needed.
 */
static Size
pgss_memsize(void)
{
	Size		size;

	size = MAXALIGN(sizeof(pgssSharedState));
	size = add_size(size, hash_estimate_size(pgss_max, sizeof(pgssEntry)));

	return size;
}

/*
 * Allocate a new hashtable entry.
 * caller must hold an exclusive lock on pgss->lock
 *
 * "query" need not be null-terminated; we rely on query_len instead
 *
 * If "sticky" is true, make the new entry artificially sticky so that it will
 * probably still be there when the query finishes execution.  We do this by
 * giving it a median usage value rather than the normal value.  (Strictly
 * speaking, query strings are normalized on a best effort basis, though it
 * would be difficult to demonstrate this even under artificial conditions.)
 *
 * Note: despite needing exclusive lock, it's not an error for the target
 * entry to already exist.  This is because pgss_store releases and
 * reacquires lock after failing to find a match; so someone else could
 * have made the entry while we waited to get exclusive lock.
 */
static pgssEntry *
entry_alloc(pgssHashKey *key, Size query_offset, int query_len, int encoding,
			bool sticky)
{
	pgssEntry  *entry;
	bool		found;

	/* Make space if needed */
	while (hash_get_num_entries(pgss_hash) >= pgss_max)
		entry_dealloc();

	/* Find or create an entry with desired hash code */
	entry = (pgssEntry *) hash_search(pgss_hash, key, HASH_ENTER, &found);

	if (!found)
	{
		/* New entry, initialize it */

		/* reset the statistics */
		memset(&entry->counters, 0, sizeof(Counters));
		/* set the appropriate initial usage count */
		entry->counters.usage = sticky ? pgss->cur_median_usage : USAGE_INIT;
		/* re-initialize the mutex each time ... we assume no one using it */
		SpinLockInit(&entry->mutex);
		/* ... and don't forget the query text metadata */
		Assert(query_len >= 0);
		entry->query_offset = query_offset;
		entry->query_len = query_len;
		entry->encoding = encoding;
	}

	return entry;
}

/*
 * qsort comparator for sorting into increasing usage order
 */
static int
entry_cmp(const void *lhs, const void *rhs)
{
	double		l_usage = (*(pgssEntry *const *) lhs)->counters.usage;
	double		r_usage = (*(pgssEntry *const *) rhs)->counters.usage;

	if (l_usage < r_usage)
		return -1;
	else if (l_usage > r_usage)
		return +1;
	else
		return 0;
}

/*
 * Deallocate least-used entries.
 *
 * Caller must hold an exclusive lock on pgss->lock.
 */
static void
entry_dealloc(void)
{
	HASH_SEQ_STATUS hash_seq;
	pgssEntry **entries;
	pgssEntry  *entry;
	int			nvictims;
	int			i;
	Size		tottextlen;
	int			nvalidtexts;

	/*
	 * Sort entries by usage and deallocate USAGE_DEALLOC_PERCENT of them.
	 * While we're scanning the table, apply the decay factor to the usage
	 * values, and update the mean query length.
	 *
	 * Note that the mean query length is almost immediately obsolete, since
	 * we compute it before not after discarding the least-used entries.
	 * Hopefully, that doesn't affect the mean too much; it doesn't seem worth
	 * making two passes to get a more current result.  Likewise, the new
	 * cur_median_usage includes the entries we're about to zap.
	 */

	entries = palloc(hash_get_num_entries(pgss_hash) * sizeof(pgssEntry *));

	i = 0;
	tottextlen = 0;
	nvalidtexts = 0;

	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		entries[i++] = entry;
		/* "Sticky" entries get a different usage decay rate. */
		if (IS_STICKY(entry->counters))
			entry->counters.usage *= STICKY_DECREASE_FACTOR;
		else
			entry->counters.usage *= USAGE_DECREASE_FACTOR;
		/* In the mean length computation, ignore dropped texts. */
		if (entry->query_len >= 0)
		{
			tottextlen += entry->query_len + 1;
			nvalidtexts++;
		}
	}

	/* Sort into increasing order by usage */
	qsort(entries, i, sizeof(pgssEntry *), entry_cmp);

	/* Record the (approximate) median usage */
	if (i > 0)
		pgss->cur_median_usage = entries[i / 2]->counters.usage;
	/* Record the mean query length */
	if (nvalidtexts > 0)
		pgss->mean_query_len = tottextlen / nvalidtexts;
	else
		pgss->mean_query_len = ASSUMED_LENGTH_INIT;

	/* Now zap an appropriate fraction of lowest-usage entries */
	nvictims = Max(10, i * USAGE_DEALLOC_PERCENT / 100);
	nvictims = Min(nvictims, i);

	for (i = 0; i < nvictims; i++)
	{
		hash_search(pgss_hash, &entries[i]->key, HASH_REMOVE, NULL);
	}

	pfree(entries);

	/* Increment the number of times entries are deallocated */
	{
		volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;

		SpinLockAcquire(&s->mutex);
		s->stats.dealloc += 1;
		SpinLockRelease(&s->mutex);
	}
}

/*
 * Given a query string (not necessarily null-terminated), allocate a new
 * entry in the external query text file and store the string there.
 *
 * If successful, returns true, and stores the new entry's offset in the file
 * into *query_offset.  Also, if gc_count isn't NULL, *gc_count is set to the
 * number of garbage collections that have occurred so far.
 *
 * On failure, returns false.
 *
 * At least a shared lock on pgss->lock must be held by the caller, so as
 * to prevent a concurrent garbage collection.  Share-lock-holding callers
 * should pass a gc_count pointer to obtain the number of garbage collections,
 * so that they can recheck the count after obtaining exclusive lock to
 * detect whether a garbage collection occurred (and removed this entry).
 */
static bool
qtext_store(const char *query, int query_len,
			Size *query_offset, int *gc_count)
{
	Size		off;
	int			fd;

	/*
	 * We use a spinlock to protect extent/n_writers/gc_count, so that
	 * multiple processes may execute this function concurrently.
	 */
	{
		volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;

		SpinLockAcquire(&s->mutex);
		off = s->extent;
		s->extent += query_len + 1;
		s->n_writers++;
		if (gc_count)
			*gc_count = s->gc_count;
		SpinLockRelease(&s->mutex);
	}

	*query_offset = off;

	/*
	 * Don't allow the file to grow larger than what qtext_load_file can
	 * (theoretically) handle.  This has been seen to be reachable on 32-bit
	 * platforms.
	 */
	if (unlikely(query_len >= MaxAllocHugeSize - off))
	{
		errno = EFBIG;			/* not quite right, but it'll do */
		fd = -1;
		goto error;
	}

	/* Now write the data into the successfully-reserved part of the file */
	fd = OpenTransientFile(PGSS_TEXT_FILE, O_RDWR | O_CREAT | PG_BINARY);
	if (fd < 0)
		goto error;

	if (pg_pwrite(fd, query, query_len, off) != query_len)
		goto error;
	if (pg_pwrite(fd, "\0", 1, off + query_len) != 1)
		goto error;

	CloseTransientFile(fd);

	/* Mark our write complete */
	{
		volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;

		SpinLockAcquire(&s->mutex);
		s->n_writers--;
		SpinLockRelease(&s->mutex);
	}

	return true;

error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not write file \"%s\": %m",
					PGSS_TEXT_FILE)));

	if (fd >= 0)
		CloseTransientFile(fd);

	/* Mark our write complete */
	{
		volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;

		SpinLockAcquire(&s->mutex);
		s->n_writers--;
		SpinLockRelease(&s->mutex);
	}

	return false;
}

/*
 * Read the external query text file into a malloc'd buffer.
 *
 * Returns NULL (without throwing an error) if unable to read, eg
 * file not there or insufficient memory.
 *
 * On success, the buffer size is also returned into *buffer_size.
 *
 * This can be called without any lock on pgss->lock, but in that case
 * the caller is responsible for verifying that the result is sane.
 */
static char *
qtext_load_file(Size *buffer_size)
{
	char	   *buf;
	int			fd;
	struct stat stat;
	Size		nread;

	fd = OpenTransientFile(PGSS_TEXT_FILE, O_RDONLY | PG_BINARY);
	if (fd < 0)
	{
		if (errno != ENOENT)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m",
							PGSS_TEXT_FILE)));
		return NULL;
	}

	/* Get file length */
	if (fstat(fd, &stat))
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m",
						PGSS_TEXT_FILE)));
		CloseTransientFile(fd);
		return NULL;
	}

	/* Allocate buffer; beware that off_t might be wider than size_t */
	if (stat.st_size <= MaxAllocHugeSize)
		buf = (char *) malloc(stat.st_size);
	else
		buf = NULL;
	if (buf == NULL)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Could not allocate enough memory to read file \"%s\".",
						   PGSS_TEXT_FILE)));
		CloseTransientFile(fd);
		return NULL;
	}

	/*
	 * OK, slurp in the file.  Windows fails if we try to read more than
	 * INT_MAX bytes at once, and other platforms might not like that either,
	 * so read a very large file in 1GB segments.
	 */
	nread = 0;
	while (nread < stat.st_size)
	{
		int			toread = Min(1024 * 1024 * 1024, stat.st_size - nread);

		/*
		 * If we get a short read and errno doesn't get set, the reason is
		 * probably that garbage collection truncated the file since we did
		 * the fstat(), so we don't log a complaint --- but we don't return
		 * the data, either, since it's most likely corrupt due to concurrent
		 * writes from garbage collection.
		 */
		errno = 0;
		if (read(fd, buf + nread, toread) != toread)
		{
			if (errno)
				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("could not read file \"%s\": %m",
								PGSS_TEXT_FILE)));
			free(buf);
			CloseTransientFile(fd);
			return NULL;
		}
		nread += toread;
	}

	if (CloseTransientFile(fd) != 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", PGSS_TEXT_FILE)));

	*buffer_size = nread;
	return buf;
}

/*
 * Locate a query text in the file image previously read by qtext_load_file().
 *
 * We validate the given offset/length, and return NULL if bogus.  Otherwise,
 * the result points to a null-terminated string within the buffer.
 */
static char *
qtext_fetch(Size query_offset, int query_len,
			char *buffer, Size buffer_size)
{
	/* File read failed? */
	if (buffer == NULL)
		return NULL;
	/* Bogus offset/length? */
	if (query_len < 0 ||
		query_offset + query_len >= buffer_size)
		return NULL;
	/* As a further sanity check, make sure there's a trailing null */
	if (buffer[query_offset + query_len] != '\0')
		return NULL;
	/* Looks OK */
	return buffer + query_offset;
}

/*
 * Do we need to garbage-collect the external query text file?
 *
 * Caller should hold at least a shared lock on pgss->lock.
 */
static bool
need_gc_qtexts(void)
{
	Size		extent;

	/* Read shared extent pointer */
	{
		volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;

		SpinLockAcquire(&s->mutex);
		extent = s->extent;
		SpinLockRelease(&s->mutex);
	}

	/*
	 * Don't proceed if file does not exceed 512 bytes per possible entry.
	 *
	 * Here and in the next test, 32-bit machines have overflow hazards if
	 * pgss_max and/or mean_query_len are large.  Force the multiplications
	 * and comparisons to be done in uint64 arithmetic to forestall trouble.
	 */
	if ((uint64) extent < (uint64) 512 * pgss_max)
		return false;

	/*
	 * Don't proceed if file is less than about 50% bloat.  Nothing can or
	 * should be done in the event of unusually large query texts accounting
	 * for file's large size.  We go to the trouble of maintaining the mean
	 * query length in order to prevent garbage collection from thrashing
	 * uselessly.
	 */
	if ((uint64) extent < (uint64) pgss->mean_query_len * pgss_max * 2)
		return false;

	return true;
}

/*
 * Garbage-collect orphaned query texts in external file.
 *
 * This won't be called often in the typical case, since it's likely that
 * there won't be too much churn, and besides, a similar compaction process
 * occurs when serializing to disk at shutdown or as part of resetting.
 * Despite this, it seems prudent to plan for the edge case where the file
 * becomes unreasonably large, with no other method of compaction likely to
 * occur in the foreseeable future.
 *
 * The caller must hold an exclusive lock on pgss->lock.
 *
 * At the first sign of trouble we unlink the query text file to get a clean
 * slate (although existing statistics are retained), rather than risk
 * thrashing by allowing the same problem case to recur indefinitely.
 */
static void
gc_qtexts(void)
{
	char	   *qbuffer;
	Size		qbuffer_size;
	FILE	   *qfile = NULL;
	HASH_SEQ_STATUS hash_seq;
	pgssEntry  *entry;
	Size		extent;
	int			nentries;

	/*
	 * When called from pgss_store, some other session might have proceeded
	 * with garbage collection in the no-lock-held interim of lock strength
	 * escalation.  Check once more that this is actually necessary.
	 */
	if (!need_gc_qtexts())
		return;

	/*
	 * Load the old texts file.  If we fail (out of memory, for instance),
	 * invalidate query texts.  Hopefully this is rare.  It might seem better
	 * to leave things alone on an OOM failure, but the problem is that the
	 * file is only going to get bigger; hoping for a future non-OOM result is
	 * risky and can easily lead to complete denial of service.
	 */
	qbuffer = qtext_load_file(&qbuffer_size);
	if (qbuffer == NULL)
		goto gc_fail;

	/*
	 * We overwrite the query texts file in place, so as to reduce the risk of
	 * an out-of-disk-space failure.  Since the file is guaranteed not to get
	 * larger, this should always work on traditional filesystems; though we
	 * could still lose on copy-on-write filesystems.
	 */
	qfile = AllocateFile(PGSS_TEXT_FILE, PG_BINARY_W);
	if (qfile == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m",
						PGSS_TEXT_FILE)));
		goto gc_fail;
	}

	extent = 0;
	nentries = 0;

	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		int			query_len = entry->query_len;
		char	   *qry = qtext_fetch(entry->query_offset,
									  query_len,
									  qbuffer,
									  qbuffer_size);

		if (qry == NULL)
		{
			/* Trouble ... drop the text */
			entry->query_offset = 0;
			entry->query_len = -1;
			/* entry will not be counted in mean query length computation */
			continue;
		}

		if (fwrite(qry, 1, query_len + 1, qfile) != query_len + 1)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							PGSS_TEXT_FILE)));
			hash_seq_term(&hash_seq);
			goto gc_fail;
		}

		entry->query_offset = extent;
		extent += query_len + 1;
		nentries++;
	}

	/*
	 * Truncate away any now-unused space.  If this fails for some odd reason,
	 * we log it, but there's no need to fail.
	 */
	if (ftruncate(fileno(qfile), extent) != 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not truncate file \"%s\": %m",
						PGSS_TEXT_FILE)));

	if (FreeFile(qfile))
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m",
						PGSS_TEXT_FILE)));
		qfile = NULL;
		goto gc_fail;
	}

	elog(DEBUG1, "pgss gc of queries file shrunk size from %zu to %zu",
		 pgss->extent, extent);

	/* Reset the shared extent pointer */
	pgss->extent = extent;

	/*
	 * Also update the mean query length, to be sure that need_gc_qtexts()
	 * won't still think we have a problem.
	 */
	if (nentries > 0)
		pgss->mean_query_len = extent / nentries;
	else
		pgss->mean_query_len = ASSUMED_LENGTH_INIT;

	free(qbuffer);

	/*
	 * OK, count a garbage collection cycle.  (Note: even though we have
	 * exclusive lock on pgss->lock, we must take pgss->mutex for this, since
	 * other processes may examine gc_count while holding only the mutex.
	 * Also, we have to advance the count *after* we've rewritten the file,
	 * else other processes might not realize they read a stale file.)
	 */
	record_gc_qtexts();

	return;

gc_fail:
	/* clean up resources */
	if (qfile)
		FreeFile(qfile);
	if (qbuffer)
		free(qbuffer);

	/*
	 * Since the contents of the external file are now uncertain, mark all
	 * hashtable entries as having invalid texts.
	 */
	hash_seq_init(&hash_seq, pgss_hash);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		entry->query_offset = 0;
		entry->query_len = -1;
	}

	/*
	 * Destroy the query text file and create a new, empty one
	 */
	(void) unlink(PGSS_TEXT_FILE);
	qfile = AllocateFile(PGSS_TEXT_FILE, PG_BINARY_W);
	if (qfile == NULL)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not recreate file \"%s\": %m",
						PGSS_TEXT_FILE)));
	else
		FreeFile(qfile);

	/* Reset the shared extent pointer */
	pgss->extent = 0;

	/* Reset mean_query_len to match the new state */
	pgss->mean_query_len = ASSUMED_LENGTH_INIT;

	/*
	 * Bump the GC count even though we failed.
	 *
	 * This is needed to make concurrent readers of file without any lock on
	 * pgss->lock notice existence of new version of file.  Once readers
	 * subsequently observe a change in GC count with pgss->lock held, that
	 * forces a safe reopen of file.  Writers also require that we bump here,
	 * of course.  (As required by locking protocol, readers and writers don't
	 * trust earlier file contents until gc_count is found unchanged after
	 * pgss->lock acquired in shared or exclusive mode respectively.)
	 */
	record_gc_qtexts();
}

/*
 * Release entries corresponding to parameters passed.
 */
static void
entry_reset(Oid userid, Oid dbid, uint64 queryid)
{
	HASH_SEQ_STATUS hash_seq;
	pgssEntry  *entry;
	FILE	   *qfile;
	long		num_entries;
	long		num_remove = 0;
	pgssHashKey key;

	if (!pgss || !pgss_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stat_statements must be loaded via shared_preload_libraries")));

	LWLockAcquire(pgss->lock, LW_EXCLUSIVE);
	num_entries = hash_get_num_entries(pgss_hash);

	if (userid != 0 && dbid != 0 && queryid != UINT64CONST(0))
	{
		/* If all the parameters are available, use the fast path. */
		memset(&key, 0, sizeof(pgssHashKey));
		key.userid = userid;
		key.dbid = dbid;
		key.queryid = queryid;

		/* Remove the key if it exists, starting with the top-level entry  */
		key.toplevel = false;
		entry = (pgssEntry *) hash_search(pgss_hash, &key, HASH_REMOVE, NULL);
		if (entry)				/* found */
			num_remove++;

		/* Also remove entries for top level statements */
		key.toplevel = true;

		/* Remove the key if exists */
		entry = (pgssEntry *) hash_search(pgss_hash, &key, HASH_REMOVE, NULL);
		if (entry)				/* found */
			num_remove++;
	}
	else if (userid != 0 || dbid != 0 || queryid != UINT64CONST(0))
	{
		/* Remove entries corresponding to valid parameters. */
		hash_seq_init(&hash_seq, pgss_hash);
		while ((entry = hash_seq_search(&hash_seq)) != NULL)
		{
			if ((!userid || entry->key.userid == userid) &&
				(!dbid || entry->key.dbid == dbid) &&
				(!queryid || entry->key.queryid == queryid))
			{
				hash_search(pgss_hash, &entry->key, HASH_REMOVE, NULL);
				num_remove++;
			}
		}
	}
	else
	{
		/* Remove all entries. */
		hash_seq_init(&hash_seq, pgss_hash);
		while ((entry = hash_seq_search(&hash_seq)) != NULL)
		{
			hash_search(pgss_hash, &entry->key, HASH_REMOVE, NULL);
			num_remove++;
		}
	}

	/* All entries are removed? */
	if (num_entries != num_remove)
		goto release_lock;

	/*
	 * Reset global statistics for pg_stat_statements since all entries are
	 * removed.
	 */
	{
		volatile pgssSharedState *s = (volatile pgssSharedState *) pgss;
		TimestampTz stats_reset = GetCurrentTimestamp();

		SpinLockAcquire(&s->mutex);
		s->stats.dealloc = 0;
		s->stats.stats_reset = stats_reset;
		SpinLockRelease(&s->mutex);
	}

	/*
	 * Write new empty query file, perhaps even creating a new one to recover
	 * if the file was missing.
	 */
	qfile = AllocateFile(PGSS_TEXT_FILE, PG_BINARY_W);
	if (qfile == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m",
						PGSS_TEXT_FILE)));
		goto done;
	}

	/* If ftruncate fails, log it, but it's not a fatal problem */
	if (ftruncate(fileno(qfile), 0) != 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not truncate file \"%s\": %m",
						PGSS_TEXT_FILE)));

	FreeFile(qfile);

done:
	pgss->extent = 0;
	/* This counts as a query text garbage collection for our purposes */
	record_gc_qtexts();

release_lock:
	LWLockRelease(pgss->lock);
}

/*
 * Generate a normalized version of the query string that will be used to
 * represent all similar queries.
 *
 * Note that the normalized representation may well vary depending on
 * just which "equivalent" query is used to create the hashtable entry.
 * We assume this is OK.
 *
 * If query_loc > 0, then "query" has been advanced by that much compared to
 * the original string start, so we need to translate the provided locations
 * to compensate.  (This lets us avoid re-scanning statements before the one
 * of interest, so it's worth doing.)
 *
 * *query_len_p contains the input string length, and is updated with
 * the result string length on exit.  The resulting string might be longer
 * or shorter depending on what happens with replacement of constants.
 *
 * Returns a palloc'd string.
 */
static char *
generate_normalized_query(JumbleState *jstate, const char *query,
						  int query_loc, int *query_len_p)
{
	char	   *norm_query;
	int			query_len = *query_len_p;
	int			i,
				norm_query_buflen,	/* Space allowed for norm_query */
				len_to_wrt,		/* Length (in bytes) to write */
				quer_loc = 0,	/* Source query byte location */
				n_quer_loc = 0, /* Normalized query byte location */
				last_off = 0,	/* Offset from start for previous tok */
				last_tok_len = 0;	/* Length (in bytes) of that tok */

	/*
	 * Get constants' lengths (core system only gives us locations).  Note
	 * this also ensures the items are sorted by location.
	 */
	fill_in_constant_lengths(jstate, query, query_loc);

	/*
	 * Allow for $n symbols to be longer than the constants they replace.
	 * Constants must take at least one byte in text form, while a $n symbol
	 * certainly isn't more than 11 bytes, even if n reaches INT_MAX.  We
	 * could refine that limit based on the max value of n for the current
	 * query, but it hardly seems worth any extra effort to do so.
	 */
	norm_query_buflen = query_len + jstate->clocations_count * 10;

	/* Allocate result buffer */
	norm_query = palloc(norm_query_buflen + 1);

	for (i = 0; i < jstate->clocations_count; i++)
	{
		int			off,		/* Offset from start for cur tok */
					tok_len;	/* Length (in bytes) of that tok */

		off = jstate->clocations[i].location;
		/* Adjust recorded location if we're dealing with partial string */
		off -= query_loc;

		tok_len = jstate->clocations[i].length;

		if (tok_len < 0)
			continue;			/* ignore any duplicates */

		/* Copy next chunk (what precedes the next constant) */
		len_to_wrt = off - last_off;
		len_to_wrt -= last_tok_len;

		Assert(len_to_wrt >= 0);
		memcpy(norm_query + n_quer_loc, query + quer_loc, len_to_wrt);
		n_quer_loc += len_to_wrt;

		/* And insert a param symbol in place of the constant token */
		n_quer_loc += sprintf(norm_query + n_quer_loc, "$%d",
							  i + 1 + jstate->highest_extern_param_id);

		quer_loc = off + tok_len;
		last_off = off;
		last_tok_len = tok_len;
	}

	/*
	 * We've copied up until the last ignorable constant.  Copy over the
	 * remaining bytes of the original query string.
	 */
	len_to_wrt = query_len - quer_loc;

	Assert(len_to_wrt >= 0);
	memcpy(norm_query + n_quer_loc, query + quer_loc, len_to_wrt);
	n_quer_loc += len_to_wrt;

	Assert(n_quer_loc <= norm_query_buflen);
	norm_query[n_quer_loc] = '\0';

	*query_len_p = n_quer_loc;
	return norm_query;
}

/*
 * Given a valid SQL string and an array of constant-location records,
 * fill in the textual lengths of those constants.
 *
 * The constants may use any allowed constant syntax, such as float literals,
 * bit-strings, single-quoted strings and dollar-quoted strings.  This is
 * accomplished by using the public API for the core scanner.
 *
 * It is the caller's job to ensure that the string is a valid SQL statement
 * with constants at the indicated locations.  Since in practice the string
 * has already been parsed, and the locations that the caller provides will
 * have originated from within the authoritative parser, this should not be
 * a problem.
 *
 * Duplicate constant pointers are possible, and will have their lengths
 * marked as '-1', so that they are later ignored.  (Actually, we assume the
 * lengths were initialized as -1 to start with, and don't change them here.)
 *
 * If query_loc > 0, then "query" has been advanced by that much compared to
 * the original string start, so we need to translate the provided locations
 * to compensate.  (This lets us avoid re-scanning statements before the one
 * of interest, so it's worth doing.)
 *
 * N.B. There is an assumption that a '-' character at a Const location begins
 * a negative numeric constant.  This precludes there ever being another
 * reason for a constant to start with a '-'.
 */
static void
fill_in_constant_lengths(JumbleState *jstate, const char *query,
						 int query_loc)
{
	LocationLen *locs;
	core_yyscan_t yyscanner;
	core_yy_extra_type yyextra;
	core_YYSTYPE yylval;
	YYLTYPE		yylloc;
	int			last_loc = -1;
	int			i;

	/*
	 * Sort the records by location so that we can process them in order while
	 * scanning the query text.
	 */
	if (jstate->clocations_count > 1)
		qsort(jstate->clocations, jstate->clocations_count,
			  sizeof(LocationLen), comp_location);
	locs = jstate->clocations;

	/* initialize the flex scanner --- should match raw_parser() */
	yyscanner = scanner_init(query,
							 &yyextra,
							 &ScanKeywords,
							 ScanKeywordTokens);

	/* we don't want to re-emit any escape string warnings */
	yyextra.escape_string_warning = false;

	/* Search for each constant, in sequence */
	for (i = 0; i < jstate->clocations_count; i++)
	{
		int			loc = locs[i].location;
		int			tok;

		/* Adjust recorded location if we're dealing with partial string */
		loc -= query_loc;

		Assert(loc >= 0);

		if (loc <= last_loc)
			continue;			/* Duplicate constant, ignore */

		/* Lex tokens until we find the desired constant */
		for (;;)
		{
			tok = core_yylex(&yylval, &yylloc, yyscanner);

			/* We should not hit end-of-string, but if we do, behave sanely */
			if (tok == 0)
				break;			/* out of inner for-loop */

			/*
			 * We should find the token position exactly, but if we somehow
			 * run past it, work with that.
			 */
			if (yylloc >= loc)
			{
				if (query[loc] == '-')
				{
					/*
					 * It's a negative value - this is the one and only case
					 * where we replace more than a single token.
					 *
					 * Do not compensate for the core system's special-case
					 * adjustment of location to that of the leading '-'
					 * operator in the event of a negative constant.  It is
					 * also useful for our purposes to start from the minus
					 * symbol.  In this way, queries like "select * from foo
					 * where bar = 1" and "select * from foo where bar = -2"
					 * will have identical normalized query strings.
					 */
					tok = core_yylex(&yylval, &yylloc, yyscanner);
					if (tok == 0)
						break;	/* out of inner for-loop */
				}

				/*
				 * We now rely on the assumption that flex has placed a zero
				 * byte after the text of the current token in scanbuf.
				 */
				locs[i].length = strlen(yyextra.scanbuf + loc);
				break;			/* out of inner for-loop */
			}
		}

		/* If we hit end-of-string, give up, leaving remaining lengths -1 */
		if (tok == 0)
			break;

		last_loc = loc;
	}

	scanner_finish(yyscanner);
}

/*
 * comp_location: comparator for qsorting LocationLen structs by location
 */
static int
comp_location(const void *a, const void *b)
{
	int			l = ((const LocationLen *) a)->location;
	int			r = ((const LocationLen *) b)->location;

	if (l < r)
		return -1;
	else if (l > r)
		return +1;
	else
		return 0;
}
