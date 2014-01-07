/*-------------------------------------------------------------------------
 *
 *	  GBK <--> UTF8
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mb/conversion_procs/utf8_and_gbk/utf8_and_gbk.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "../../Unicode/gbk_to_utf8.map"
#include "../../Unicode/utf8_to_gbk.map"

PG_MODULE_MAGIC;

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
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	CHECK_ENCODING_CONVERSION_ARGS(PG_GBK, PG_UTF8);

	LocalToUtf(src, dest, LUmapGBK, NULL,
			   sizeof(LUmapGBK) / sizeof(pg_local_to_utf), 0, PG_GBK, len);

	PG_RETURN_VOID();
}

Datum
utf8_to_gbk(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	CHECK_ENCODING_CONVERSION_ARGS(PG_UTF8, PG_GBK);

	UtfToLocal(src, dest, ULmapGBK, NULL,
			   sizeof(ULmapGBK) / sizeof(pg_utf_to_local), 0, PG_GBK, len);

	PG_RETURN_VOID();
}
