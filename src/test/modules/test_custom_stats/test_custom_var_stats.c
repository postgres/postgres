/*------------------------------------------------------------------------------------
 *
 * test_custom_var_stats.c
 *		Test module for variable-sized custom pgstats
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_custom_var_stats/test_custom_var_stats.c
 *
 * ------------------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/pgstat_internal.h"

PG_MODULE_MAGIC_EXT(
					.name = "test_custom_var_stats",
					.version = PG_VERSION
);

/*--------------------------------------------------------------------------
 * Macros and constants
 *--------------------------------------------------------------------------
 */

/*
 * Kind ID for test_custom_var_stats statistics.
 */
#define PGSTAT_KIND_TEST_CUSTOM_VAR_STATS 25

/*
 * Hash statistic name to generate entry index for pgstat lookup.
 */
#define PGSTAT_CUSTOM_VAR_STATS_IDX(name) hash_bytes_extended((const unsigned char *) name, strlen(name), 0)

/*--------------------------------------------------------------------------
 * Type definitions
 *--------------------------------------------------------------------------
 */

/* Backend-local pending statistics before flush to shared memory */
typedef struct PgStat_StatCustomVarEntry
{
	PgStat_Counter numcalls;	/* times statistic was incremented */
} PgStat_StatCustomVarEntry;

/* Shared memory statistics entry visible to all backends */
typedef struct PgStatShared_CustomVarEntry
{
	PgStatShared_Common header; /* standard pgstat entry header */
	PgStat_StatCustomVarEntry stats;	/* custom statistics data */
} PgStatShared_CustomVarEntry;

/*--------------------------------------------------------------------------
 * Function prototypes
 *--------------------------------------------------------------------------
 */

/* Flush callback: merge pending stats into shared memory */
static bool test_custom_stats_var_flush_pending_cb(PgStat_EntryRef *entry_ref,
												   bool nowait);

/*--------------------------------------------------------------------------
 * Custom kind configuration
 *--------------------------------------------------------------------------
 */

static const PgStat_KindInfo custom_stats = {
	.name = "test_custom_var_stats",
	.fixed_amount = false,		/* variable number of entries */
	.write_to_file = true,		/* persist across restarts */
	.track_entry_count = true,	/* count active entries */
	.accessed_across_databases = true,	/* global statistics */
	.shared_size = sizeof(PgStatShared_CustomVarEntry),
	.shared_data_off = offsetof(PgStatShared_CustomVarEntry, stats),
	.shared_data_len = sizeof(((PgStatShared_CustomVarEntry *) 0)->stats),
	.pending_size = sizeof(PgStat_StatCustomVarEntry),
	.flush_pending_cb = test_custom_stats_var_flush_pending_cb,
};

/*--------------------------------------------------------------------------
 * Module initialization
 *--------------------------------------------------------------------------
 */

void
_PG_init(void)
{
	/* Must be loaded via shared_preload_libraries */
	if (!process_shared_preload_libraries_in_progress)
		return;

	/* Register custom statistics kind */
	pgstat_register_kind(PGSTAT_KIND_TEST_CUSTOM_VAR_STATS, &custom_stats);
}

/*--------------------------------------------------------------------------
 * Statistics callback functions
 *--------------------------------------------------------------------------
 */

/*
 * test_custom_stats_var_flush_pending_cb
 *		Merge pending backend statistics into shared memory
 *
 * Called by pgstat collector to flush accumulated local statistics
 * to shared memory where other backends can read them.
 *
 * Returns false only if nowait=true and lock acquisition fails.
 */
static bool
test_custom_stats_var_flush_pending_cb(PgStat_EntryRef *entry_ref, bool nowait)
{
	PgStat_StatCustomVarEntry *pending_entry;
	PgStatShared_CustomVarEntry *shared_entry;

	pending_entry = (PgStat_StatCustomVarEntry *) entry_ref->pending;
	shared_entry = (PgStatShared_CustomVarEntry *) entry_ref->shared_stats;

	if (!pgstat_lock_entry(entry_ref, nowait))
		return false;

	/* Add pending counts to shared totals */
	shared_entry->stats.numcalls += pending_entry->numcalls;

	pgstat_unlock_entry(entry_ref);

	return true;
}

/*--------------------------------------------------------------------------
 * Helper functions
 *--------------------------------------------------------------------------
 */

/*
 * test_custom_stats_var_fetch_entry
 *		Look up custom statistic by name
 *
 * Returns statistics entry from shared memory, or NULL if not found.
 */
