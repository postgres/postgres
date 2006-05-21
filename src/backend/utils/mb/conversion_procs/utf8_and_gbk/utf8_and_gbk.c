/*-------------------------------------------------------------------------
 *
 *	  GBK <--> UTF-8
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/mb/conversion_procs/utf8_and_gbk/utf8_and_gbk.c,v 1.6.4.1 2006/05/21 20:06:45 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "../../Unicode/gbk_to_utf8.map"
#include "../../Unicode/utf8_to_gbk.map"

PG_FUNCTION_INFO_V1(gbk_to_utf8);
PG_FUNCTION_INFO_V1(utf8_to_gbk);

extern Datum gbk_to_utf8(PG_FUNCTION_ARGS);
extern Datum utf8_to_gbk(PG_FUNCTION_ARGS);

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
gbk_to_utf8(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_GBK);
	Assert(PG_GETARG_INT32(1) == PG_UTF8);
	Assert(len >= 0);

	LocalToUtf(src, dest, LUmapGBK,
			   sizeof(LUmapGBK) / sizeof(pg_local_to_utf), PG_GBK, len);

	PG_RETURN_VOID();
}

Datum
utf8_to_gbk(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_UTF8);
	Assert(PG_GETARG_INT32(1) == PG_GBK);
	Assert(len >= 0);

	UtfToLocal(src, dest, ULmapGBK,
			   sizeof(ULmapGBK) / sizeof(pg_utf_to_local), PG_GBK, len);

	PG_RETURN_VOID();
}
