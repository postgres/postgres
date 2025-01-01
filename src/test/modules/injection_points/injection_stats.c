/*--------------------------------------------------------------------------
 *
 * injection_stats.c
 *		Code for statistics of injection points.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/test/modules/injection_points/injection_stats.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"

#include "common/hashfn.h"
#include "injection_stats.h"
#include "pgstat.h"
#include "utils/builtins.h"
#include "utils/pgstat_internal.h"

/* Structures for statistics of injection points */
typedef struct PgStat_StatInjEntry
{
	PgStat_Counter numcalls;	/* number of times point has been run */
} PgStat_StatInjEntry;

typedef struct PgStatShared_InjectionPoint
{
	PgStatShared_Common header;
	PgStat_StatInjEntry stats;
} PgStatShared_InjectionPoint;

static bool injection_stats_flush_cb(PgStat_EntryRef *entry_ref, bool nowait);

static const PgStat_KindInfo injection_stats = {
	.name = "injection_points",
	.fixed_amount = false,		/* Bounded by the number of points */
	.write_to_file = true,

	/* Injection points are system-wide */
	.accessed_across_databases = true,

	.shared_size = sizeof(PgStatShared_InjectionPoint),
	.shared_data_off = offsetof(PgStatShared_InjectionPoint, stats),
	.shared_data_len = sizeof(((PgStatShared_InjectionPoint *) 0)->stats),
	.pending_size = sizeof(PgStat_StatInjEntry),
	.flush_pending_cb = injection_stats_flush_cb,
};

/*
 * Compute stats entry idx from point name with an 8-byte hash.
 */
#define PGSTAT_INJ_IDX(name) hash_bytes_extended((const unsigned char *) name, strlen(name), 0)

/*
 * Kind ID reserved for statistics of injection points.
 */
#define PGSTAT_KIND_INJECTION	129

/* Track if stats are loaded */
static bool inj_stats_loaded = false;

/*
 * Callback for stats handling
 */
static bool
injection_stats_flush_cb(PgStat_EntryRef *entry_ref, bool nowait)
{
	PgStat_StatInjEntry *localent;
	PgStatShared_InjectionPoint *shfuncent;

	localent = (PgStat_StatInjEntry *) entry_ref->pending;
	shfuncent = (PgStatShared_InjectionPoint *) entry_ref->shared_stats;

	if (!pgstat_lock_entry(entry_ref, nowait))
		return false;

	shfuncent->stats.numcalls += localent->numcalls;

	pgstat_unlock_entry(entry_ref);

	return true;
}

/*
 * Support function for the SQL-callable pgstat* functions.  Returns
 * a pointer to the injection point statistics struct.
 */
static PgStat_StatInjEntry *
pgstat_fetch_stat_injentry(const char *name)
{
	PgStat_StatInjEntry *entry = NULL;

	if (!inj_stats_loaded || !inj_stats_enabled)
		return NULL;

	/* Compile the lookup key as a hash of the point name */
	entry = (PgStat_StatInjEntry *) pgstat_fetch_entry(PGSTAT_KIND_INJECTION,
													   InvalidOid,
													   PGSTAT_INJ_IDX(name));
	return entry;
}

/*
 * Workhorse to do the registration work, called in _PG_init().
 */
void
pgstat_register_inj(void)
{
	pgstat_register_kind(PGSTAT_KIND_INJECTION, &injection_stats);

	/* mark stats as loaded */
	inj_stats_loaded = true;
}

/*
 * Report injection point creation.
 */
void
pgstat_create_inj(const char *name)
{
	PgStat_EntryRef *entry_ref;
	PgStatShared_InjectionPoint *shstatent;

	/* leave if disabled */
	if (!inj_stats_loaded || !inj_stats_enabled)
		return;

	entry_ref = pgstat_prep_pending_entry(PGSTAT_KIND_INJECTION, InvalidOid,
										  PGSTAT_INJ_IDX(name), NULL);

	shstatent = (PgStatShared_InjectionPoint *) entry_ref->shared_stats;

	/* initialize shared memory data */
	memset(&shstatent->stats, 0, sizeof(shstatent->stats));
}

/*
 * Report injection point drop.
 */
void
pgstat_drop_inj(const char *name)
{
	/* leave if disabled */
	if (!inj_stats_loaded || !inj_stats_enabled)
		return;

	if (!pgstat_drop_entry(PGSTAT_KIND_INJECTION, InvalidOid,
						   PGSTAT_INJ_IDX(name)))
		pgstat_request_entry_refs_gc();
}

/*
 * Report statistics for injection point.
 *
 * This is simple because the set of stats to report currently is simple:
 * track the number of times a point has been run.
 */
void
pgstat_report_inj(const char *name)
{
	PgStat_EntryRef *entry_ref;
	PgStatShared_InjectionPoint *shstatent;
	PgStat_StatInjEntry *statent;

	/* leave if disabled */
	if (!inj_stats_loaded || !inj_stats_enabled)
		return;

	entry_ref = pgstat_prep_pending_entry(PGSTAT_KIND_INJECTION, InvalidOid,
										  PGSTAT_INJ_IDX(name), NULL);

	shstatent = (PgStatShared_InjectionPoint *) entry_ref->shared_stats;
	statent = &shstatent->stats;

	/* Update the injection point statistics */
	statent->numcalls++;
}

/*
 * SQL function returning the number of times an injection point
 * has been called.
 */
PG_FUNCTION_INFO_V1(injection_points_stats_numcalls);
Datum
injection_points_stats_numcalls(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	PgStat_StatInjEntry *entry = pgstat_fetch_stat_injentry(name);

	if (entry == NULL)
		PG_RETURN_NULL();

	PG_RETURN_INT64(entry->numcalls);
}
