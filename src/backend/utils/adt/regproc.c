 /*-------------------------------------------------------------------------
 *
 * regproc.c--
 *	  Functions for the built-in type "RegProcedure".
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/regproc.c,v 1.31 1998/10/02 05:10:11 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"
#include "miscadmin.h"
#include "access/heapam.h"
#include "access/genam.h"
#include "access/itup.h"
#include "access/relscan.h"
#include "storage/bufmgr.h"
#include "fmgr.h"
#include "utils/palloc.h"
#include "utils/syscache.h"

#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"		/* where function declarations go */

/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

/*
 *		regprocin		- converts "proname" or "proid" to proid
 *
 *		proid of NULL signifies unknown
 */
int32
regprocin(char *pro_name_or_oid)
{
	HeapTuple	 proctup = NULL;
	RegProcedure result = InvalidOid;

	if (pro_name_or_oid == NULL)
		return InvalidOid;

	if (!IsBootstrapProcessingMode())
	{
		/*
		 * we need to use the oid because there can be multiple entries
		 * with the same name.	We accept int4eq_1323 and 1323.
		 */
		if (pro_name_or_oid[0] >= '0' &&
			pro_name_or_oid[0] <= '9')
		{
			proctup = SearchSysCacheTuple(PROOID,
								ObjectIdGetDatum(oidin(pro_name_or_oid)),
										  0, 0, 0);
			if (HeapTupleIsValid(proctup))
				result = (RegProcedure) proctup->t_oid;
			else
				elog(ERROR, "No such procedure with oid %s", pro_name_or_oid);
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
		
			hdesc = heap_openr(ProcedureRelationName);
			idesc = index_openr(ProcedureNameIndex);
		
			sd = index_beginscan(idesc, false, 1, skey);
			while ((indexRes = index_getnext(sd, ForwardScanDirection)))
			{
				proctup = heap_fetch(hdesc, SnapshotNow,
									&indexRes->heap_iptr,
									&buffer);
				pfree(indexRes);
				if (HeapTupleIsValid(proctup))
				{
					result = (RegProcedure) proctup->t_oid;
					ReleaseBuffer(buffer);

					if (++matches > 1)
						break;
				}
			}

			index_endscan(sd);
			pfree(sd);
			index_close(idesc);

			if (matches > 1)
				elog(ERROR, "There is more than one %s procedure, supply oid in quotes.", pro_name_or_oid);
			else if (matches == 0)
				elog(ERROR, "No such procedure %s", pro_name_or_oid);
		}
	}
	else
	{
		Relation	proc;
		HeapScanDesc procscan;
		ScanKeyData key;
		bool		isnull;

		proc = heap_openr(ProcedureRelationName);
		if (!RelationIsValid(proc))
		{
			elog(ERROR, "regprocin: could not open %s",
				 ProcedureRelationName);
			return 0;
		}
		ScanKeyEntryInitialize(&key,
							   (bits16) 0,
							   (AttrNumber) 1,
							   (RegProcedure) F_NAMEEQ,
							   (Datum) pro_name_or_oid);

		procscan = heap_beginscan(proc, 0, SnapshotNow, 1, &key);
		if (!HeapScanIsValid(procscan))
		{
			heap_close(proc);
			elog(ERROR, "regprocin: could not being scan of %s",
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
		heap_close(proc);
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
		proctup = SearchSysCacheTuple(PROOID,
									  ObjectIdGetDatum(proid),
									  0, 0, 0);

		if (HeapTupleIsValid(proctup))
		{
			char	   *s;

			s = ((Form_pg_proc) GETSTRUCT(proctup))->proname.data;
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

		proc = heap_openr(ProcedureRelationName);
		if (!RelationIsValid(proc))
		{
			elog(ERROR, "regprocout: could not open %s", ProcedureRelationName);
			return 0;
		}
		ScanKeyEntryInitialize(&key,
							   (bits16) 0,
							   (AttrNumber) ObjectIdAttributeNumber,
							   (RegProcedure) F_INT4EQ,
							   (Datum) proid);

		procscan = heap_beginscan(proc, 0, SnapshotNow, 1, &key);
		if (!HeapScanIsValid(procscan))
		{
			heap_close(proc);
			elog(ERROR, "regprocout: could not being scan of %s",
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
				elog(FATAL, "regprocout: null procedure %d", proid);
		}
		else
		{
			result[0] = '-';
			result[1] = '\0';
		}
		heap_endscan(procscan);
		heap_close(proc);
		return result;
	}

	return result;
}

/*
 *		int8typeout			- converts int8 type oids to "typname" list
 */
text *
oid8types(Oid *oidArray)
{
	HeapTuple	typetup;
	text	   *result;
	int			num;
	Oid		   *sp;

	if (oidArray == NULL)
	{
		result = (text *) palloc(VARHDRSZ);
		VARSIZE(result) = 0;
		return result;
	}

	result = (text *) palloc(NAMEDATALEN * 8 + 8 + VARHDRSZ);
	*VARDATA(result) = '\0';

	sp = oidArray;
	for (num = 8; num != 0; num--, sp++)
	{
		if (*sp != InvalidOid)
		{
			typetup = SearchSysCacheTuple(TYPOID,
										  ObjectIdGetDatum(*sp),
										  0, 0, 0);
			if (HeapTupleIsValid(typetup))
			{
				char	   *s;

				s = ((Form_pg_type) GETSTRUCT(typetup))->typname.data;
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