static PgStat_StatCustomVarEntry *
test_custom_stats_var_fetch_entry(const char *stat_name)
{
	/* Fetch entry by hashed name */
	return (PgStat_StatCustomVarEntry *)
		pgstat_fetch_entry(PGSTAT_KIND_TEST_CUSTOM_VAR_STATS,
						   InvalidOid,
						   PGSTAT_CUSTOM_VAR_STATS_IDX(stat_name));
}

/*--------------------------------------------------------------------------
 * SQL-callable functions
 *--------------------------------------------------------------------------
 */

/*
 * test_custom_stats_var_create
 *		Create new custom statistic entry
 *
 * Initializes a zero-valued statistics entry in shared memory.
 * Validates name length against NAMEDATALEN limit.
 */
PG_FUNCTION_INFO_V1(test_custom_stats_var_create);
Datum
test_custom_stats_var_create(PG_FUNCTION_ARGS)
{
	PgStat_EntryRef *entry_ref;
	PgStatShared_CustomVarEntry *shared_entry;
	char	   *stat_name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	/* Validate name length first */
	if (strlen(stat_name) >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_NAME_TOO_LONG),
				 errmsg("custom statistic name \"%s\" is too long", stat_name),
				 errdetail("Name must be less than %d characters.", NAMEDATALEN)));

	/* Create or get existing entry */
	entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_TEST_CUSTOM_VAR_STATS, InvalidOid,
											PGSTAT_CUSTOM_VAR_STATS_IDX(stat_name), true);

	if (!entry_ref)
		PG_RETURN_VOID();

	shared_entry = (PgStatShared_CustomVarEntry *) entry_ref->shared_stats;

	/* Zero-initialize statistics */
	memset(&shared_entry->stats, 0, sizeof(shared_entry->stats));

	pgstat_unlock_entry(entry_ref);

	PG_RETURN_VOID();
}

/*
 * test_custom_stats_var_update
 *		Increment custom statistic counter
 *
 * Increments call count in backend-local memory.  Changes are flushed
 * to shared memory by the statistics collector.
 */
PG_FUNCTION_INFO_V1(test_custom_stats_var_update);
Datum
test_custom_stats_var_update(PG_FUNCTION_ARGS)
{
	char	   *stat_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	PgStat_EntryRef *entry_ref;
	PgStat_StatCustomVarEntry *pending_entry;

	/* Get pending entry in local memory */
	entry_ref = pgstat_prep_pending_entry(PGSTAT_KIND_TEST_CUSTOM_VAR_STATS, InvalidOid,
										  PGSTAT_CUSTOM_VAR_STATS_IDX(stat_name), NULL);

	pending_entry = (PgStat_StatCustomVarEntry *) entry_ref->pending;
	pending_entry->numcalls++;

	PG_RETURN_VOID();
}

/*
 * test_custom_stats_var_drop
 *		Remove custom statistic entry
 *
 * Drops the named statistic from shared memory and requests
 * garbage collection if needed.
 */
PG_FUNCTION_INFO_V1(test_custom_stats_var_drop);
Datum
test_custom_stats_var_drop(PG_FUNCTION_ARGS)
{
	char	   *stat_name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	/* Drop entry and request GC if the entry could not be freed */
	if (!pgstat_drop_entry(PGSTAT_KIND_TEST_CUSTOM_VAR_STATS, InvalidOid,
						   PGSTAT_CUSTOM_VAR_STATS_IDX(stat_name)))
		pgstat_request_entry_refs_gc();

	PG_RETURN_VOID();
}

/*
 * test_custom_stats_var_report
 *		Retrieve custom statistic values
 *
 * Returns single row with statistic name and call count if the
 * statistic exists, otherwise returns no rows.
 */
PG_FUNCTION_INFO_V1(test_custom_stats_var_report);
Datum
test_custom_stats_var_report(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	char	   *stat_name;
	PgStat_StatCustomVarEntry *stat_entry;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;

		/* Initialize SRF context */
		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Get composite return type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "test_custom_stats_var_report: return type is not composite");

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);
		funcctx->max_calls = 1; /* single row result */

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		Datum		values[2];
		bool		nulls[2] = {false, false};
		HeapTuple	tuple;

		stat_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
		stat_entry = test_custom_stats_var_fetch_entry(stat_name);

		/* Return row only if entry exists */
		if (stat_entry)
		{
			values[0] = PointerGetDatum(cstring_to_text(stat_name));
			values[1] = Int64GetDatum(stat_entry->numcalls);

			tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
			SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
		}
	}

	SRF_RETURN_DONE(funcctx);
}
