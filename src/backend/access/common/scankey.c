/*-------------------------------------------------------------------------
 *
 * scan.c
 *	  scan direction and key code
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/common/scankey.c,v 1.22 2003/08/04 02:39:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/skey.h"

/*
 * ScanKeyEntryIsLegal
 *		True iff the scan key entry is legal.
 */
#define ScanKeyEntryIsLegal(entry) \
( \
	AssertMacro(PointerIsValid(entry)), \
	AttributeNumberIsValid((entry)->sk_attno) \
)

/*
 * ScanKeyEntrySetIllegal
 *		Marks a scan key entry as illegal.
 */
void
ScanKeyEntrySetIllegal(ScanKey entry)
{

	Assert(PointerIsValid(entry));

	entry->sk_flags = 0;		/* just in case... */
	entry->sk_attno = InvalidAttrNumber;
	entry->sk_procedure = 0;	/* should be InvalidRegProcedure */
	entry->sk_func.fn_oid = InvalidOid;
	entry->sk_argument = (Datum) 0;
}

/*
 * ScanKeyEntryInitialize
 *		Initializes a scan key entry.
 *
 * Note:
 *		Assumes the scan key entry is valid.
 *		Assumes the intialized scan key entry will be legal.
 */
void
ScanKeyEntryInitialize(ScanKey entry,
					   bits16 flags,
					   AttrNumber attributeNumber,
					   RegProcedure procedure,
					   Datum argument)
{
	Assert(PointerIsValid(entry));

	entry->sk_flags = flags;
	entry->sk_attno = attributeNumber;
	entry->sk_procedure = procedure;
	entry->sk_argument = argument;
	fmgr_info(procedure, &entry->sk_func);

	Assert(ScanKeyEntryIsLegal(entry));
}

/*
 * ScanKeyEntryInitializeWithInfo
 *		Initializes a scan key entry using an already-completed FmgrInfo
 *		function lookup record.
 *
 * mcxt is the memory context holding the scan key; it'll be used for
 * any subsidiary info attached to the scankey's FmgrInfo record.
 */
void
ScanKeyEntryInitializeWithInfo(ScanKey entry,
							   bits16 flags,
							   AttrNumber attributeNumber,
							   FmgrInfo *finfo,
							   MemoryContext mcxt,
							   Datum argument)
{
	Assert(PointerIsValid(entry));
	Assert(RegProcedureIsValid(finfo->fn_oid));

	entry->sk_flags = flags;
	entry->sk_attno = attributeNumber;
	entry->sk_procedure = finfo->fn_oid;
	entry->sk_argument = argument;
	fmgr_info_copy(&entry->sk_func, finfo, mcxt);

	Assert(ScanKeyEntryIsLegal(entry));
}
