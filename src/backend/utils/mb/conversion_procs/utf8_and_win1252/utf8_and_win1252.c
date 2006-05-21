/*-------------------------------------------------------------------------
 *
 *	  WIN1252 and UTF8
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/mb/conversion_procs/utf8_and_win1252/utf8_and_win1252.c,v 1.5.2.1 2006/05/21 20:05:50 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "../../Unicode/utf8_to_win1252.map"
#include "../../Unicode/win1252_to_utf8.map"

PG_FUNCTION_INFO_V1(utf8_to_win1252);
PG_FUNCTION_INFO_V1(win1252_to_utf8);

extern Datum utf8_to_win1252(PG_FUNCTION_ARGS);
extern Datum win1252_to_utf8(PG_FUNCTION_ARGS);

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
utf8_to_win1252(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_UTF8);
	Assert(PG_GETARG_INT32(1) == PG_WIN1252);
	Assert(len >= 0);

	UtfToLocal(src, dest, ULmapWIN1252,
			   sizeof(ULmapWIN1252) / sizeof(pg_utf_to_local), PG_WIN1252, len);

	PG_RETURN_VOID();
}

Datum
win1252_to_utf8(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_WIN1252);
	Assert(PG_GETARG_INT32(1) == PG_UTF8);
	Assert(len >= 0);

	LocalToUtf(src, dest, LUmapWIN1252,
			sizeof(LUmapWIN1252) / sizeof(pg_local_to_utf), PG_WIN1252, len);

	PG_RETURN_VOID();
}
