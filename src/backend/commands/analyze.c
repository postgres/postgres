/*-------------------------------------------------------------------------
 *
 * analyze.c
 *	  the postgres optimizer analyzer
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/analyze.c,v 1.8 2000/10/16 17:08:05 momjian Exp $
 *

 *-------------------------------------------------------------------------
 */
#include <sys/types.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "parser/parse_oper.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/syscache.h"

#define swapLong(a,b)	{long tmp; tmp=a; a=b; b=tmp;}
#define swapInt(a,b)	{int tmp; tmp=a; a=b; b=tmp;}
#define swapDatum(a,b)	{Datum tmp; tmp=a; a=b; b=tmp;}
#define VacAttrStatsEqValid(stats) ( stats->f_cmpeq.fn_addr != NULL )
#define VacAttrStatsLtGtValid(stats) ( stats->f_cmplt.fn_addr != NULL && \
								   stats->f_cmpgt.fn_addr != NULL && \
								   RegProcedureIsValid(stats->outfunc) )


static void attr_stats(Relation onerel, int attr_cnt, VacAttrStats *vacattrstats, HeapTuple tuple);
static void bucketcpy(Form_pg_attribute attr, Datum value, Datum *bucket, int *bucket_len);
static void update_attstats(Oid relid, int natts, VacAttrStats *vacattrstats);
static void del_stats(Oid relid, int attcnt, int *attnums);


/*
 *	analyze_rel() -- analyze relation
 */
