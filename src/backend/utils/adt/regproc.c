 /*-------------------------------------------------------------------------
 *
 * regproc.c
 *	  Functions for the built-in type "RegProcedure".
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/regproc.c,v 1.52 2000/02/18 09:28:48 inoue Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

/*
 *		regprocin		- converts "proname" or "proid" to proid
 *
 *		proid of '-' signifies unknown, for consistency with regprocout
 */
int32
regprocin(char *pro_name_or_oid)
{
	HeapTuple	proctup = NULL;
	HeapTupleData tuple;
	RegProcedure result = InvalidOid;

	if (pro_name_or_oid == NULL)
		return InvalidOid;
	if (pro_name_or_oid[0] == '-' && pro_name_or_oid[1] == '\0')
		return InvalidOid;

	if (!IsIgnoringSystemIndexes())
	{

		/*
		 * we need to use the oid because there can be multiple entries
		 * with the same name.	We accept int4eq_1323 and 1323.
		 */
		if (pro_name_or_oid[0] >= '0' &&
			pro_name_or_oid[0] <= '9')
		{
			proctup = SearchSysCacheTuple(PROCOID,
								ObjectIdGetDatum(oidin(pro_name_or_oid)),
										  0, 0, 0);
			if (HeapTupleIsValid(proctup))
				result = (RegProcedure) proctup->t_data->t_oid;
			else
				elog(ERROR, "No procedure with oid %s", pro_name_or_oid);
		}
		else
		{
			Relation	hdesc;
			Relation	idesc;
			IndexScanDesc sd;
			ScanKeyData skey[1];
			RetrieveIndexResult indexRes;
			Buffer		buffer;
			int			matches = 0;

			ScanKeyEntryInitialize(&skey[0],
								   (bits16) 0x0,
								   (AttrNumber) 1,
								   (RegProcedure) F_NAMEEQ,
								   PointerGetDatum(pro_name_or_oid));

			hdesc = heap_openr(ProcedureRelationName, AccessShareLock);
			idesc = index_openr(ProcedureNameIndex);

			sd = index_beginscan(idesc, false, 1, skey);
			while ((indexRes = index_getnext(sd, ForwardScanDirection)))
			{
				tuple.t_datamcxt = NULL;
				tuple.t_data = NULL;
				tuple.t_self = indexRes->heap_iptr;
				heap_fetch(hdesc, SnapshotNow,
						   &tuple,
						   &buffer);
				pfree(indexRes);
				if (tuple.t_data != NULL)
				{
					result = (RegProcedure) tuple.t_data->t_oid;
					ReleaseBuffer(buffer);

					if (++matches > 1)
						break;
				}
			}

			index_endscan(sd);
			index_close(idesc);
			heap_close(hdesc, AccessShareLock);

			if (matches > 1)
				elog(ERROR, "There is more than one procedure named %s.\n\tSupply the pg_proc oid inside single quotes.", pro_name_or_oid);
			else if (matches == 0)
				elog(ERROR, "No procedure with name %s", pro_name_or_oid);
		}
	}
	else
	{
		Relation	proc;
		HeapScanDesc procscan;
		ScanKeyData key;
		bool		isnull;

		proc = heap_openr(ProcedureRelationName, AccessShareLock);
		ScanKeyEntryInitialize(&key,
							   (bits16) 0,
							   (AttrNumber) 1,
							   (RegProcedure) F_NAMEEQ,
							   (Datum) pro_name_or_oid);

		procscan = heap_beginscan(proc, 0, SnapshotNow, 1, &key);
		if (!HeapScanIsValid(procscan))
		{
			heap_close(proc, AccessShareLock);
			elog(ERROR, "regprocin: could not begin scan of %s",
				 ProcedureRelationName);
			return 0;
		}
		proctup = heap_getnext(procscan, 0);
		if (HeapTupleIsValid(proctup))
		{
			result = (RegProcedure) heap_getattr(proctup,
												 ObjectIdAttributeNumber,
												 RelationGetDescr(proc),
												 &isnull);
			if (isnull)
				elog(FATAL, "regprocin: null procedure %s", pro_name_or_oid);
		}
		else
			result = (RegProcedure) 0;

		heap_endscan(procscan);
		heap_close(proc, AccessShareLock);
	}

	return (int32) result;
}

