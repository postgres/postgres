/*-------------------------------------------------------------------------
 *
 * analyze.c
 *	  the postgres optimizer analyzer
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/analyze.c,v 1.17 2001/05/07 00:43:17 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/heapam.h"
#include "access/tuptoaster.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "parser/parse_oper.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"
#include "utils/tuplesort.h"


/*
 * Analysis algorithms supported
 */
typedef enum {
	ALG_MINIMAL = 1,			/* Compute only most-common-values */
	ALG_SCALAR					/* Compute MCV, histogram, sort correlation */
} AlgCode;

/*
 * To avoid consuming too much memory during analysis and/or too much space
 * in the resulting pg_statistic rows, we ignore varlena datums that are wider
 * than WIDTH_THRESHOLD (after detoasting!).  This is legitimate for MCV
 * and distinct-value calculations since a wide value is unlikely to be
 * duplicated at all, much less be a most-common value.  For the same reason,
 * ignoring wide values will not affect our estimates of histogram bin
 * boundaries very much.
 */
#define WIDTH_THRESHOLD  256

/*
 * We build one of these structs for each attribute (column) that is to be
 * analyzed.  The struct and subsidiary data are in TransactionCommandContext,
 * so they live until the end of the ANALYZE operation.
 */
typedef struct
{
	/* These fields are set up by examine_attribute */
	int			attnum;			/* attribute number */
	AlgCode		algcode;		/* Which algorithm to use for this column */
	int			minrows;		/* Minimum # of rows needed for stats */
	Form_pg_attribute attr;		/* copy of pg_attribute row for column */
	Form_pg_type attrtype;		/* copy of pg_type row for column */
	Oid			eqopr;			/* '=' operator for datatype, if any */
	Oid			eqfunc;			/* and associated function */
	Oid			ltopr;			/* '<' operator for datatype, if any */

	/* These fields are filled in by the actual statistics-gathering routine */
	bool		stats_valid;
	float4		stanullfrac;	/* fraction of entries that are NULL */
	int4		stawidth;		/* average width */
	float4		stadistinct;	/* # distinct values */
	int2		stakind[STATISTIC_NUM_SLOTS];
	Oid			staop[STATISTIC_NUM_SLOTS];
	int			numnumbers[STATISTIC_NUM_SLOTS];
	float4	   *stanumbers[STATISTIC_NUM_SLOTS];
	int			numvalues[STATISTIC_NUM_SLOTS];
	Datum	   *stavalues[STATISTIC_NUM_SLOTS];
} VacAttrStats;


typedef struct
{
	Datum		value;			/* a data value */
	int			tupno;			/* position index for tuple it came from */
} ScalarItem;

typedef struct
{
	int			count;			/* # of duplicates */
	int			first;			/* values[] index of first occurrence */
} ScalarMCVItem;


#define swapInt(a,b)	{int _tmp; _tmp=a; a=b; b=_tmp;}
#define swapDatum(a,b)	{Datum _tmp; _tmp=a; a=b; b=_tmp;}


static int MESSAGE_LEVEL;

/* context information for compare_scalars() */
static FmgrInfo *datumCmpFn;
static SortFunctionKind datumCmpFnKind;
static int *datumCmpTupnoLink;


static VacAttrStats *examine_attribute(Relation onerel, int attnum);
static int acquire_sample_rows(Relation onerel, HeapTuple *rows,
							   int targrows, long *totalrows);
static double random_fract(void);
static double init_selection_state(int n);
static long select_next_random_record(long t, int n, double *stateptr);
static int compare_rows(const void *a, const void *b);
static int compare_scalars(const void *a, const void *b);
static int compare_mcvs(const void *a, const void *b);
static OffsetNumber get_page_max_offset(Relation relation,
										BlockNumber blocknumber);
static void compute_minimal_stats(VacAttrStats *stats,
								  TupleDesc tupDesc, long totalrows,
								  HeapTuple *rows, int numrows);
static void compute_scalar_stats(VacAttrStats *stats,
								 TupleDesc tupDesc, long totalrows,
								 HeapTuple *rows, int numrows);
static void update_attstats(Oid relid, int natts, VacAttrStats **vacattrstats);


/*
 *	analyze_rel() -- analyze one relation
 */