void
analyze_rel(Oid relid, List *anal_cols2, int MESSAGE_LEVEL)
{
	HeapTuple	tuple,
				typetuple;
	Relation	onerel;
	int32		i;
	int			attr_cnt,
			   *attnums = NULL;
	Form_pg_attribute *attr;
	VacAttrStats *vacattrstats;
	HeapScanDesc scan;

	StartTransactionCommand();

	/*
	 * Check for user-requested abort.	Note we want this to be inside a
	 * transaction, so xact.c doesn't issue useless NOTICE.
	 */
	if (QueryCancel)
		CancelQuery();

	/*
	 * Race condition -- if the pg_class tuple has gone away since the
	 * last time we saw it, we don't need to vacuum it.
	 */
	tuple = SearchSysCacheTuple(RELOID,
								ObjectIdGetDatum(relid),
								0, 0, 0);
	/*
	 * We can VACUUM ANALYZE any table except pg_statistic.
	 * see update_relstats
	 */
	if (!HeapTupleIsValid(tuple) ||
		strcmp(NameStr(((Form_pg_class) GETSTRUCT(tuple))->relname),
				StatisticRelationName) == 0)
	{
		CommitTransactionCommand();
		return;
	}

	onerel = heap_open(relid, AccessShareLock);

	if (!pg_ownercheck(GetUserId(), RelationGetRelationName(onerel),
					   RELNAME))
	{
		/* we already did an elog during vacuum
		elog(NOTICE, "Skipping \"%s\" --- only table owner can VACUUM it",
			 RelationGetRelationName(onerel));
		*/
		heap_close(onerel, NoLock);
		CommitTransactionCommand();
		return;
	}

	elog(MESSAGE_LEVEL, "Analyzing...");

	attr_cnt = onerel->rd_att->natts;
	attr = onerel->rd_att->attrs;

	if (anal_cols2 != NIL)
	{
		int			tcnt = 0;
		List	   *le;

		if (length(anal_cols2) > attr_cnt)
			elog(ERROR, "vacuum: too many attributes specified for relation %s",
				 RelationGetRelationName(onerel));
		attnums = (int *) palloc(attr_cnt * sizeof(int));
		foreach(le, anal_cols2)
		{
			char	   *col = (char *) lfirst(le);

			for (i = 0; i < attr_cnt; i++)
			{
				if (namestrcmp(&(attr[i]->attname), col) == 0)
					break;
			}
			if (i < attr_cnt)		/* found */
				attnums[tcnt++] = i;
			else
			{
				elog(ERROR, "vacuum: there is no attribute %s in %s",
					 col, RelationGetRelationName(onerel));
			}
		}
		attr_cnt = tcnt;
	}

	vacattrstats = (VacAttrStats *) palloc(attr_cnt * sizeof(VacAttrStats));

	for (i = 0; i < attr_cnt; i++)
	{
		Operator	func_operator;
		Form_pg_operator pgopform;
		VacAttrStats *stats;

		stats = &vacattrstats[i];
		stats->attr = palloc(ATTRIBUTE_TUPLE_SIZE);
		memmove(stats->attr, attr[((attnums) ? attnums[i] : i)], ATTRIBUTE_TUPLE_SIZE);
		stats->best = stats->guess1 = stats->guess2 = 0;
		stats->max = stats->min = 0;
		stats->best_len = stats->guess1_len = stats->guess2_len = 0;
		stats->max_len = stats->min_len = 0;
		stats->initialized = false;
		stats->best_cnt = stats->guess1_cnt = stats->guess1_hits = stats->guess2_hits = 0;
		stats->max_cnt = stats->min_cnt = stats->null_cnt = stats->nonnull_cnt = 0;

		func_operator = oper("=", stats->attr->atttypid, stats->attr->atttypid, true);
		if (func_operator != NULL)
		{
			pgopform = (Form_pg_operator) GETSTRUCT(func_operator);
			fmgr_info(pgopform->oprcode, &(stats->f_cmpeq));
		}
		else
			stats->f_cmpeq.fn_addr = NULL;

		func_operator = oper("<", stats->attr->atttypid, stats->attr->atttypid, true);
		if (func_operator != NULL)
		{
			pgopform = (Form_pg_operator) GETSTRUCT(func_operator);
			fmgr_info(pgopform->oprcode, &(stats->f_cmplt));
			stats->op_cmplt = oprid(func_operator);
		}
		else
		{
			stats->f_cmplt.fn_addr = NULL;
			stats->op_cmplt = InvalidOid;
		}

		func_operator = oper(">", stats->attr->atttypid, stats->attr->atttypid, true);
		if (func_operator != NULL)
		{
			pgopform = (Form_pg_operator) GETSTRUCT(func_operator);
			fmgr_info(pgopform->oprcode, &(stats->f_cmpgt));
		}
		else
			stats->f_cmpgt.fn_addr = NULL;

		typetuple = SearchSysCacheTuple(TYPEOID,
							 ObjectIdGetDatum(stats->attr->atttypid),
										0, 0, 0);
		if (HeapTupleIsValid(typetuple))
		{
			stats->outfunc = ((Form_pg_type) GETSTRUCT(typetuple))->typoutput;
			stats->typelem = ((Form_pg_type) GETSTRUCT(typetuple))->typelem;
		}
		else
		{
			stats->outfunc = InvalidOid;
			stats->typelem = InvalidOid;
		}
	}
	/* delete existing pg_statistic rows for relation */
	del_stats(relid, ((attnums) ? attr_cnt : 0), attnums);

	scan = heap_beginscan(onerel, false, SnapshotNow, 0, NULL);

	while (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
		attr_stats(onerel, attr_cnt, vacattrstats, tuple);

	heap_endscan(scan);

	/* close rel, but keep lock so it doesn't go away before commit */
	heap_close(onerel, NoLock);

	/* update statistics in pg_class */
	update_attstats(relid, attr_cnt, vacattrstats);

	CommitTransactionCommand();
}

/*
 *	attr_stats() -- compute column statistics used by the optimzer
 *
 *	We compute the column min, max, null and non-null counts.
 *	Plus we attempt to find the count of the value that occurs most
 *	frequently in each column.	These figures are used to compute
 *	the selectivity of the column.
 *
 *	We use a three-bucket cache to get the most frequent item.
 *	The 'guess' buckets count hits.  A cache miss causes guess1
 *	to get the most hit 'guess' item in the most recent cycle, and
 *	the new item goes into guess2.	Whenever the total count of hits
 *	of a 'guess' entry is larger than 'best', 'guess' becomes 'best'.
 *
 *	This method works perfectly for columns with unique values, and columns
 *	with only two unique values, plus nulls.
 *
 *	It becomes less perfect as the number of unique values increases and
 *	their distribution in the table becomes more random.
 *
 */
static void
attr_stats(Relation onerel, int attr_cnt, VacAttrStats *vacattrstats, HeapTuple tuple)
{
	int			i;
	TupleDesc	tupDesc = onerel->rd_att;

	for (i = 0; i < attr_cnt; i++)
	{
		VacAttrStats *stats = &vacattrstats[i];
		Datum		value;
		bool		isnull;
		bool		value_hit;

		if (!VacAttrStatsEqValid(stats))
			continue;

#ifdef	_DROP_COLUMN_HACK__
		if (COLUMN_IS_DROPPED(stats->attr))
			continue;
#endif	 /* _DROP_COLUMN_HACK__ */

		value = heap_getattr(tuple,
							 stats->attr->attnum, tupDesc, &isnull);

		if (isnull)
		{
			stats->null_cnt++;
			continue;
		}

		stats->nonnull_cnt++;
		if (! stats->initialized)
		{
			bucketcpy(stats->attr, value, &stats->best, &stats->best_len);
			/* best_cnt gets incremented below */
			bucketcpy(stats->attr, value, &stats->guess1, &stats->guess1_len);
			stats->guess1_cnt = stats->guess1_hits = 1;
			bucketcpy(stats->attr, value, &stats->guess2, &stats->guess2_len);
			stats->guess2_hits = 1;
			if (VacAttrStatsLtGtValid(stats))
			{
				bucketcpy(stats->attr, value, &stats->max, &stats->max_len);
				bucketcpy(stats->attr, value, &stats->min, &stats->min_len);
				/* min_cnt, max_cnt get incremented below */
			}
			stats->initialized = true;
		}

		if (VacAttrStatsLtGtValid(stats))
		{
			if (DatumGetBool(FunctionCall2(&stats->f_cmplt,
										   value, stats->min)))
			{
				bucketcpy(stats->attr, value, &stats->min, &stats->min_len);
				stats->min_cnt = 1;
			}
			else if (DatumGetBool(FunctionCall2(&stats->f_cmpeq,
												value, stats->min)))
				stats->min_cnt++;

			if (DatumGetBool(FunctionCall2(&stats->f_cmpgt,
										   value, stats->max)))
			{
				bucketcpy(stats->attr, value, &stats->max, &stats->max_len);
				stats->max_cnt = 1;
			}
			else if (DatumGetBool(FunctionCall2(&stats->f_cmpeq,
												value, stats->max)))
				stats->max_cnt++;
		}

		value_hit = true;
		if (DatumGetBool(FunctionCall2(&stats->f_cmpeq,
									   value, stats->best)))
			stats->best_cnt++;
		else if (DatumGetBool(FunctionCall2(&stats->f_cmpeq,
											value, stats->guess1)))
		{
			stats->guess1_cnt++;
			stats->guess1_hits++;
		}
		else if (DatumGetBool(FunctionCall2(&stats->f_cmpeq,
											value, stats->guess2)))
			stats->guess2_hits++;
		else
			value_hit = false;

		if (stats->guess2_hits > stats->guess1_hits)
		{
			swapDatum(stats->guess1, stats->guess2);
			swapInt(stats->guess1_len, stats->guess2_len);
			swapLong(stats->guess1_hits, stats->guess2_hits);
			stats->guess1_cnt = stats->guess1_hits;
		}
		if (stats->guess1_cnt > stats->best_cnt)
		{
			swapDatum(stats->best, stats->guess1);
			swapInt(stats->best_len, stats->guess1_len);
			swapLong(stats->best_cnt, stats->guess1_cnt);
			stats->guess1_hits = 1;
			stats->guess2_hits = 1;
		}
		if (!value_hit)
		{
			bucketcpy(stats->attr, value, &stats->guess2, &stats->guess2_len);
			stats->guess1_hits = 1;
			stats->guess2_hits = 1;
		}
	}
}

/*
 *	bucketcpy() -- copy a new value into one of the statistics buckets
 *
 */
static void
bucketcpy(Form_pg_attribute attr, Datum value, Datum *bucket, int *bucket_len)
{
	if (attr->attbyval && attr->attlen != -1)
		*bucket = value;
	else
	{
		int			len = (attr->attlen != -1 ? attr->attlen : VARSIZE(value));

		if (len > *bucket_len)
		{
			if (*bucket_len != 0)
				pfree(DatumGetPointer(*bucket));
			*bucket = PointerGetDatum(palloc(len));
			*bucket_len = len;
		}
		memcpy(DatumGetPointer(*bucket), DatumGetPointer(value), len);
	}
}


/*
 *	update_attstats() -- update attribute statistics for one relation
 *
 *		Updates of pg_attribute statistics are handled by over-write,
 *		for reasons described above.  pg_statistic rows are added normally.
 *
 *		To keep things simple, we punt for pg_statistic, and don't try
 *		to compute or store rows for pg_statistic itself in pg_statistic.
 *		This could possibly be made to work, but it's not worth the trouble.
 */
static void
update_attstats(Oid relid, int natts, VacAttrStats *vacattrstats)
{
	Relation	ad,
				sd;
	HeapScanDesc scan;
	HeapTuple	atup,
				stup;
	ScanKeyData askey;
	Form_pg_attribute attp;

	ad = heap_openr(AttributeRelationName, RowExclusiveLock);
	sd = heap_openr(StatisticRelationName, RowExclusiveLock);

	/* Find pg_attribute rows for this relation */
	ScanKeyEntryInitialize(&askey, 0, Anum_pg_attribute_attrelid,
						   F_INT4EQ, relid);

	scan = heap_beginscan(ad, false, SnapshotNow, 1, &askey);

	while (HeapTupleIsValid(atup = heap_getnext(scan, 0)))
	{
		int			i;
		VacAttrStats *stats;

		attp = (Form_pg_attribute) GETSTRUCT(atup);
		if (attp->attnum <= 0)		/* skip system attributes for now */
			continue;

		for (i = 0; i < natts; i++)
		{
			if (attp->attnum == vacattrstats[i].attr->attnum)
				break;
		}
		if (i >= natts)
			continue;		/* skip attr if no stats collected */
		stats = &(vacattrstats[i]);

		if (VacAttrStatsEqValid(stats))
		{
			float4	selratio;	/* average ratio of rows selected
								 * for a random constant */

			/* Compute dispersion */
			if (stats->nonnull_cnt == 0 && stats->null_cnt == 0)
			{

				/*
				 * empty relation, so put a dummy value in
				 * attdispersion
				 */
				selratio = 0;
			}
			else if (stats->null_cnt <= 1 && stats->best_cnt == 1)
			{
				/*
				 * looks like we have a unique-key attribute --- flag
				 * this with special -1.0 flag value.
				 *
				 * The correct dispersion is 1.0/numberOfRows, but since
				 * the relation row count can get updated without
				 * recomputing dispersion, we want to store a
				 * "symbolic" value and figure 1.0/numberOfRows on the
				 * fly.
				 */
				selratio = -1;
			}
			else
			{
				if (VacAttrStatsLtGtValid(stats) &&
				stats->min_cnt + stats->max_cnt == stats->nonnull_cnt)
				{

					/*
					 * exact result when there are just 1 or 2
					 * values...
					 */
					double		min_cnt_d = stats->min_cnt,
								max_cnt_d = stats->max_cnt,
								null_cnt_d = stats->null_cnt;
					double		total = ((double) stats->nonnull_cnt) + null_cnt_d;

					selratio = (min_cnt_d * min_cnt_d + max_cnt_d * max_cnt_d + null_cnt_d * null_cnt_d) / (total * total);
				}
				else
				{
					double		most = (double) (stats->best_cnt > stats->null_cnt ? stats->best_cnt : stats->null_cnt);
					double		total = ((double) stats->nonnull_cnt) + ((double) stats->null_cnt);

					/*
					 * we assume count of other values are 20% of best
					 * count in table
					 */
					selratio = (most * most + 0.20 * most * (total - most)) / (total * total);
				}
				/* Make sure calculated values are in-range */
				if (selratio < 0.0)
					selratio = 0.0;
				else if (selratio > 1.0)
					selratio = 1.0;
			}

			/* overwrite the existing statistics in the tuple */
			attp->attdispersion = selratio;

			/* invalidate the tuple in the cache and write the buffer */
			RelationInvalidateHeapTuple(ad, atup);
			WriteNoReleaseBuffer(scan->rs_cbuf);

			/*
			 * Create pg_statistic tuples for the relation, if we have
			 * gathered the right data.  del_stats() previously
			 * deleted all the pg_statistic tuples for the rel, so we
			 * just have to insert new ones here.
			 *
			 * Note analyze_rel() has seen to it that we won't come here
			 * when vacuuming pg_statistic itself.
			 */
			if (VacAttrStatsLtGtValid(stats) && stats->initialized)
			{
				float4		nullratio;
				float4		bestratio;
				FmgrInfo	out_function;
				char	   *out_string;
				double		best_cnt_d = stats->best_cnt,
							null_cnt_d = stats->null_cnt,
							nonnull_cnt_d = stats->nonnull_cnt;		/* prevent overflow */
				Datum		values[Natts_pg_statistic];
				char		nulls[Natts_pg_statistic];
				Relation	irelations[Num_pg_statistic_indices];

				nullratio = null_cnt_d / (nonnull_cnt_d + null_cnt_d);
				bestratio = best_cnt_d / (nonnull_cnt_d + null_cnt_d);

				fmgr_info(stats->outfunc, &out_function);

				for (i = 0; i < Natts_pg_statistic; ++i)
					nulls[i] = ' ';

				/* ----------------
				 *	initialize values[]
				 * ----------------
				 */
				i = 0;
				values[i++] = ObjectIdGetDatum(relid);		/* starelid */
				values[i++] = Int16GetDatum(attp->attnum);	/* staattnum */
				values[i++] = ObjectIdGetDatum(stats->op_cmplt); /* staop */
				values[i++] = Float4GetDatum(nullratio);	/* stanullfrac */
				values[i++] = Float4GetDatum(bestratio);	/* stacommonfrac */
				out_string = DatumGetCString(FunctionCall3(&out_function,
											 stats->best,
											 ObjectIdGetDatum(stats->typelem),
											 Int32GetDatum(stats->attr->atttypmod)));
				values[i++] = DirectFunctionCall1(textin,	/* stacommonval */
												  CStringGetDatum(out_string));
				pfree(out_string);
				out_string = DatumGetCString(FunctionCall3(&out_function,
											 stats->min,
											 ObjectIdGetDatum(stats->typelem),
											 Int32GetDatum(stats->attr->atttypmod)));
				values[i++] = DirectFunctionCall1(textin,	/* staloval */
												  CStringGetDatum(out_string));
				pfree(out_string);
				out_string = DatumGetCString(FunctionCall3(&out_function,
											 stats->max,
											 ObjectIdGetDatum(stats->typelem),
											 Int32GetDatum(stats->attr->atttypmod)));
				values[i++] = DirectFunctionCall1(textin,	/* stahival */
												  CStringGetDatum(out_string));
				pfree(out_string);

				stup = heap_formtuple(sd->rd_att, values, nulls);

				/* store tuple and update indexes too */
				heap_insert(sd, stup);

				CatalogOpenIndices(Num_pg_statistic_indices, Name_pg_statistic_indices, irelations);
				CatalogIndexInsert(irelations, Num_pg_statistic_indices, sd, stup);
				CatalogCloseIndices(Num_pg_statistic_indices, irelations);

				/* release allocated space */
				pfree(DatumGetPointer(values[Anum_pg_statistic_stacommonval - 1]));
				pfree(DatumGetPointer(values[Anum_pg_statistic_staloval - 1]));
				pfree(DatumGetPointer(values[Anum_pg_statistic_stahival - 1]));
				heap_freetuple(stup);
			}
		}
	}
	heap_endscan(scan);
	/* close rels, but hold locks till upcoming commit */
	heap_close(ad, NoLock);
	heap_close(sd, NoLock);
}

/*
 *	del_stats() -- delete pg_statistic rows for a relation
 *
 *	If a list of attribute numbers is given, only zap stats for those attrs.
 */
static void
del_stats(Oid relid, int attcnt, int *attnums)
{
	Relation	pgstatistic;
	HeapScanDesc scan;
	HeapTuple	tuple;
	ScanKeyData key;

	pgstatistic = heap_openr(StatisticRelationName, RowExclusiveLock);

	ScanKeyEntryInitialize(&key, 0x0, Anum_pg_statistic_starelid,
						   F_OIDEQ, ObjectIdGetDatum(relid));
	scan = heap_beginscan(pgstatistic, false, SnapshotNow, 1, &key);

	while (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
	{
		if (attcnt > 0)
		{
			Form_pg_statistic pgs = (Form_pg_statistic) GETSTRUCT(tuple);
			int			i;

			for (i = 0; i < attcnt; i++)
			{
				if (pgs->staattnum == attnums[i] + 1)
					break;
			}
			if (i >= attcnt)
				continue;		/* don't delete it */
		}
		heap_delete(pgstatistic, &tuple->t_self, NULL);
	}

	heap_endscan(scan);

	/*
	 * Close rel, but *keep* lock; we will need to reacquire it later, so
	 * there's a possibility of deadlock against another VACUUM process if
	 * we let go now.  Keeping the lock shouldn't delay any common
	 * operation other than an attempted VACUUM of pg_statistic itself.
	 */
	heap_close(pgstatistic, NoLock);
}



