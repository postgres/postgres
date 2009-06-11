/*-------------------------------------------------------------------------
 *
 * ginutil.c
 *	  utilities routines for the postgres inverted index access method.
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			$PostgreSQL: pgsql/src/backend/access/gin/ginutil.c,v 1.22 2009/06/11 14:48:53 momjian Exp $
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/genam.h"
#include "access/gin.h"
#include "access/reloptions.h"
#include "catalog/pg_type.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"

void
initGinState(GinState *state, Relation index)
{
	int			i;

	state->origTupdesc = index->rd_att;

	state->oneCol = (index->rd_att->natts == 1) ? true : false;

	for (i = 0; i < index->rd_att->natts; i++)
	{
		state->tupdesc[i] = CreateTemplateTupleDesc(2, false);

		TupleDescInitEntry(state->tupdesc[i], (AttrNumber) 1, NULL,
						   INT2OID, -1, 0);
		TupleDescInitEntry(state->tupdesc[i], (AttrNumber) 2, NULL,
						   index->rd_att->attrs[i]->atttypid,
						   index->rd_att->attrs[i]->atttypmod,
						   index->rd_att->attrs[i]->attndims
			);

		fmgr_info_copy(&(state->compareFn[i]),
					   index_getprocinfo(index, i + 1, GIN_COMPARE_PROC),
					   CurrentMemoryContext);
		fmgr_info_copy(&(state->extractValueFn[i]),
					   index_getprocinfo(index, i + 1, GIN_EXTRACTVALUE_PROC),
					   CurrentMemoryContext);
		fmgr_info_copy(&(state->extractQueryFn[i]),
					   index_getprocinfo(index, i + 1, GIN_EXTRACTQUERY_PROC),
					   CurrentMemoryContext);
		fmgr_info_copy(&(state->consistentFn[i]),
					   index_getprocinfo(index, i + 1, GIN_CONSISTENT_PROC),
					   CurrentMemoryContext);

		/*
		 * Check opclass capability to do partial match.
		 */
		if (index_getprocid(index, i + 1, GIN_COMPARE_PARTIAL_PROC) != InvalidOid)
		{
			fmgr_info_copy(&(state->comparePartialFn[i]),
				   index_getprocinfo(index, i + 1, GIN_COMPARE_PARTIAL_PROC),
						   CurrentMemoryContext);

			state->canPartialMatch[i] = true;
		}
		else
		{
			state->canPartialMatch[i] = false;
		}
	}
}

/*
 * Extract attribute (column) number of stored entry from GIN tuple
 */
OffsetNumber
gintuple_get_attrnum(GinState *ginstate, IndexTuple tuple)
{
	OffsetNumber colN = FirstOffsetNumber;

	if (!ginstate->oneCol)
	{
		Datum		res;
		bool		isnull;

		/*
		 * First attribute is always int16, so we can safely use any tuple
		 * descriptor to obtain first attribute of tuple
		 */
		res = index_getattr(tuple, FirstOffsetNumber, ginstate->tupdesc[0],
							&isnull);
		Assert(!isnull);

		colN = DatumGetUInt16(res);
		Assert(colN >= FirstOffsetNumber && colN <= ginstate->origTupdesc->natts);
	}

	return colN;
}

/*
 * Extract stored datum from GIN tuple
 */
Datum
gin_index_getattr(GinState *ginstate, IndexTuple tuple)
{
	bool		isnull;
	Datum		res;

	if (ginstate->oneCol)
	{
		/*
		 * Single column index doesn't store attribute numbers in tuples
		 */
		res = index_getattr(tuple, FirstOffsetNumber, ginstate->origTupdesc,
							&isnull);
	}
	else
	{
		/*
		 * Since the datum type depends on which index column it's from, we
		 * must be careful to use the right tuple descriptor here.
		 */
		OffsetNumber colN = gintuple_get_attrnum(ginstate, tuple);

		res = index_getattr(tuple, OffsetNumberNext(FirstOffsetNumber),
							ginstate->tupdesc[colN - 1],
							&isnull);
	}

	Assert(!isnull);

	return res;
}

/*
 * Allocate a new page (either by recycling, or by extending the index file)
 * The returned buffer is already pinned and exclusive-locked
 * Caller is responsible for initializing the page by calling GinInitBuffer
 */

Buffer
GinNewBuffer(Relation index)
{
	Buffer		buffer;
	bool		needLock;

	/* First, try to get a page from FSM */
	for (;;)
	{
		BlockNumber blkno = GetFreeIndexPage(index);

		if (blkno == InvalidBlockNumber)
			break;

		buffer = ReadBuffer(index, blkno);

		/*
		 * We have to guard against the possibility that someone else already
		 * recycled this page; the buffer may be locked if so.
		 */
		if (ConditionalLockBuffer(buffer))
		{
			Page		page = BufferGetPage(buffer);

			if (PageIsNew(page))
				return buffer;	/* OK to use, if never initialized */

			if (GinPageIsDeleted(page))
				return buffer;	/* OK to use */

			LockBuffer(buffer, GIN_UNLOCK);
		}

		/* Can't use it, so release buffer and try again */
		ReleaseBuffer(buffer);
	}

	/* Must extend the file */
	needLock = !RELATION_IS_LOCAL(index);
	if (needLock)
		LockRelationForExtension(index, ExclusiveLock);

	buffer = ReadBuffer(index, P_NEW);
	LockBuffer(buffer, GIN_EXCLUSIVE);

	if (needLock)
		UnlockRelationForExtension(index, ExclusiveLock);

	return buffer;
}

