/*-------------------------------------------------------------------------
 *
 * regproc.c--
 *    Functions for the built-in type "RegProcedure".
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/adt/regproc.c,v 1.1.1.1 1996/07/09 06:22:05 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"
#include "access/heapam.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "utils/tqual.h"	/* for NowTimeQual */
#include "fmgr.h"
#include "utils/elog.h"
#include "utils/palloc.h"

#include "catalog/catname.h"
#include "utils/builtins.h"	/* where function declarations go */

/***************************************************************************** 
 *   USER I/O ROUTINES                                                       *
 *****************************************************************************/

/*
 *	regprocin	- converts "proname" to proid
 *
 *	proid of NULL signifies unknown
 */
int32 regprocin(char *proname)
{
    Relation	proc;
    HeapScanDesc	procscan;
    HeapTuple	proctup;
    ScanKeyData	       key;		
    RegProcedure	result;
    bool		isnull;
    
    if (proname == NULL)
	return(0);
    proc = heap_openr(ProcedureRelationName);
    if (!RelationIsValid(proc)) {
	elog(WARN, "regprocin: could not open %s",
	     ProcedureRelationName);
	return(0);
    }
    ScanKeyEntryInitialize(&key,
			   (bits16)0, 
			   (AttrNumber)1, 
			   (RegProcedure)F_CHAR16EQ,
			   (Datum)proname);
    
    procscan = heap_beginscan(proc, 0, NowTimeQual, 1, &key);
    if (!HeapScanIsValid(procscan)) {
	heap_close(proc);
	elog(WARN, "regprocin: could not being scan of %s",
	     ProcedureRelationName);
	return(0);
    }
    proctup = heap_getnext(procscan, 0, (Buffer *) NULL);
    switch (HeapTupleIsValid(proctup)) {
    case 1:
	result = (RegProcedure) heap_getattr(proctup,
					  InvalidBuffer,
					  ObjectIdAttributeNumber,
					  RelationGetTupleDescriptor(proc),
					  &isnull);
	if (isnull) {
	    elog(FATAL, "regprocin: null procedure %s", proname);
	}
	break;
    case 0:
	result = (RegProcedure) 0;
#ifdef	EBUG
	elog(DEBUG, "regprocin: no such procedure %s", proname);
#endif	/* defined(EBUG) */
    }
    heap_endscan(procscan);
    heap_close(proc);
    return((int32) result);
}

/*
 *	regprocout	- converts proid to "proname"
 */
char *regprocout(RegProcedure proid)
{
    Relation	proc;
    HeapScanDesc	procscan;
    HeapTuple	proctup;
    char		*result;
    ScanKeyData	key;
    
    result = (char *)palloc(NAMEDATALEN);
    proc = heap_openr(ProcedureRelationName);
    if (!RelationIsValid(proc)) {
	elog(WARN, "regprocout: could not open %s",
	     ProcedureRelationName);
	return(0);
    }
    ScanKeyEntryInitialize(&key,
			   (bits16)0,
			   (AttrNumber)ObjectIdAttributeNumber,
			   (RegProcedure)F_INT4EQ,
			   (Datum)proid);
    
    procscan = heap_beginscan(proc, 0, NowTimeQual, 1, &key);
    if (!HeapScanIsValid(procscan)) {
	heap_close(proc);
	elog(WARN, "regprocin: could not being scan of %s",
	     ProcedureRelationName);
	return(0);
    }
    proctup = heap_getnext(procscan, 0, (Buffer *)NULL);
    switch (HeapTupleIsValid(proctup)) {
	char	*s;
	bool	isnull;
    case 1:
	s = (char *) heap_getattr(proctup, InvalidBuffer, 1,
				  RelationGetTupleDescriptor(proc), &isnull);
	if (!isnull) {
	    strncpy(result, s, 16);
	    break;
	}
	elog(FATAL, "regprocout: null procedure %d", proid);
	/*FALLTHROUGH*/
    case 0:
	memset(result, 0, 16);
	result[0] = '-';
#ifdef	EBUG
	elog(DEBUG, "regprocout: no such procedure %d", proid);
#endif	/* defined(EBUG) */
    }
    heap_endscan(procscan);
    heap_close(proc);
    return(result);
}


/***************************************************************************** 
 *   PUBLIC ROUTINES                                                         *
 *****************************************************************************/

Oid RegprocToOid(RegProcedure rp)
{
    return (Oid)rp;
}

/* (see int.c for comparison/operation routines) */


/* ========== PRIVATE ROUTINES ========== */

