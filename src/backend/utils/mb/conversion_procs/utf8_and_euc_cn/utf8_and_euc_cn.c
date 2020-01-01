/*-------------------------------------------------------------------------
 *
 *	  EUC_CN <--> UTF8
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mb/conversion_procs/utf8_and_euc_cn/utf8_and_euc_cn.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "../../Unicode/euc_cn_to_utf8.map"
#include "../../Unicode/utf8_to_euc_cn.map"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(euc_cn_to_utf8);
PG_FUNCTION_INFO_V1(utf8_to_euc_cn);

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
euc_cn_to_utf8(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	CHECK_ENCODING_CONVERSION_ARGS(PG_EUC_CN, PG_UTF8);

	LocalToUtf(src, len, dest,
			   &euc_cn_to_unicode_tree,
			   NULL, 0,
			   NULL,
			   PG_EUC_CN);

	PG_RETURN_VOID();
}

Datum
utf8_to_euc_cn(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	CHECK_ENCODING_CONVERSION_ARGS(PG_UTF8, PG_EUC_CN);

	UtfToLocal(src, len, dest,
			   &euc_cn_from_unicode_tree,
			   NULL, 0,
			   NULL,
			   PG_EUC_CN);

	PG_RETURN_VOID();
}