void
GinInitPage(Page page, uint32 f, Size pageSize)
{
	GinPageOpaque opaque;

	PageInit(page, pageSize, sizeof(GinPageOpaqueData));

	opaque = GinPageGetOpaque(page);
	memset(opaque, 0, sizeof(GinPageOpaqueData));
	opaque->flags = f;
	opaque->rightlink = InvalidBlockNumber;
}

void
GinInitBuffer(Buffer b, uint32 f)
{
	GinInitPage(BufferGetPage(b), f, BufferGetPageSize(b));
}

void
GinInitMetabuffer(Buffer b)
{
	GinMetaPageData *metadata;
	Page		page = BufferGetPage(b);

	GinInitPage(page, GIN_META, BufferGetPageSize(b));

	metadata = GinPageGetMeta(page);

	metadata->head = metadata->tail = InvalidBlockNumber;
	metadata->tailFreeSize = 0;
	metadata->nPendingPages = 0;
	metadata->nPendingHeapTuples = 0;
}

int
compareEntries(GinState *ginstate, OffsetNumber attnum, Datum a, Datum b)
{
	return DatumGetInt32(
						 FunctionCall2(
									   &ginstate->compareFn[attnum - 1],
									   a, b
									   )
		);
}

int
compareAttEntries(GinState *ginstate, OffsetNumber attnum_a, Datum a,
				  OffsetNumber attnum_b, Datum b)
{
	if (attnum_a == attnum_b)
		return compareEntries(ginstate, attnum_a, a, b);

	return (attnum_a < attnum_b) ? -1 : 1;
}

typedef struct
{
	FmgrInfo   *cmpDatumFunc;
	bool	   *needUnique;
} cmpEntriesData;

static int
cmpEntries(const Datum *a, const Datum *b, cmpEntriesData *arg)
{
	int			res = DatumGetInt32(FunctionCall2(arg->cmpDatumFunc,
												  *a, *b));

	if (res == 0)
		*(arg->needUnique) = TRUE;

	return res;
}

Datum *
extractEntriesS(GinState *ginstate, OffsetNumber attnum, Datum value, int32 *nentries,
				bool *needUnique)
{
	Datum	   *entries;

	entries = (Datum *) DatumGetPointer(FunctionCall2(
									   &ginstate->extractValueFn[attnum - 1],
													  value,
													PointerGetDatum(nentries)
													  ));

	if (entries == NULL)
		*nentries = 0;

	*needUnique = FALSE;
	if (*nentries > 1)
	{
		cmpEntriesData arg;

		arg.cmpDatumFunc = &ginstate->compareFn[attnum - 1];
		arg.needUnique = needUnique;
		qsort_arg(entries, *nentries, sizeof(Datum),
				  (qsort_arg_comparator) cmpEntries, (void *) &arg);
	}

	return entries;
}


Datum *
extractEntriesSU(GinState *ginstate, OffsetNumber attnum, Datum value, int32 *nentries)
{
	bool		needUnique;
	Datum	   *entries = extractEntriesS(ginstate, attnum, value, nentries,
										  &needUnique);

	if (needUnique)
	{
		Datum	   *ptr,
				   *res;

		ptr = res = entries;

		while (ptr - entries < *nentries)
		{
			if (compareEntries(ginstate, attnum, *ptr, *res) != 0)
				*(++res) = *ptr++;
			else
				ptr++;
		}

		*nentries = res + 1 - entries;
	}

	return entries;
}

Datum
ginoptions(PG_FUNCTION_ARGS)
{
	Datum		reloptions = PG_GETARG_DATUM(0);
	bool		validate = PG_GETARG_BOOL(1);
	relopt_value *options;
	GinOptions *rdopts;
	int			numoptions;
	static const relopt_parse_elt tab[] = {
		{"fastupdate", RELOPT_TYPE_BOOL, offsetof(GinOptions, useFastUpdate)}
	};

	options = parseRelOptions(reloptions, validate, RELOPT_KIND_GIN,
							  &numoptions);

	/* if none set, we're done */
	if (numoptions == 0)
		PG_RETURN_NULL();

	rdopts = allocateReloptStruct(sizeof(GinOptions), options, numoptions);

	fillRelOptions((void *) rdopts, sizeof(GinOptions), options, numoptions,
				   validate, tab, lengthof(tab));

	pfree(options);

	PG_RETURN_BYTEA_P(rdopts);
}