void
analyze_rel(Oid relid, VacuumStmt *vacstmt)
{
	Relation	onerel;
	Form_pg_attribute *attr;
	int			attr_cnt,
				tcnt,
				i;
	VacAttrStats **vacattrstats;
	int			targrows,
				numrows;
	long		totalrows;
	HeapTuple  *rows;
	HeapTuple	tuple;

	if (vacstmt->verbose)
		MESSAGE_LEVEL = NOTICE;
	else
		MESSAGE_LEVEL = DEBUG;

	/*
	 * Begin a transaction for analyzing this relation.
	 *
	 * Note: All memory allocated during ANALYZE will live in
	 * TransactionCommandContext or a subcontext thereof, so it will
	 * all be released by transaction commit at the end of this routine.
	 */
	StartTransactionCommand();

	/*
	 * Check for user-requested abort.	Note we want this to be inside a
	 * transaction, so xact.c doesn't issue useless NOTICE.
	 */
	CHECK_FOR_INTERRUPTS();

	/*
	 * Race condition -- if the pg_class tuple has gone away since the
	 * last time we saw it, we don't need to process it.
	 */
	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(relid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
	{
		CommitTransactionCommand();
		return;
	}

	/*
	 * We can ANALYZE any table except pg_statistic. See update_attstats
	 */
	if (strcmp(NameStr(((Form_pg_class) GETSTRUCT(tuple))->relname),
			   StatisticRelationName) == 0)
	{
		ReleaseSysCache(tuple);
		CommitTransactionCommand();
		return;
	}
	ReleaseSysCache(tuple);

	/*
	 * Open the class, getting only a read lock on it, and check permissions
	 */
	onerel = heap_open(relid, AccessShareLock);

	if (!pg_ownercheck(GetUserId(), RelationGetRelationName(onerel),
					   RELNAME))
	{
		/* No need for a notice if we already complained during VACUUM */
		if (!vacstmt->vacuum)
			elog(NOTICE, "Skipping \"%s\" --- only table owner can ANALYZE it",
				 RelationGetRelationName(onerel));
		heap_close(onerel, NoLock);
		CommitTransactionCommand();
		return;
	}

	elog(MESSAGE_LEVEL, "Analyzing %s", RelationGetRelationName(onerel));

	/*
	 * Determine which columns to analyze
	 *
	 * Note that system attributes are never analyzed.
	 */
	attr = onerel->rd_att->attrs;
	attr_cnt = onerel->rd_att->natts;

	if (vacstmt->va_cols != NIL)
	{
		List	   *le;

		vacattrstats = (VacAttrStats **) palloc(length(vacstmt->va_cols) *
												sizeof(VacAttrStats *));
		tcnt = 0;
		foreach(le, vacstmt->va_cols)
		{
			char	   *col = strVal(lfirst(le));

			for (i = 0; i < attr_cnt; i++)
			{
				if (namestrcmp(&(attr[i]->attname), col) == 0)
					break;
			}
			if (i >= attr_cnt)
				elog(ERROR, "ANALYZE: there is no attribute %s in %s",
					 col, RelationGetRelationName(onerel));
			vacattrstats[tcnt] = examine_attribute(onerel, i+1);
			if (vacattrstats[tcnt] != NULL)
				tcnt++;
		}
		attr_cnt = tcnt;
	}
	else
	{
		vacattrstats = (VacAttrStats **) palloc(attr_cnt *
												sizeof(VacAttrStats *));
		tcnt = 0;
		for (i = 0; i < attr_cnt; i++)
		{
			vacattrstats[tcnt] = examine_attribute(onerel, i+1);
			if (vacattrstats[tcnt] != NULL)
				tcnt++;
		}
		attr_cnt = tcnt;
	}

	/*
	 * Quit if no analyzable columns
	 */
	if (attr_cnt <= 0)
	{
		heap_close(onerel, NoLock);
		CommitTransactionCommand();
		return;
	}

	/*
	 * Determine how many rows we need to sample, using the worst case
	 * from all analyzable columns.  We use a lower bound of 100 rows
	 * to avoid possible overflow in Vitter's algorithm.
	 */
	targrows = 100;
	for (i = 0; i < attr_cnt; i++)
	{
		if (targrows < vacattrstats[i]->minrows)
			targrows = vacattrstats[i]->minrows;
	}

	/*
	 * Acquire the sample rows
	 */
	rows = (HeapTuple *) palloc(targrows * sizeof(HeapTuple));
	numrows = acquire_sample_rows(onerel, rows, targrows, &totalrows);

	/*
	 * If we are running a standalone ANALYZE, update pages/tuples stats
	 * in pg_class.  We have the accurate page count from heap_beginscan,
	 * but only an approximate number of tuples; therefore, if we are
	 * part of VACUUM ANALYZE do *not* overwrite the accurate count already
	 * inserted by VACUUM.
	 */
	if (!vacstmt->vacuum)
		vac_update_relstats(RelationGetRelid(onerel),
							onerel->rd_nblocks,
							(double) totalrows,
							RelationGetForm(onerel)->relhasindex);

	/*
	 * Compute the statistics.  Temporary results during the calculations
	 * for each column are stored in a child context.  The calc routines
	 * are responsible to make sure that whatever they store into the
	 * VacAttrStats structure is allocated in TransactionCommandContext.
	 */
	if (numrows > 0)
	{
		MemoryContext col_context,
					old_context;

		col_context = AllocSetContextCreate(CurrentMemoryContext,
											"Analyze Column",
											ALLOCSET_DEFAULT_MINSIZE,
											ALLOCSET_DEFAULT_INITSIZE,
											ALLOCSET_DEFAULT_MAXSIZE);
		old_context = MemoryContextSwitchTo(col_context);
		for (i = 0; i < attr_cnt; i++)
		{
			switch (vacattrstats[i]->algcode)
			{
				case ALG_MINIMAL:
					compute_minimal_stats(vacattrstats[i],
										  onerel->rd_att, totalrows,
										  rows, numrows);
					break;
				case ALG_SCALAR:
					compute_scalar_stats(vacattrstats[i],
										 onerel->rd_att, totalrows,
										 rows, numrows);
					break;
			}
			MemoryContextResetAndDeleteChildren(col_context);
		}
		MemoryContextSwitchTo(old_context);
		MemoryContextDelete(col_context);

		/*
		 * Emit the completed stats rows into pg_statistic, replacing any
		 * previous statistics for the target columns.  (If there are stats
		 * in pg_statistic for columns we didn't process, we leave them alone.)
		 */
		update_attstats(relid, attr_cnt, vacattrstats);
	}

	/*
	 * Close source relation now, but keep lock so that no one deletes it
	 * before we commit.  (If someone did, they'd fail to clean up the
	 * entries we made in pg_statistic.)
	 */
	heap_close(onerel, NoLock);

	/* Commit and release working memory */
	CommitTransactionCommand();
}

/*
 * examine_attribute -- pre-analysis of a single column
 *
 * Determine whether the column is analyzable; if so, create and initialize
 * a VacAttrStats struct for it.  If not, return NULL.
 */
static VacAttrStats *
examine_attribute(Relation onerel, int attnum)
{
	Form_pg_attribute attr = onerel->rd_att->attrs[attnum-1];
	Operator	func_operator;
	Oid			oprrest;
	HeapTuple	typtuple;
	Oid			eqopr = InvalidOid;
	Oid			eqfunc = InvalidOid;
	Oid			ltopr = InvalidOid;
	VacAttrStats *stats;

	/* Don't analyze column if user has specified not to */
	if (attr->attstattarget <= 0)
		return NULL;

	/* If column has no "=" operator, we can't do much of anything */
	func_operator = compatible_oper("=",
									attr->atttypid,
									attr->atttypid,
									true);
	if (func_operator != NULL)
	{
		oprrest = ((Form_pg_operator) GETSTRUCT(func_operator))->oprrest;
		if (oprrest == F_EQSEL)
		{
			eqopr = oprid(func_operator);
			eqfunc = oprfuncid(func_operator);
		}
		ReleaseSysCache(func_operator);
	}
	if (!OidIsValid(eqfunc))
		return NULL;

	/*
	 * If we have "=" then we're at least able to do the minimal algorithm,
	 * so start filling in a VacAttrStats struct.
	 */
	stats = (VacAttrStats *) palloc(sizeof(VacAttrStats));
	MemSet(stats, 0, sizeof(VacAttrStats));
	stats->attnum = attnum;
	stats->attr = (Form_pg_attribute) palloc(ATTRIBUTE_TUPLE_SIZE);
	memcpy(stats->attr, attr, ATTRIBUTE_TUPLE_SIZE);
	typtuple = SearchSysCache(TYPEOID,
							  ObjectIdGetDatum(attr->atttypid),
							  0, 0, 0);
	if (!HeapTupleIsValid(typtuple))
		elog(ERROR, "cache lookup of type %u failed", attr->atttypid);
	stats->attrtype = (Form_pg_type) palloc(sizeof(FormData_pg_type));
	memcpy(stats->attrtype, GETSTRUCT(typtuple), sizeof(FormData_pg_type));
	ReleaseSysCache(typtuple);
	stats->eqopr = eqopr;
	stats->eqfunc = eqfunc;

	/* Is there a "<" operator with suitable semantics? */
	func_operator = compatible_oper("<",
									attr->atttypid,
									attr->atttypid,
									true);
	if (func_operator != NULL)
	{
		oprrest = ((Form_pg_operator) GETSTRUCT(func_operator))->oprrest;
		if (oprrest == F_SCALARLTSEL)
		{
			ltopr = oprid(func_operator);
		}
		ReleaseSysCache(func_operator);
	}
	stats->ltopr = ltopr;

	/*
	 * Determine the algorithm to use (this will get more complicated later)
	 */
	if (OidIsValid(ltopr))
	{
		/* Seems to be a scalar datatype */
		stats->algcode = ALG_SCALAR;
		/*--------------------
		 * The following choice of minrows is based on the paper
		 * "Random sampling for histogram construction: how much is enough?"
		 * by Surajit Chaudhuri, Rajeev Motwani and Vivek Narasayya, in
		 * Proceedings of ACM SIGMOD International Conference on Management
		 * of Data, 1998, Pages 436-447.  Their Corollary 1 to Theorem 5
		 * says that for table size n, histogram size k, maximum relative
		 * error in bin size f, and error probability gamma, the minimum
		 * random sample size is
		 *		r = 4 * k * ln(2*n/gamma) / f^2
		 * Taking f = 0.5, gamma = 0.01, n = 1 million rows, we obtain
		 *		r = 305.82 * k
		 * Note that because of the log function, the dependence on n is
		 * quite weak; even at n = 1 billion, a 300*k sample gives <= 0.59
		 * bin size error with probability 0.99.  So there's no real need to
		 * scale for n, which is a good thing because we don't necessarily
		 * know it at this point.
		 *--------------------
		 */
		stats->minrows = 300 * attr->attstattarget;
	}
	else
	{
		/* Can't do much but the minimal stuff */
		stats->algcode = ALG_MINIMAL;
		/* Might as well use the same minrows as above */
		stats->minrows = 300 * attr->attstattarget;
	}

	return stats;
}

/*
 * acquire_sample_rows -- acquire a random sample of rows from the table
 *
 * Up to targrows rows are collected (if there are fewer than that many
 * rows in the table, all rows are collected).  When the table is larger
 * than targrows, a truly random sample is collected: every row has an
 * equal chance of ending up in the final sample.
 *
 * We also estimate the total number of rows in the table, and return that
 * into *totalrows.
 *
 * The returned list of tuples is in order by physical position in the table.
 * (We will rely on this later to derive correlation estimates.)
 */
static int
acquire_sample_rows(Relation onerel, HeapTuple *rows, int targrows,
					long *totalrows)
{
	int			numrows = 0;
	HeapScanDesc scan;
	HeapTuple	tuple;
	ItemPointer	lasttuple;
	BlockNumber	lastblock,
				estblock;
	OffsetNumber lastoffset;
	int			numest;
	double		tuplesperpage;
	long		t;
	double		rstate;

	Assert(targrows > 1);
	/*
	 * Do a simple linear scan until we reach the target number of rows.
	 */
	scan = heap_beginscan(onerel, false, SnapshotNow, 0, NULL);
	while (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
	{
		rows[numrows++] = heap_copytuple(tuple);
		if (numrows >= targrows)
			break;
	}
	heap_endscan(scan);
	/*
	 * If we ran out of tuples then we're done, no matter how few we 
	 * collected.  No sort is needed, since they're already in order.
	 */
	if (!HeapTupleIsValid(tuple))
	{
		*totalrows = numrows;
		return numrows;
	}
	/*
	 * Otherwise, start replacing tuples in the sample until we reach the
	 * end of the relation.  This algorithm is from Jeff Vitter's paper
	 * (see full citation below).  It works by repeatedly computing the number
	 * of the next tuple we want to fetch, which will replace a randomly
	 * chosen element of the reservoir (current set of tuples).  At all times
	 * the reservoir is a true random sample of the tuples we've passed over
	 * so far, so when we fall off the end of the relation we're done.
	 *
	 * A slight difficulty is that since we don't want to fetch tuples or even
	 * pages that we skip over, it's not possible to fetch *exactly* the N'th
	 * tuple at each step --- we don't know how many valid tuples are on
	 * the skipped pages.  We handle this by assuming that the average number
	 * of valid tuples/page on the pages already scanned over holds good for
	 * the rest of the relation as well; this lets us estimate which page
	 * the next tuple should be on and its position in the page.  Then we
	 * fetch the first valid tuple at or after that position, being careful
	 * not to use the same tuple twice.  This approach should still give a
	 * good random sample, although it's not perfect.
	 */
	lasttuple = &(rows[numrows-1]->t_self);
	lastblock = ItemPointerGetBlockNumber(lasttuple);
	lastoffset = ItemPointerGetOffsetNumber(lasttuple);
	/*
	 * If possible, estimate tuples/page using only completely-scanned pages.
	 */
	for (numest = numrows; numest > 0; numest--)
	{
		if (ItemPointerGetBlockNumber(&(rows[numest-1]->t_self)) != lastblock)
			break;
	}
	if (numest == 0)
	{
		numest = numrows;		/* don't have a full page? */
		estblock = lastblock + 1;
	}
	else
	{
		estblock = lastblock;
	}
	tuplesperpage = (double) numest / (double) estblock;

	t = numrows;				/* t is the # of records processed so far */
	rstate = init_selection_state(targrows);
	for (;;)
	{
		double			targpos;
		BlockNumber		targblock;
		OffsetNumber	targoffset,
						maxoffset;

		t = select_next_random_record(t, targrows, &rstate);
		/* Try to read the t'th record in the table */
		targpos = (double) t / tuplesperpage;
		targblock = (BlockNumber) targpos;
		targoffset = ((int) (targpos - targblock) * tuplesperpage) + 
			FirstOffsetNumber;
		/* Make sure we are past the last selected record */
		if (targblock <= lastblock)
		{
			targblock = lastblock;
			if (targoffset <= lastoffset)
				targoffset = lastoffset + 1;
		}
		/* Loop to find first valid record at or after given position */
	pageloop:;
		/*
		 * Have we fallen off the end of the relation?  (We rely on
		 * heap_beginscan to have updated rd_nblocks.)
		 */
		if (targblock >= onerel->rd_nblocks)
			break;
		maxoffset = get_page_max_offset(onerel, targblock);
		for (;;)
		{
			HeapTupleData targtuple;
			Buffer		targbuffer;

			if (targoffset > maxoffset)
			{
				/* Fell off end of this page, try next */
				targblock++;
				targoffset = FirstOffsetNumber;
				goto pageloop;
			}
			ItemPointerSet(&targtuple.t_self, targblock, targoffset);
			heap_fetch(onerel, SnapshotNow, &targtuple, &targbuffer);
			if (targtuple.t_data != NULL)
			{
				/*
				 * Found a suitable tuple, so save it, replacing one old
				 * tuple at random
				 */
				int		k = (int) (targrows * random_fract());

				Assert(k >= 0 && k < targrows);
				heap_freetuple(rows[k]);
				rows[k] = heap_copytuple(&targtuple);
				ReleaseBuffer(targbuffer);
				lastblock = targblock;
				lastoffset = targoffset;
				break;
			}
			/* this tuple is dead, so advance to next one on same page */
			targoffset++;
		}
	}

	/*
	 * Now we need to sort the collected tuples by position (itempointer).
	 */
	qsort((void *) rows, numrows, sizeof(HeapTuple), compare_rows);

	/*
	 * Estimate total number of valid rows in relation.
	 */
	*totalrows = (long) (onerel->rd_nblocks * tuplesperpage + 0.5);

	return numrows;
}

/* Select a random value R uniformly distributed in 0 < R < 1 */
static double
random_fract(void)
{
	long	z;

	/* random() can produce endpoint values, try again if so */
	do
	{
		z = random();
	} while (! (z > 0 && z < MAX_RANDOM_VALUE));
	return (double) z / (double) MAX_RANDOM_VALUE;
}

/*
 * These two routines embody Algorithm Z from "Random sampling with a
 * reservoir" by Jeffrey S. Vitter, in ACM Trans. Math. Softw. 11, 1
 * (Mar. 1985), Pages 37-57.  While Vitter describes his algorithm in terms
 * of the count S of records to skip before processing another record,
 * it is convenient to work primarily with t, the index (counting from 1)
 * of the last record processed and next record to process.  The only extra
 * state needed between calls is W, a random state variable.
 *
 * init_selection_state computes the initial W value.
 *
 * Given that we've already processed t records (t >= n),
 * select_next_random_record determines the number of the next record to
 * process.
 */
static double
init_selection_state(int n)
{
	/* Initial value of W (for use when Algorithm Z is first applied) */
	return exp(- log(random_fract())/n);
}

static long
select_next_random_record(long t, int n, double *stateptr)
{
	/* The magic constant here is T from Vitter's paper */
	if (t <= (22 * n))
	{
		/* Process records using Algorithm X until t is large enough */
		double	V,
				quot;

		V = random_fract();		/* Generate V */
		t++;
		quot = (double) (t - n) / (double) t;
		/* Find min S satisfying (4.1) */
		while (quot > V)
		{
			t++;
			quot *= (double) (t - n) / (double) t;
		}
	}
	else
	{
		/* Now apply Algorithm Z */
		double	W = *stateptr;
		long	term = t - n + 1;
		int		S;

		for (;;)
		{
			long	numer,
					numer_lim,
					denom;
			double	U,
					X,
					lhs,
					rhs,
					y,
					tmp;

			/* Generate U and X */
			U = random_fract();
			X = t * (W - 1.0);
			S = X;				/* S is tentatively set to floor(X) */
			/* Test if U <= h(S)/cg(X) in the manner of (6.3) */
			tmp = (double) (t + 1) / (double) term;
			lhs = exp(log(((U * tmp * tmp) * (term + S))/(t + X))/n);
			rhs = (((t + X)/(term + S)) * term)/t;
			if (lhs <= rhs)
			{
				W = rhs/lhs;
				break;
			}
			/* Test if U <= f(S)/cg(X) */
			y = (((U * (t + 1))/term) * (t + S + 1))/(t + X);
			if (n < S)
			{
				denom = t;
				numer_lim = term + S;
			}
			else
			{
				denom = t - n + S;
				numer_lim = t + 1;
			}
			for (numer = t + S; numer >= numer_lim; numer--)
			{
				y *= (double) numer / (double) denom;
				denom--;
			}
			W = exp(- log(random_fract())/n); /* Generate W in advance */
			if (exp(log(y)/n) <= (t + X)/t)
				break;
		}
		t += S + 1;
		*stateptr = W;
	}
	return t;
}

/*
 * qsort comparator for sorting rows[] array
 */
static int
compare_rows(const void *a, const void *b)
{
	HeapTuple	ha = * (HeapTuple *) a;
	HeapTuple	hb = * (HeapTuple *) b;
	BlockNumber	ba = ItemPointerGetBlockNumber(&ha->t_self);
	OffsetNumber oa = ItemPointerGetOffsetNumber(&ha->t_self);
	BlockNumber	bb = ItemPointerGetBlockNumber(&hb->t_self);
	OffsetNumber ob = ItemPointerGetOffsetNumber(&hb->t_self);

	if (ba < bb)
		return -1;
	if (ba > bb)
		return 1;
	if (oa < ob)
		return -1;
	if (oa > ob)
		return 1;
	return 0;
}

/*
 * Discover the largest valid tuple offset number on the given page
 *
 * This code probably ought to live in some other module.
 */
static OffsetNumber
get_page_max_offset(Relation relation, BlockNumber blocknumber)
{
	Buffer		buffer;
	Page		p;
	OffsetNumber offnum;

	buffer = ReadBuffer(relation, blocknumber);
	if (!BufferIsValid(buffer))
		elog(ERROR, "get_page_max_offset: %s relation: ReadBuffer(%ld) failed",
			 RelationGetRelationName(relation), (long) blocknumber);
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	p = BufferGetPage(buffer);
	offnum = PageGetMaxOffsetNumber(p);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	ReleaseBuffer(buffer);
	return offnum;
}


/*
 *	compute_minimal_stats() -- compute minimal column statistics
 *
 *	We use this when we can find only an "=" operator for the datatype.
 *
 *	We determine the fraction of non-null rows, the average width, the
 *	most common values, and the (estimated) number of distinct values.
 *
 *	The most common values are determined by brute force: we keep a list
 *	of previously seen values, ordered by number of times seen, as we scan
 *	the samples.  A newly seen value is inserted just after the last
 *	multiply-seen value, causing the bottommost (oldest) singly-seen value
 *	to drop off the list.  The accuracy of this method, and also its cost,
 *	depend mainly on the length of the list we are willing to keep.
 */
static void
compute_minimal_stats(VacAttrStats *stats,
					  TupleDesc tupDesc, long totalrows,
					  HeapTuple *rows, int numrows)
{
	int			i;
	int			null_cnt = 0;
	int			nonnull_cnt = 0;
	int			toowide_cnt = 0;
	double		total_width = 0;
	bool		is_varlena = (!stats->attr->attbyval &&
							  stats->attr->attlen == -1);
	FmgrInfo	f_cmpeq;
	typedef struct
	{
		Datum	value;
		int		count;
	} TrackItem;
	TrackItem  *track;
	int			track_cnt,
				track_max;
	int			num_mcv = stats->attr->attstattarget;

	/* We track up to 2*n values for an n-element MCV list; but at least 10 */
	track_max = 2 * num_mcv;
	if (track_max < 10)
		track_max = 10;
	track = (TrackItem *) palloc(track_max * sizeof(TrackItem));
	track_cnt = 0;

	fmgr_info(stats->eqfunc, &f_cmpeq);

	for (i = 0; i < numrows; i++)
	{
		HeapTuple	tuple = rows[i];
		Datum		value;
		bool		isnull;
		bool		match;
		int			firstcount1,
					j;

		value = heap_getattr(tuple, stats->attnum, tupDesc, &isnull);

		/* Check for null/nonnull */
		if (isnull)
		{
			null_cnt++;
			continue;
		}
		nonnull_cnt++;

		/*
		 * If it's a varlena field, add up widths for average width
		 * calculation.  Note that if the value is toasted, we
		 * use the toasted width.  We don't bother with this calculation
		 * if it's a fixed-width type.
		 */
		if (is_varlena)
		{
			total_width += VARSIZE(DatumGetPointer(value));
			/*
			 * If the value is toasted, we want to detoast it just once to
			 * avoid repeated detoastings and resultant excess memory usage
			 * during the comparisons.  Also, check to see if the value is
			 * excessively wide, and if so don't detoast at all --- just
			 * ignore the value.
			 */
			if (toast_raw_datum_size(value) > WIDTH_THRESHOLD)
			{
				toowide_cnt++;
				continue;
			}
			value = PointerGetDatum(PG_DETOAST_DATUM(value));
		}

		/*
		 * See if the value matches anything we're already tracking.
		 */
		match = false;
		firstcount1 = track_cnt;
		for (j = 0; j < track_cnt; j++)
		{
			if (DatumGetBool(FunctionCall2(&f_cmpeq, value, track[j].value)))
			{
				match = true;
				break;
			}
			if (j < firstcount1 && track[j].count == 1)
				firstcount1 = j;
		}

		if (match)
		{
			/* Found a match */
			track[j].count++;
			/* This value may now need to "bubble up" in the track list */
			while (j > 0 && track[j].count > track[j-1].count)
			{
				swapDatum(track[j].value, track[j-1].value);
				swapInt(track[j].count, track[j-1].count);
				j--;
			}
		}
		else
		{
			/* No match.  Insert at head of count-1 list */
			if (track_cnt < track_max)
				track_cnt++;
			for (j = track_cnt-1; j > firstcount1; j--)
			{
				track[j].value = track[j-1].value;
				track[j].count = track[j-1].count;
			}
			if (firstcount1 < track_cnt)
			{
				track[firstcount1].value = value;
				track[firstcount1].count = 1;
			}
		}
	}

	/* We can only compute valid stats if we found some non-null values. */
	if (nonnull_cnt > 0)
	{
		int		nmultiple,
				summultiple;

		stats->stats_valid = true;
		/* Do the simple null-frac and width stats */
		stats->stanullfrac = (double) null_cnt / (double) numrows;
		if (is_varlena)
			stats->stawidth = total_width / (double) nonnull_cnt;
		else
			stats->stawidth = stats->attrtype->typlen;

		/* Count the number of values we found multiple times */
		summultiple = 0;
		for (nmultiple = 0; nmultiple < track_cnt; nmultiple++)
		{
			if (track[nmultiple].count == 1)
				break;
			summultiple += track[nmultiple].count;
		}

		if (nmultiple == 0)
		{
			/* If we found no repeated values, assume it's a unique column */
			stats->stadistinct = -1.0;
		}
		else if (track_cnt < track_max && toowide_cnt == 0 &&
				 nmultiple == track_cnt)
		{
			/*
			 * Our track list includes every value in the sample, and every
			 * value appeared more than once.  Assume the column has just
			 * these values.
			 */
			stats->stadistinct = track_cnt;
		}
		else
		{
			/*----------
			 * Estimate the number of distinct values using the estimator
			 * proposed by Chaudhuri et al (see citation above).  This is
			 *		sqrt(n/r) * max(f1,1) + f2 + f3 + ...
			 * where fk is the number of distinct values that occurred
			 * exactly k times in our sample of r rows (from a total of n).
			 * We assume (not very reliably!) that all the multiply-occurring
			 * values are reflected in the final track[] list, and the other
			 * nonnull values all appeared but once.
			 *----------
			 */
			int		f1 = nonnull_cnt - summultiple;
			double	term1;

			if (f1 < 1)
				f1 = 1;
			term1 = sqrt((double) totalrows / (double) numrows) * f1;
			stats->stadistinct = floor(term1 + nmultiple + 0.5);
		}

		/*
		 * If we estimated the number of distinct values at more than 10%
		 * of the total row count (a very arbitrary limit), then assume
		 * that stadistinct should scale with the row count rather than be
		 * a fixed value.
		 */
		if (stats->stadistinct > 0.1 * totalrows)
			stats->stadistinct = - (stats->stadistinct / totalrows);

		/* Generate an MCV slot entry, only if we found multiples */
		if (nmultiple < num_mcv)
			num_mcv = nmultiple;
		if (num_mcv > 0)
		{
			MemoryContext old_context;
			Datum  *mcv_values;
			float4 *mcv_freqs;

			/* Must copy the target values into TransactionCommandContext */
			old_context = MemoryContextSwitchTo(TransactionCommandContext);
			mcv_values = (Datum *) palloc(num_mcv * sizeof(Datum));
			mcv_freqs = (float4 *) palloc(num_mcv * sizeof(float4));
			for (i = 0; i < num_mcv; i++)
			{
				mcv_values[i] = datumCopy(track[i].value,
										  stats->attr->attbyval,
										  stats->attr->attlen);
				mcv_freqs[i] = (double) track[i].count / (double) numrows;
			}
			MemoryContextSwitchTo(old_context);

			stats->stakind[0] = STATISTIC_KIND_MCV;
			stats->staop[0] = stats->eqopr;
			stats->stanumbers[0] = mcv_freqs;
			stats->numnumbers[0] = num_mcv;
			stats->stavalues[0] = mcv_values;
			stats->numvalues[0] = num_mcv;
		}
	}

	/* We don't need to bother cleaning up any of our temporary palloc's */
}


/*
 *	compute_scalar_stats() -- compute column statistics
 *
 *	We use this when we can find "=" and "<" operators for the datatype.
 *
 *	We determine the fraction of non-null rows, the average width, the
 *	most common values, the (estimated) number of distinct values, the
 *	distribution histogram, and the correlation of physical to logical order.
 *
 *	The desired stats can be determined fairly easily after sorting the
 *	data values into order.
 */
static void
compute_scalar_stats(VacAttrStats *stats,
					 TupleDesc tupDesc, long totalrows,
					 HeapTuple *rows, int numrows)
{
	int			i;
	int			null_cnt = 0;
	int			nonnull_cnt = 0;
	int			toowide_cnt = 0;
	double		total_width = 0;
	bool		is_varlena = (!stats->attr->attbyval &&
							  stats->attr->attlen == -1);
	double		corr_xysum;
	RegProcedure cmpFn;
	SortFunctionKind cmpFnKind;
	FmgrInfo	f_cmpfn;
	ScalarItem *values;
	int			values_cnt = 0;
	int		   *tupnoLink;
	ScalarMCVItem *track;
	int			track_cnt = 0;
	int			num_mcv = stats->attr->attstattarget;

	values = (ScalarItem *) palloc(numrows * sizeof(ScalarItem));
	tupnoLink = (int *) palloc(numrows * sizeof(int));
	track = (ScalarMCVItem *) palloc(num_mcv * sizeof(ScalarMCVItem));

	SelectSortFunction(stats->ltopr, &cmpFn, &cmpFnKind);
	fmgr_info(cmpFn, &f_cmpfn);

	/* Initial scan to find sortable values */
	for (i = 0; i < numrows; i++)
	{
		HeapTuple	tuple = rows[i];
		Datum		value;
		bool		isnull;

		value = heap_getattr(tuple, stats->attnum, tupDesc, &isnull);

		/* Check for null/nonnull */
		if (isnull)
		{
			null_cnt++;
			continue;
		}
		nonnull_cnt++;

		/*
		 * If it's a varlena field, add up widths for average width
		 * calculation.  Note that if the value is toasted, we
		 * use the toasted width.  We don't bother with this calculation
		 * if it's a fixed-width type.
		 */
		if (is_varlena)
		{
			total_width += VARSIZE(DatumGetPointer(value));
			/*
			 * If the value is toasted, we want to detoast it just once to
			 * avoid repeated detoastings and resultant excess memory usage
			 * during the comparisons.  Also, check to see if the value is
			 * excessively wide, and if so don't detoast at all --- just
			 * ignore the value.
			 */
			if (toast_raw_datum_size(value) > WIDTH_THRESHOLD)
			{
				toowide_cnt++;
				continue;
			}
			value = PointerGetDatum(PG_DETOAST_DATUM(value));
		}

		/* Add it to the list to be sorted */
		values[values_cnt].value = value;
		values[values_cnt].tupno = values_cnt;
		tupnoLink[values_cnt] = values_cnt;
		values_cnt++;
	}

	/* We can only compute valid stats if we found some sortable values. */
	if (values_cnt > 0)
	{
		int		ndistinct,		/* # distinct values in sample */
				nmultiple,		/* # that appear multiple times */
				num_hist,
				dups_cnt;
		int		slot_idx = 0;

		/* Sort the collected values */
		datumCmpFn = &f_cmpfn;
		datumCmpFnKind = cmpFnKind;
		datumCmpTupnoLink = tupnoLink;
		qsort((void *) values, values_cnt,
			  sizeof(ScalarItem), compare_scalars);

		/*
		 * Now scan the values in order, find the most common ones,
		 * and also accumulate ordering-correlation statistics.
		 *
		 * To determine which are most common, we first have to count the
		 * number of duplicates of each value.  The duplicates are adjacent
		 * in the sorted list, so a brute-force approach is to compare
		 * successive datum values until we find two that are not equal.
		 * However, that requires N-1 invocations of the datum comparison
		 * routine, which are completely redundant with work that was done
		 * during the sort.  (The sort algorithm must at some point have
		 * compared each pair of items that are adjacent in the sorted order;
		 * otherwise it could not know that it's ordered the pair correctly.)
		 * We exploit this by having compare_scalars remember the highest
		 * tupno index that each ScalarItem has been found equal to.  At the
		 * end of the sort, a ScalarItem's tupnoLink will still point to
		 * itself if and only if it is the last item of its group of
		 * duplicates (since the group will be ordered by tupno).
		 */
		corr_xysum = 0;
		ndistinct = 0;
		nmultiple = 0;
		dups_cnt = 0;
		for (i = 0; i < values_cnt; i++)
		{
			int			tupno = values[i].tupno;

			corr_xysum += (double) i * (double) tupno;
			dups_cnt++;
			if (tupnoLink[tupno] == tupno)
			{
				/* Reached end of duplicates of this value */
				ndistinct++;
				if (dups_cnt > 1)
				{
					nmultiple++;
					if (track_cnt < num_mcv ||
						dups_cnt > track[track_cnt-1].count)
					{
						/*
						 * Found a new item for the mcv list; find its
						 * position, bubbling down old items if needed.
						 * Loop invariant is that j points at an empty/
						 * replaceable slot.
						 */
						int		j;

						if (track_cnt < num_mcv)
							track_cnt++;
						for (j = track_cnt-1; j > 0; j--)
						{
							if (dups_cnt <= track[j-1].count)
								break;
							track[j].count = track[j-1].count;
							track[j].first = track[j-1].first;
						}
						track[j].count = dups_cnt;
						track[j].first = i + 1 - dups_cnt;
					}
				}
				dups_cnt = 0;
			}
		}

		stats->stats_valid = true;
		/* Do the simple null-frac and width stats */
		stats->stanullfrac = (double) null_cnt / (double) numrows;
		if (is_varlena)
			stats->stawidth = total_width / (double) nonnull_cnt;
		else
			stats->stawidth = stats->attrtype->typlen;

		if (nmultiple == 0)
		{
			/* If we found no repeated values, assume it's a unique column */
			stats->stadistinct = -1.0;
		}
		else if (toowide_cnt == 0 && nmultiple == ndistinct)
		{
			/*
			 * Every value in the sample appeared more than once.  Assume the
			 * column has just these values.
			 */
			stats->stadistinct = ndistinct;
		}
		else
		{
			/*----------
			 * Estimate the number of distinct values using the estimator
			 * proposed by Chaudhuri et al (see citation above).  This is
			 *		sqrt(n/r) * max(f1,1) + f2 + f3 + ...
			 * where fk is the number of distinct values that occurred
			 * exactly k times in our sample of r rows (from a total of n).
			 * Overwidth values are assumed to have been distinct.
			 *----------
			 */
			int		f1 = ndistinct - nmultiple + toowide_cnt;
			double	term1;

			if (f1 < 1)
				f1 = 1;
			term1 = sqrt((double) totalrows / (double) numrows) * f1;
			stats->stadistinct = floor(term1 + nmultiple + 0.5);
		}

		/*
		 * If we estimated the number of distinct values at more than 10%
		 * of the total row count (a very arbitrary limit), then assume
		 * that stadistinct should scale with the row count rather than be
		 * a fixed value.
		 */
		if (stats->stadistinct > 0.1 * totalrows)
			stats->stadistinct = - (stats->stadistinct / totalrows);

		/* Generate an MCV slot entry, only if we found multiples */
		if (nmultiple < num_mcv)
			num_mcv = nmultiple;
		Assert(track_cnt >= num_mcv);
		if (num_mcv > 0)
		{
			MemoryContext old_context;
			Datum  *mcv_values;
			float4 *mcv_freqs;

			/* Must copy the target values into TransactionCommandContext */
			old_context = MemoryContextSwitchTo(TransactionCommandContext);
			mcv_values = (Datum *) palloc(num_mcv * sizeof(Datum));
			mcv_freqs = (float4 *) palloc(num_mcv * sizeof(float4));
			for (i = 0; i < num_mcv; i++)
			{
				mcv_values[i] = datumCopy(values[track[i].first].value,
										  stats->attr->attbyval,
										  stats->attr->attlen);
				mcv_freqs[i] = (double) track[i].count / (double) numrows;
			}
			MemoryContextSwitchTo(old_context);

			stats->stakind[slot_idx] = STATISTIC_KIND_MCV;
			stats->staop[slot_idx] = stats->eqopr;
			stats->stanumbers[slot_idx] = mcv_freqs;
			stats->numnumbers[slot_idx] = num_mcv;
			stats->stavalues[slot_idx] = mcv_values;
			stats->numvalues[slot_idx] = num_mcv;
			slot_idx++;
		}

		/*
		 * Generate a histogram slot entry if there are at least two
		 * distinct values not accounted for in the MCV list.  (This
		 * ensures the histogram won't collapse to empty or a singleton.)
		 */
		num_hist = ndistinct - num_mcv;
		if (num_hist > stats->attr->attstattarget)
			num_hist = stats->attr->attstattarget + 1;
		if (num_hist >= 2)
		{
			MemoryContext old_context;
			Datum  *hist_values;
			int		nvals;

			/* Sort the MCV items into position order to speed next loop */
			qsort((void *) track, num_mcv,
				  sizeof(ScalarMCVItem), compare_mcvs);

			/*
			 * Collapse out the MCV items from the values[] array.
			 *
			 * Note we destroy the values[] array here... but we don't need
			 * it for anything more.  We do, however, still need values_cnt.
			 */
			if (num_mcv > 0)
			{
				int		src,
						dest;
				int		j;

				src = dest = 0;
				j = 0;			/* index of next interesting MCV item */
				while (src < values_cnt)
				{
					int		ncopy;

					if (j < num_mcv)
					{
						int		first = track[j].first;

						if (src >= first)
						{
							/* advance past this MCV item */
							src = first + track[j].count;
							j++;
							continue;
						}
						ncopy = first - src;
					}
					else
					{
						ncopy = values_cnt - src;
					}
					memmove(&values[dest], &values[src],
							ncopy * sizeof(ScalarItem));
					src += ncopy;
					dest += ncopy;
				}
				nvals = dest;
			}
			else
				nvals = values_cnt;
			Assert(nvals >= num_hist);

			/* Must copy the target values into TransactionCommandContext */
			old_context = MemoryContextSwitchTo(TransactionCommandContext);
			hist_values = (Datum *) palloc(num_hist * sizeof(Datum));
			for (i = 0; i < num_hist; i++)
			{
				int		pos;

				pos = (i * (nvals - 1)) / (num_hist - 1);
				hist_values[i] = datumCopy(values[pos].value,
										   stats->attr->attbyval,
										   stats->attr->attlen);
			}
			MemoryContextSwitchTo(old_context);

			stats->stakind[slot_idx] = STATISTIC_KIND_HISTOGRAM;
			stats->staop[slot_idx] = stats->ltopr;
			stats->stavalues[slot_idx] = hist_values;
			stats->numvalues[slot_idx] = num_hist;
			slot_idx++;
		}

		/* Generate a correlation entry if there are multiple values */
		if (values_cnt > 1)
		{
			MemoryContext old_context;
			float4 *corrs;
			double	corr_xsum,
					corr_x2sum;

			/* Must copy the target values into TransactionCommandContext */
			old_context = MemoryContextSwitchTo(TransactionCommandContext);
			corrs = (float4 *) palloc(sizeof(float4));
			MemoryContextSwitchTo(old_context);

			/*----------
			 * Since we know the x and y value sets are both
			 *		0, 1, ..., values_cnt-1
			 * we have sum(x) = sum(y) =
			 *		(values_cnt-1)*values_cnt / 2
			 * and sum(x^2) = sum(y^2) =
			 *		(values_cnt-1)*values_cnt*(2*values_cnt-1) / 6.
			 *----------
			 */
			corr_xsum = (double) (values_cnt-1) * (double) values_cnt / 2.0;
			corr_x2sum = (double) (values_cnt-1) * (double) values_cnt *
				(double) (2*values_cnt-1) / 6.0;
			/* And the correlation coefficient reduces to */
			corrs[0] = (values_cnt * corr_xysum - corr_xsum * corr_xsum) /
				(values_cnt * corr_x2sum - corr_xsum * corr_xsum);

			stats->stakind[slot_idx] = STATISTIC_KIND_CORRELATION;
			stats->staop[slot_idx] = stats->ltopr;
			stats->stanumbers[slot_idx] = corrs;
			stats->numnumbers[slot_idx] = 1;
			slot_idx++;
		}
	}

	/* We don't need to bother cleaning up any of our temporary palloc's */
}

/*
 * qsort comparator for sorting ScalarItems
 *
 * Aside from sorting the items, we update the datumCmpTupnoLink[] array
 * whenever two ScalarItems are found to contain equal datums.  The array
 * is indexed by tupno; for each ScalarItem, it contains the highest
 * tupno that that item's datum has been found to be equal to.  This allows
 * us to avoid additional comparisons in compute_scalar_stats().
 */
static int
compare_scalars(const void *a, const void *b)
{
	Datum		da = ((ScalarItem *) a)->value;
	int			ta = ((ScalarItem *) a)->tupno;
	Datum		db = ((ScalarItem *) b)->value;
	int			tb = ((ScalarItem *) b)->tupno;

	if (datumCmpFnKind == SORTFUNC_LT)
	{
		if (DatumGetBool(FunctionCall2(datumCmpFn, da, db)))
			return -1;			/* a < b */
		if (DatumGetBool(FunctionCall2(datumCmpFn, db, da)))
			return 1;			/* a > b */
	}
	else
	{
		/* sort function is CMP or REVCMP */
		int32	compare;

		compare = DatumGetInt32(FunctionCall2(datumCmpFn, da, db));
		if (compare != 0)
		{
			if (datumCmpFnKind == SORTFUNC_REVCMP)
				compare = -compare;
			return compare;
		}
	}

	/*
	 * The two datums are equal, so update datumCmpTupnoLink[].
	 */
	if (datumCmpTupnoLink[ta] < tb)
		datumCmpTupnoLink[ta] = tb;
	if (datumCmpTupnoLink[tb] < ta)
		datumCmpTupnoLink[tb] = ta;

	/*
	 * For equal datums, sort by tupno
	 */
	return ta - tb;
}

/*
 * qsort comparator for sorting ScalarMCVItems by position
 */
static int
compare_mcvs(const void *a, const void *b)
{
	int			da = ((ScalarMCVItem *) a)->first;
	int			db = ((ScalarMCVItem *) b)->first;

	return da - db;
}


/*
 *	update_attstats() -- update attribute statistics for one relation
 *
 *		Statistics are stored in several places: the pg_class row for the
 *		relation has stats about the whole relation, and there is a
 *		pg_statistic row for each (non-system) attribute that has ever
 *		been analyzed.  The pg_class values are updated by VACUUM, not here.
 *
 *		pg_statistic rows are just added or updated normally.  This means
 *		that pg_statistic will probably contain some deleted rows at the
 *		completion of a vacuum cycle, unless it happens to get vacuumed last.
 *
 *		To keep things simple, we punt for pg_statistic, and don't try
 *		to compute or store rows for pg_statistic itself in pg_statistic.
 *		This could possibly be made to work, but it's not worth the trouble.
 *		Note analyze_rel() has seen to it that we won't come here when
 *		vacuuming pg_statistic itself.
 */
static void
update_attstats(Oid relid, int natts, VacAttrStats **vacattrstats)
{
	Relation	sd;
	int			attno;

	/*
	 * We use an ExclusiveLock on pg_statistic to ensure that only one
	 * backend is writing it at a time --- without that, we might have to
	 * deal with concurrent updates here, and it's not worth the trouble.
	 */
	sd = heap_openr(StatisticRelationName, ExclusiveLock);

	for (attno = 0; attno < natts; attno++)
	{
		VacAttrStats *stats = vacattrstats[attno];
		FmgrInfo	out_function;
		HeapTuple	stup,
					oldtup;
		int			i, k, n;
		Datum		values[Natts_pg_statistic];
		char		nulls[Natts_pg_statistic];
		char		replaces[Natts_pg_statistic];
		Relation	irelations[Num_pg_statistic_indices];

		/* Ignore attr if we weren't able to collect stats */
		if (!stats->stats_valid)
			continue;

		fmgr_info(stats->attrtype->typoutput, &out_function);

		/*
		 * Construct a new pg_statistic tuple
		 */
		for (i = 0; i < Natts_pg_statistic; ++i)
		{
			nulls[i] = ' ';
			replaces[i] = 'r';
		}

		i = 0;
		values[i++] = ObjectIdGetDatum(relid); /* starelid */
		values[i++] = Int16GetDatum(stats->attnum); /* staattnum */
		values[i++] = Float4GetDatum(stats->stanullfrac); /* stanullfrac */
		values[i++] = Int32GetDatum(stats->stawidth); /* stawidth */
		values[i++] = Float4GetDatum(stats->stadistinct); /* stadistinct */
		for (k = 0; k < STATISTIC_NUM_SLOTS; k++)
		{
			values[i++] = Int16GetDatum(stats->stakind[k]);	/* stakindN */
		}
		for (k = 0; k < STATISTIC_NUM_SLOTS; k++)
		{
			values[i++] = ObjectIdGetDatum(stats->staop[k]); /* staopN */
		}
		for (k = 0; k < STATISTIC_NUM_SLOTS; k++)
		{
			int		nnum = stats->numnumbers[k];

			if (nnum > 0)
			{
				Datum	   *numdatums = (Datum *) palloc(nnum * sizeof(Datum));
				ArrayType  *arry;

				for (n = 0; n < nnum; n++)
					numdatums[n] = Float4GetDatum(stats->stanumbers[k][n]);
				/* XXX knows more than it should about type float4: */
				arry = construct_array(numdatums, nnum,
									   false, sizeof(float4), 'i');
				values[i++] = PointerGetDatum(arry); /* stanumbersN */
			}
			else
			{
				nulls[i] = 'n';
				values[i++] = (Datum) 0;
			}
		}
		for (k = 0; k < STATISTIC_NUM_SLOTS; k++)
		{
			int		ntxt = stats->numvalues[k];

			if (ntxt > 0)
			{
				Datum	   *txtdatums = (Datum *) palloc(ntxt * sizeof(Datum));
				ArrayType  *arry;

				for (n = 0; n < ntxt; n++)
				{
					/*
					 * Convert data values to a text string to be inserted
					 * into the text array.
					 */
					Datum	stringdatum;

					stringdatum =
						FunctionCall3(&out_function,
									  stats->stavalues[k][n],
									  ObjectIdGetDatum(stats->attrtype->typelem),
									  Int32GetDatum(stats->attr->atttypmod));
					txtdatums[n] = DirectFunctionCall1(textin, stringdatum);
					pfree(DatumGetPointer(stringdatum));
				}
				/* XXX knows more than it should about type text: */
				arry = construct_array(txtdatums, ntxt,
									   false, -1, 'i');
				values[i++] = PointerGetDatum(arry); /* stavaluesN */
			}
			else
			{
				nulls[i] = 'n';
				values[i++] = (Datum) 0;
			}
		}

		/* Is there already a pg_statistic tuple for this attribute? */
		oldtup = SearchSysCache(STATRELATT,
								ObjectIdGetDatum(relid),
								Int16GetDatum(stats->attnum),
								0, 0);

		if (HeapTupleIsValid(oldtup))
		{
			/* Yes, replace it */
			stup = heap_modifytuple(oldtup,
									sd,
									values,
									nulls,
									replaces);
			ReleaseSysCache(oldtup);
			simple_heap_update(sd, &stup->t_self, stup);
		}
		else
		{
			/* No, insert new tuple */
			stup = heap_formtuple(sd->rd_att, values, nulls);
			heap_insert(sd, stup);
		}

		/* update indices too */
		CatalogOpenIndices(Num_pg_statistic_indices, Name_pg_statistic_indices,
						   irelations);
		CatalogIndexInsert(irelations, Num_pg_statistic_indices, sd, stup);
		CatalogCloseIndices(Num_pg_statistic_indices, irelations);

		heap_freetuple(stup);
	}

	/* close rel, but hold lock till upcoming commit */
	heap_close(sd, NoLock);
}
