/*-------------------------------------------------------------------------
 *
 * regproc.c--
 *	  Functions for the built-in type "RegProcedure".
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/regproc.c,v 1.20 1998/07/20 16:57:01 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"
#include "access/heapam.h"
#include "access/relscan.h"
#include "fmgr.h"
#include "utils/palloc.h"

#include "catalog/catname.h"
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
regprocin(char *proname)
{
	Relation	proc;
	HeapScanDesc procscan;
	HeapTuple	proctup;
	ScanKeyData key;
	RegProcedure result = (Oid) 0;
	bool		isnull;

	if (proname == NULL)
		return (0);
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
						   (Datum) proname);

	procscan = heap_beginscan(proc, 0, false, 1, &key);
	if (!HeapScanIsValid(procscan))
	{
		heap_close(proc);
		elog(ERROR, "regprocin: could not being scan of %s",
			 ProcedureRelationName);
		return (0);
	}
	proctup = heap_getnext(procscan, 0, (Buffer *) NULL);
	switch (HeapTupleIsValid(proctup))
	{
		case 1:
			result = (RegProcedure) heap_getattr(proctup,
												 ObjectIdAttributeNumber,
										RelationGetTupleDescriptor(proc),
												 &isnull);
			if (isnull)
				elog(FATAL, "regprocin: null procedure %s", proname);
			break;
		case 0:
			result = (RegProcedure) 0;
#ifdef	EBUG
			elog(DEBUG, "regprocin: no such procedure %s", proname);
#endif							/* defined(EBUG) */
	}
	heap_endscan(procscan);
	heap_close(proc);
	return ((int32) result);
}

/*
 *		regprocout		- converts proid to "proname"
 */
char *
regprocout(RegProcedure proid)
{
	Relation	proc;
	HeapScanDesc procscan;
	HeapTuple	proctup;
	char	   *result;
	ScanKeyData key;

	result = (char *) palloc(NAMEDATALEN);
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

	procscan = heap_beginscan(proc, 0, false, 1, &key);
	if (!HeapScanIsValid(procscan))
	{
		heap_close(proc);
		elog(ERROR, "regprocout: could not being scan of %s",
			 ProcedureRelationName);
		return (0);
	}
	proctup = heap_getnext(procscan, 0, (Buffer *) NULL);
	switch (HeapTupleIsValid(proctup))
	{
			char	   *s;
			bool		isnull;

		case 1:
			s = (char *) heap_getattr(proctup, 1,
							  RelationGetTupleDescriptor(proc), &isnull);
			if (!isnull)
			{
				StrNCpy(result, s, NAMEDATALEN);
				break;
			}
			elog(FATAL, "regprocout: null procedure %d", proid);
			/* FALLTHROUGH */
		case 0:
			result[0] = '-';
			result[1] = '\0';
#ifdef	EBUG
			elog(DEBUG, "regprocout: no such procedure %d", proid);
#endif							/* defined(EBUG) */
	}
	heap_endscan(procscan);
	heap_close(proc);
	return (result);
}

/*
 *		int8typeout			- converts int8 type oids to "typname" list
 */
text *
oid8types(Oid (*oidArray)[])
{
	Relation	type;
	HeapScanDesc typescan;
	HeapTuple	typetup;
	text	   *result;
	ScanKeyData key;
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
	type = heap_openr(TypeRelationName);
	if (!RelationIsValid(type))
	{
		elog(ERROR, "int8typeout: could not open %s",
			 TypeRelationName);
		return (0);
	}

	sp = *oidArray;
	for (num = 8; num != 0; num--, sp++)
	{
		if (*sp != InvalidOid)
		{
			ScanKeyEntryInitialize(&key,
								   (bits16) 0,
								   (AttrNumber) ObjectIdAttributeNumber,
								   (RegProcedure) F_INT4EQ,
								   (Datum) *sp);

			typescan = heap_beginscan(type, 0, false, 1, &key);
			if (!HeapScanIsValid(typescan))
			{
				heap_close(type);
				elog(ERROR, "int8typeout: could not being scan of %s",
					 TypeRelationName);
				return (0);
			}
			typetup = heap_getnext(typescan, 0, (Buffer *) NULL);
			if (HeapTupleIsValid(typetup))
			{
				char	   *s;
				bool		isnull;

				s = (char *) heap_getattr(typetup, 1,
							  RelationGetTupleDescriptor(type), &isnull);
				if (!isnull)
				{
					StrNCpy(VARDATA(result) + strlen(VARDATA(result)), s,
							NAMEDATALEN);
					strcat(VARDATA(result), " ");
				}
				else
					elog(FATAL, "int8typeout: null procedure %d", *sp);
				/* FALLTHROUGH */
			}
			heap_endscan(typescan);
		}
	}
	heap_close(type);
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
