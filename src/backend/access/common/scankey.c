/*-------------------------------------------------------------------------
 *
 * scankey.c
 *	  scan key support code
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/common/scankey.c,v 1.23 2003/11/09 21:30:35 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/skey.h"


/*
 * ScanKeyEntryInitialize
 *		Initializes a scan key entry given all the field values.
 *		The target procedure is specified by OID.
 *
 * Note: CurrentMemoryContext at call should be as long-lived as the ScanKey
 * itself, because that's what will be used for any subsidiary info attached
 * to the ScanKey's FmgrInfo record.
 */
void
ScanKeyEntryInitialize(ScanKey entry,
					   int flags,
					   AttrNumber attributeNumber,
					   StrategyNumber strategy,
					   RegProcedure procedure,
					   Datum argument,
					   Oid argtype)
{
	entry->sk_flags = flags;
	entry->sk_attno = attributeNumber;
	entry->sk_strategy = strategy;
	entry->sk_argument = argument;
	entry->sk_argtype = argtype;
	fmgr_info(procedure, &entry->sk_func);
}

/*
 * ScanKeyEntryInitializeWithInfo
 *		Initializes a scan key entry using an already-completed FmgrInfo
 *		function lookup record.
 *
 * Note: CurrentMemoryContext at call should be as long-lived as the ScanKey
 * itself, because that's what will be used for any subsidiary info attached
 * to the ScanKey's FmgrInfo record.
 */
void
ScanKeyEntryInitializeWithInfo(ScanKey entry,
							   int flags,
							   AttrNumber attributeNumber,
							   StrategyNumber strategy,
							   FmgrInfo *finfo,
							   Datum argument,
							   Oid argtype)
{
	entry->sk_flags = flags;
	entry->sk_attno = attributeNumber;
	entry->sk_strategy = strategy;
	entry->sk_argument = argument;
	entry->sk_argtype = argtype;
	fmgr_info_copy(&entry->sk_func, finfo, CurrentMemoryContext);
}
