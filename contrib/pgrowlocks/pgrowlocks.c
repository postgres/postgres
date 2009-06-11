/*
 * $PostgreSQL: pgsql/contrib/pgrowlocks/pgrowlocks.c,v 1.12 2009/06/11 14:48:52 momjian Exp $
 *
 * Copyright (c) 2005-2006	Tatsuo Ishii
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose, without fee, and without a
 * written agreement is hereby granted, provided that the above
 * copyright notice and this paragraph and the following two
 * paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE TO ANY PARTY FOR DIRECT,
 * INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA HAS BEEN ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHOR SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS
 * IS" BASIS, AND THE AUTHOR HAS NO OBLIGATIONS TO PROVIDE MAINTENANCE,
 * SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/multixact.h"
#include "access/relscan.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/tqual.h"


PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pgrowlocks);

extern Datum pgrowlocks(PG_FUNCTION_ARGS);

/* ----------
 * pgrowlocks:
 * returns tids of rows being locked
 * ----------
 */

#define NCHARS 32

typedef struct
{
	Relation	rel;
	HeapScanDesc scan;
	int			ncolumns;
} MyData;

Datum
pgrowlocks(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	HeapScanDesc scan;
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	AttInMetadata *attinmeta;
	Datum		result;
	MyData	   *mydata;
	Relation	rel;

	if (SRF_IS_FIRSTCALL())
	{
		text	   *relname;
		RangeVar   *relrv;
		MemoryContext oldcontext;
		AclResult	aclresult;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");

		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;

		relname = PG_GETARG_TEXT_P(0);
		relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
		rel = heap_openrv(relrv, AccessShareLock);

		/* check permissions: must have SELECT on table */
		aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
									  ACL_SELECT);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_CLASS,
						   RelationGetRelationName(rel));

		scan = heap_beginscan(rel, SnapshotNow, 0, NULL);
		mydata = palloc(sizeof(*mydata));
		mydata->rel = rel;
		mydata->scan = scan;
		mydata->ncolumns = tupdesc->natts;
		funcctx->user_fctx = mydata;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	attinmeta = funcctx->attinmeta;
	mydata = (MyData *) funcctx->user_fctx;
	scan = mydata->scan;

	/* scan the relation */
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		/* must hold a buffer lock to call HeapTupleSatisfiesUpdate */
		LockBuffer(scan->rs_cbuf, BUFFER_LOCK_SHARE);

		if (HeapTupleSatisfiesUpdate(tuple->t_data,
									 GetCurrentCommandId(false),
									 scan->rs_cbuf) == HeapTupleBeingUpdated)
		{

			char	  **values;
			int			i;

			values = (char **) palloc(mydata->ncolumns * sizeof(char *));

			i = 0;
			values[i++] = (char *) DirectFunctionCall1(tidout, PointerGetDatum(&tuple->t_self));

			if (tuple->t_data->t_infomask & HEAP_XMAX_SHARED_LOCK)
				values[i++] = pstrdup("Shared");
			else
				values[i++] = pstrdup("Exclusive");
			values[i] = palloc(NCHARS * sizeof(char));
			snprintf(values[i++], NCHARS, "%d", HeapTupleHeaderGetXmax(tuple->t_data));
			if (tuple->t_data->t_infomask & HEAP_XMAX_IS_MULTI)
			{
				TransactionId *xids;
				int			nxids;
				int			j;
				int			isValidXid = 0;		/* any valid xid ever exists? */

				values[i++] = pstrdup("true");
				nxids = GetMultiXactIdMembers(HeapTupleHeaderGetXmax(tuple->t_data), &xids);
				if (nxids == -1)
				{
					elog(ERROR, "GetMultiXactIdMembers returns error");
				}

				values[i] = palloc(NCHARS * nxids);
				values[i + 1] = palloc(NCHARS * nxids);
				strcpy(values[i], "{");
				strcpy(values[i + 1], "{");

				for (j = 0; j < nxids; j++)
				{
					char		buf[NCHARS];

					if (TransactionIdIsInProgress(xids[j]))
					{
						if (isValidXid)
						{
							strcat(values[i], ",");
							strcat(values[i + 1], ",");
						}
						snprintf(buf, NCHARS, "%d", xids[j]);
						strcat(values[i], buf);
						snprintf(buf, NCHARS, "%d", BackendXidGetPid(xids[j]));
						strcat(values[i + 1], buf);

						isValidXid = 1;
					}
				}

				strcat(values[i], "}");
				strcat(values[i + 1], "}");
				i++;
			}
			else
			{
				values[i++] = pstrdup("false");
				values[i] = palloc(NCHARS * sizeof(char));
				snprintf(values[i++], NCHARS, "{%d}", HeapTupleHeaderGetXmax(tuple->t_data));

				values[i] = palloc(NCHARS * sizeof(char));
				snprintf(values[i++], NCHARS, "{%d}", BackendXidGetPid(HeapTupleHeaderGetXmax(tuple->t_data)));
			}

			LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			/* build a tuple */
			tuple = BuildTupleFromCStrings(attinmeta, values);

			/* make the tuple into a datum */
			result = HeapTupleGetDatum(tuple);

			/* Clean up */
			for (i = 0; i < mydata->ncolumns; i++)
				pfree(values[i]);
			pfree(values);

			SRF_RETURN_NEXT(funcctx, result);
		}
		else
		{
			LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);
		}
	}

	heap_endscan(scan);
	heap_close(mydata->rel, AccessShareLock);

	SRF_RETURN_DONE(funcctx);
}
