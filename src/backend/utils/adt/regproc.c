 /*-------------------------------------------------------------------------
 *
 * regproc.c--
 *	  Functions for the built-in type "RegProcedure".
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/regproc.c,v 1.22 1998/08/19 02:03:04 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"
#include "miscadmin.h"
#include "access/heapam.h"
#include "access/relscan.h"
#include "fmgr.h"
#include "utils/palloc.h"
#include "utils/syscache.h"

#include "catalog/catname.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"		/* where function declarations go */

/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/
 
/*
 *		regprocin		- converts "proname" to proid
 *
 *		proid of NULL signifies unknown
 */
int32
regprocin(char *pro_oid_name)
{
	HeapTuple	proctup;
	RegProcedure result = (Oid) 0;

	if (pro_oid_name == NULL)
		return (0);


	if (!IsBootstrapProcessingMode())
	{
		/*
		 *  we need to use the oid because there can be multiple entries
		 *	with the same name, i.e.  1323(int4eq)
		 */
		proctup = SearchSysCacheTuple(PROOID,
										ObjectIdGetDatum(atoi(pro_oid_name)),
										0, 0, 0);
		if (HeapTupleIsValid(proctup))
				result = (RegProcedure) proctup->t_oid;
		else	result = (RegProcedure) 0;
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
			return (0);
		}
		ScanKeyEntryInitialize(&key,
							   (bits16) 0,
							   (AttrNumber) 1,
							   (RegProcedure) F_NAMEEQ,
							   (Datum) pro_oid_name);
	
		procscan = heap_beginscan(proc, 0, SnapshotNow, 1, &key);
		if (!HeapScanIsValid(procscan))
		{
			heap_close(proc);
			elog(ERROR, "regprocin: could not being scan of %s",
				 ProcedureRelationName);
			return (0);
		}
		proctup = heap_getnext(procscan, 0);
		if (HeapTupleIsValid(proctup))
		{
				result = (RegProcedure) heap_getattr(proctup,
													 ObjectIdAttributeNumber,
											RelationGetTupleDescriptor(proc),
													 &isnull);
				if (isnull)
					elog(FATAL, "regprocin: null procedure %s", pro_oid_name);
		}
		else
				result = (RegProcedure) 0;

		heap_endscan(procscan);
		heap_close(proc);
	}	

#ifdef	EBUG
	elog(DEBUG, "regprocin: no such procedure %s", pro_oid_name);
#endif							/* defined(EBUG) */
	return (int32) result;
}

/*
 *		regprocout		- converts proid to "pro_oid_name"
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
				snprintf(result, NAMEDATALEN, "%d(%s)", proid, s);
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
			elog(ERROR, "regprocout: could not open %s",
				 ProcedureRelationName);
			return (0);
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
			return (0);
		}
		proctup = heap_getnext(procscan, 0);
		if (HeapTupleIsValid(proctup))
		{
			char	   *s;
			bool		isnull;

			s = (char *) heap_getattr(proctup, 1,
							  RelationGetTupleDescriptor(proc), &isnull);
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
		return (result);
	}

#ifdef	EBUG
			elog(DEBUG, "regprocout: no such procedure %d", proid);
#endif							/* defined(EBUG) */
	return (result);
}

/*
 *		int8typeout			- converts int8 type oids to "typname" list
 */
text *
oid8types(Oid (*oidArray)[])
{
	HeapTuple	typetup;
	text	   *result;
	int			num;
	Oid		   *sp;

	if (oidArray == NULL)
	{
		result = (text *) palloc(VARHDRSZ);
		VARSIZE(result) = 0;
		return (result);
	}

	result = (text *) palloc(NAMEDATALEN * 8 + 8 + VARHDRSZ);
	*VARDATA(result) = '\0';

	sp = *oidArray;
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

				s = ((TypeTupleForm) GETSTRUCT(typetup))->typname.data;
				StrNCpy(VARDATA(result) + strlen(VARDATA(result)), s,
							NAMEDATALEN);
				strcat(VARDATA(result), " ");
			}
		}
	}
	VARSIZE(result) = strlen(VARDATA(result)) + VARHDRSZ;
	return (result);
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
