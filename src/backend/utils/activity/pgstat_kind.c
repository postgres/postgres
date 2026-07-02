/*-------------------------------------------------------------------------
 *
 * pgstat_kind.c
 *	  Functions related to statistics kinds.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_kind.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/pgstat_internal.h"
#include "utils/pgstat_kind.h"
#include "utils/tuplestore.h"


/*
 * pg_stat_get_kind_info
 *
 * Get information about the statistics kinds registered into the system.
 */
Datum
pg_stat_get_kind_info(PG_FUNCTION_ARGS)
{
#define PG_STAT_KIND_INFO_COLS	7
	ReturnSetInfo *rsinfo;

	InitMaterializedSRF(fcinfo, 0);
	rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	for (int kind = PGSTAT_KIND_MIN; kind <= PGSTAT_KIND_MAX; kind++)
	{
		Datum		values[PG_STAT_KIND_INFO_COLS] = {0};
		bool		nulls[PG_STAT_KIND_INFO_COLS] = {0};
		const PgStat_KindInfo *info;

		info = pgstat_get_kind_info(kind);
		if (info == NULL)
			continue;

		values[0] = Int32GetDatum(kind);
		values[1] = CStringGetTextDatum(info->name);

		values[2] = BoolGetDatum(pgstat_is_kind_builtin(kind));
		values[3] = BoolGetDatum(info->fixed_amount);
		values[4] = BoolGetDatum(info->accessed_across_databases);
		values[5] = BoolGetDatum(info->write_to_file);

		/*
		 * When track_entry_count is disabled, use NULL.  Fixed-sized stats
		 * kinds report NULL here.
		 */
		if (info->track_entry_count)
			values[6] = Int64GetDatum(pgstat_get_entry_count(kind));
		else
			nulls[6] = true;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
#undef PG_STAT_KIND_INFO_COLS
}
