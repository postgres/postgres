/*
 * contrib/pgrowlocks/pgrowlocks.c
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
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_authid.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/fmgrprotos.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/varlena.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pgrowlocks);

/* ----------
 * pgrowlocks:
 * returns tids of rows being locked
 * ----------
 */

#define NCHARS 32

#define		Atnum_tid		0
#define		Atnum_xmax		1
#define		Atnum_ismulti	2
#define		Atnum_xids		3
#define		Atnum_modes		4
#define		Atnum_pids		5

Datum
pgrowlocks(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	AttInMetadata *attinmeta;
	Relation	rel;
	RangeVar   *relrv;
	TableScanDesc scan;
	HeapScanDesc hscan;
	HeapTuple	tuple;
	AclResult	aclresult;
	char	  **values;

	InitMaterializedSRF(fcinfo, 0);

	/* Access the table */
	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = relation_openrv(relrv, AccessShareLock);

	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is a partitioned table",
						RelationGetRelationName(rel)),
				 errdetail("Partitioned tables do not contain rows.")));
	else if (rel->rd_rel->relkind != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table",
						RelationGetRelationName(rel))));
	else if (rel->rd_rel->relam != HEAP_TABLE_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("only heap AM is supported")));

	/*
	 * check permissions: must have SELECT on table or be in
	 * pg_stat_scan_tables
	 */
	aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
								  ACL_SELECT);
	if (aclresult != ACLCHECK_OK)
		aclresult = has_privs_of_role(GetUserId(), ROLE_PG_STAT_SCAN_TABLES) ? ACLCHECK_OK : ACLCHECK_NO_PRIV;

	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_relkind_objtype(rel->rd_rel->relkind),
					   RelationGetRelationName(rel));

	/* Scan the relation */
	scan = table_beginscan(rel, GetActiveSnapshot(), 0, NULL);
	hscan = (HeapScanDesc) scan;

	attinmeta = TupleDescGetAttInMetadata(rsinfo->setDesc);

	values = (char **) palloc(rsinfo->setDesc->natts * sizeof(char *));

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		TM_Result	htsu;
		TransactionId xmax;
		uint16		infomask;

		/* must hold a buffer lock to call HeapTupleSatisfiesUpdate */
		LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_SHARE);

		htsu = HeapTupleSatisfiesUpdate(tuple,
										GetCurrentCommandId(false),
										hscan->rs_cbuf);
		xmax = HeapTupleHeaderGetRawXmax(tuple->t_data);
		infomask = tuple->t_data->t_infomask;

		/*
		 * A tuple is locked if HTSU returns BeingModified.
		 */
		if (htsu == TM_BeingModified)
		{
			values[Atnum_tid] = (char *) DirectFunctionCall1(tidout,
															 PointerGetDatum(&tuple->t_self));

			values[Atnum_xmax] = palloc(NCHARS * sizeof(char));
			snprintf(values[Atnum_xmax], NCHARS, "%u", xmax);
			if (infomask & HEAP_XMAX_IS_MULTI)
			{
				MultiXactMember *members;
				int			nmembers;
				bool		first = true;
				bool		allow_old;

				values[Atnum_ismulti] = pstrdup("true");

				allow_old = HEAP_LOCKED_UPGRADED(infomask);
				nmembers = GetMultiXactIdMembers(xmax, &members, allow_old,
												 false);
				if (nmembers == -1)
				{
					values[Atnum_xids] = "{0}";
					values[Atnum_modes] = "{transient upgrade status}";
					values[Atnum_pids] = "{0}";
				}
				else
				{
					int			j;

					values[Atnum_xids] = palloc(NCHARS * nmembers);
					values[Atnum_modes] = palloc(NCHARS * nmembers);
					values[Atnum_pids] = palloc(NCHARS * nmembers);

					strcpy(values[Atnum_xids], "{");
					strcpy(values[Atnum_modes], "{");
					strcpy(values[Atnum_pids], "{");

					for (j = 0; j < nmembers; j++)
					{
						char		buf[NCHARS];

						if (!first)
						{
							strcat(values[Atnum_xids], ",");
							strcat(values[Atnum_modes], ",");
							strcat(values[Atnum_pids], ",");
						}
						snprintf(buf, NCHARS, "%u", members[j].xid);
						strcat(values[Atnum_xids], buf);
						switch (members[j].status)
						{
							case MultiXactStatusUpdate:
								snprintf(buf, NCHARS, "Update");
								break;
							case MultiXactStatusNoKeyUpdate:
								snprintf(buf, NCHARS, "No Key Update");
								break;
							case MultiXactStatusForUpdate:
								snprintf(buf, NCHARS, "For Update");
								break;
							case MultiXactStatusForNoKeyUpdate:
								snprintf(buf, NCHARS, "For No Key Update");
								break;
							case MultiXactStatusForShare:
								snprintf(buf, NCHARS, "For Share");
								break;
							case MultiXactStatusForKeyShare:
								snprintf(buf, NCHARS, "For Key Share");
								break;
						}
						strcat(values[Atnum_modes], buf);
						snprintf(buf, NCHARS, "%d",
								 BackendXidGetPid(members[j].xid));
						strcat(values[Atnum_pids], buf);

						first = false;
					}

					strcat(values[Atnum_xids], "}");
					strcat(values[Atnum_modes], "}");
					strcat(values[Atnum_pids], "}");
				}
			}
			else
			{
				values[Atnum_ismulti] = pstrdup("false");

				values[Atnum_xids] = palloc(NCHARS * sizeof(char));
				snprintf(values[Atnum_xids], NCHARS, "{%u}", xmax);

				values[Atnum_modes] = palloc(NCHARS);
				if (infomask & HEAP_XMAX_LOCK_ONLY)
				{
					if (HEAP_XMAX_IS_SHR_LOCKED(infomask))
						snprintf(values[Atnum_modes], NCHARS, "{For Share}");
					else if (HEAP_XMAX_IS_KEYSHR_LOCKED(infomask))
						snprintf(values[Atnum_modes], NCHARS, "{For Key Share}");
					else if (HEAP_XMAX_IS_EXCL_LOCKED(infomask))
					{
						if (tuple->t_data->t_infomask2 & HEAP_KEYS_UPDATED)
							snprintf(values[Atnum_modes], NCHARS, "{For Update}");
						else
							snprintf(values[Atnum_modes], NCHARS, "{For No Key Update}");
					}
					else
						/* neither keyshare nor exclusive bit it set */
						snprintf(values[Atnum_modes], NCHARS,
								 "{transient upgrade status}");
				}
				else
				{
					if (tuple->t_data->t_infomask2 & HEAP_KEYS_UPDATED)
						snprintf(values[Atnum_modes], NCHARS, "{Update}");
					else
						snprintf(values[Atnum_modes], NCHARS, "{No Key Update}");
				}

				values[Atnum_pids] = palloc(NCHARS * sizeof(char));
				snprintf(values[Atnum_pids], NCHARS, "{%d}",
						 BackendXidGetPid(xmax));
			}

			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);

			/* build a tuple */
			tuple = BuildTupleFromCStrings(attinmeta, values);
			tuplestore_puttuple(rsinfo->setResult, tuple);
		}
		else
		{
			LockBuffer(hscan->rs_cbuf, BUFFER_LOCK_UNLOCK);
		}
	}

	table_endscan(scan);
	table_close(rel, AccessShareLock);
	return (Datum) 0;
}
