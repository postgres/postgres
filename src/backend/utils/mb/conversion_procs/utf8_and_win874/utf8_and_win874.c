/*-------------------------------------------------------------------------
 *
 *	  WIN874 and UTF-8
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/mb/conversion_procs/utf8_and_win874/Attic/utf8_and_win874.c,v 1.6.4.1 2006/05/21 20:06:45 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "../../Unicode/utf8_to_win874.map"
#include "../../Unicode/win874_to_utf8.map"

PG_FUNCTION_INFO_V1(utf_to_win874);
PG_FUNCTION_INFO_V1(win874_to_utf);

extern Datum utf_to_win874(PG_FUNCTION_ARGS);
extern Datum win874_to_utf(PG_FUNCTION_ARGS);

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
utf_to_win874(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_UTF8);
	Assert(PG_GETARG_INT32(1) == PG_WIN874);
	Assert(len >= 0);

	UtfToLocal(src, dest, ULmapWIN874,
			   sizeof(ULmapWIN874) / sizeof(pg_utf_to_local), PG_WIN874, len);

	PG_RETURN_VOID();
}

Datum
win874_to_utf(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_WIN874);
	Assert(PG_GETARG_INT32(1) == PG_UTF8);
	Assert(len >= 0);

	LocalToUtf(src, dest, LUmapWIN874,
		  sizeof(LUmapWIN874) / sizeof(pg_local_to_utf), PG_WIN874, len);

	PG_RETURN_VOID();
}
