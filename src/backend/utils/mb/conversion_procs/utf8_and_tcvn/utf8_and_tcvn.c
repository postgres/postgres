/*-------------------------------------------------------------------------
 *
 *	  TCVN <--> UTF-8
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/mb/conversion_procs/utf8_and_tcvn/utf8_and_tcvn.c,v 1.8 2004/08/29 04:12:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "../../Unicode/tcvn_to_utf8.map"
#include "../../Unicode/utf8_to_tcvn.map"

PG_FUNCTION_INFO_V1(tcvn_to_utf8);
PG_FUNCTION_INFO_V1(utf8_to_tcvn);

extern Datum tcvn_to_utf8(PG_FUNCTION_ARGS);
extern Datum utf8_to_tcvn(PG_FUNCTION_ARGS);

/* ----------
 * conv_proc(
 *		INTEGER,	-- source encoding id
 *		INTEGER,	-- destination encoding id
 *		CSTRING,	-- source string (null terminated C string)
 *		CSTRING,	-- destination string (null terminated C string)
 *		INTEGER		-- source string length
 * ) returns VOID;
 * ----------
 */
Datum
tcvn_to_utf8(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_TCVN);
	Assert(PG_GETARG_INT32(1) == PG_UTF8);
	Assert(len >= 0);

	LocalToUtf(src, dest, LUmapTCVN,
			   sizeof(LUmapTCVN) / sizeof(pg_local_to_utf), PG_TCVN, len);

	PG_RETURN_VOID();
}

Datum
utf8_to_tcvn(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_UTF8);
	Assert(PG_GETARG_INT32(1) == PG_TCVN);
	Assert(len >= 0);

	UtfToLocal(src, dest, ULmapTCVN,
			   sizeof(ULmapTCVN) / sizeof(pg_utf_to_local), len);

	PG_RETURN_VOID();
}
