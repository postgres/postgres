/*-------------------------------------------------------------------------
 *
 *	  WIN1258 <--> UTF8
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/mb/conversion_procs/utf8_and_win1258/utf8_and_win1258.c,v 1.3.2.1 2006/05/21 20:05:50 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "../../Unicode/win1258_to_utf8.map"
#include "../../Unicode/utf8_to_win1258.map"

PG_FUNCTION_INFO_V1(win1258_to_utf8);
PG_FUNCTION_INFO_V1(utf8_to_win1258);

extern Datum win1258_to_utf8(PG_FUNCTION_ARGS);
extern Datum utf8_to_win1258(PG_FUNCTION_ARGS);

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
win1258_to_utf8(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_WIN1258);
	Assert(PG_GETARG_INT32(1) == PG_UTF8);
	Assert(len >= 0);

	LocalToUtf(src, dest, LUmapWIN1258,
			sizeof(LUmapWIN1258) / sizeof(pg_local_to_utf), PG_WIN1258, len);

	PG_RETURN_VOID();
}

Datum
utf8_to_win1258(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_UTF8);
	Assert(PG_GETARG_INT32(1) == PG_WIN1258);
	Assert(len >= 0);

	UtfToLocal(src, dest, ULmapWIN1258,
			   sizeof(ULmapWIN1258) / sizeof(pg_utf_to_local), PG_WIN1258, len);

	PG_RETURN_VOID();
}