/*
 *		regprocout		- converts proid to "pro_name"
 */
char *
regprocout(RegProcedure proid)
{
	HeapTuple	proctup;
	char	   *result;

	result = (char *) palloc(NAMEDATALEN);

	if (!IsBootstrapProcessingMode())
	{
		proctup = SearchSysCacheTuple(PROCOID,
									  ObjectIdGetDatum(proid),
									  0, 0, 0);

		if (HeapTupleIsValid(proctup))
		{
			char	   *s;

			s = NameStr(((Form_pg_proc) GETSTRUCT(proctup))->proname);
			StrNCpy(result, s, NAMEDATALEN);
		}
		else
		{
			result[0] = '-';
			result[1] = '\0';
		}
	}
	else
	{
		Relation	proc;
		HeapScanDesc procscan;
		ScanKeyData key;

		proc = heap_openr(ProcedureRelationName, AccessShareLock);
		ScanKeyEntryInitialize(&key,
							   (bits16) 0,
							   (AttrNumber) ObjectIdAttributeNumber,
							   (RegProcedure) F_INT4EQ,
							   (Datum) proid);

		procscan = heap_beginscan(proc, 0, SnapshotNow, 1, &key);
		if (!HeapScanIsValid(procscan))
		{
			heap_close(proc, AccessShareLock);
			elog(ERROR, "regprocout: could not begin scan of %s",
				 ProcedureRelationName);
			return 0;
		}
		proctup = heap_getnext(procscan, 0);
		if (HeapTupleIsValid(proctup))
		{
			char	   *s;
			bool		isnull;

			s = (char *) heap_getattr(proctup, 1,
									  RelationGetDescr(proc), &isnull);
			if (!isnull)
				StrNCpy(result, s, NAMEDATALEN);
			else
				elog(FATAL, "regprocout: null procedure %u", proid);
		}
		else
		{
			result[0] = '-';
			result[1] = '\0';
		}
		heap_endscan(procscan);
		heap_close(proc, AccessShareLock);
	}

	return result;
}

/*
 *		int8typeout			- converts int8 type oids to "typname" list
 */
text *
oidvectortypes(Oid *oidArray)
{
	HeapTuple	typetup;
	text	   *result;
	int			num;

	if (oidArray == NULL)
	{
		result = (text *) palloc(VARHDRSZ);
		VARSIZE(result) = 0;
		return result;
	}

	result = (text *) palloc(NAMEDATALEN * FUNC_MAX_ARGS +
							 FUNC_MAX_ARGS + VARHDRSZ + 1);
	*VARDATA(result) = '\0';

	for (num = 0; num < FUNC_MAX_ARGS; num++)
	{
		if (oidArray[num] != InvalidOid)
		{
			typetup = SearchSysCacheTuple(TYPEOID,
										  ObjectIdGetDatum(oidArray[num]),
										  0, 0, 0);
			if (HeapTupleIsValid(typetup))
			{
				char	   *s;

				s = NameStr(((Form_pg_type) GETSTRUCT(typetup))->typname);
				StrNCpy(VARDATA(result) + strlen(VARDATA(result)), s,
						NAMEDATALEN);
				strcat(VARDATA(result), " ");
			}
		}
	}
	VARSIZE(result) = strlen(VARDATA(result)) + VARHDRSZ;
	return result;
}


/*****************************************************************************
 *	 PUBLIC ROUTINES														 *
 *****************************************************************************/

/* regproctooid()
 * Lowercase version of RegprocToOid() to allow case-insensitive SQL.
 * Define RegprocToOid() as a macro in builtins.h.
 * Referenced in pg_proc.h. - tgl 97/04/26
 */
Oid
regproctooid(RegProcedure rp)
{
	return (Oid) rp;
}

/* (see int.c for comparison/operation routines) */


/* ========== PRIVATE ROUTINES ========== */
