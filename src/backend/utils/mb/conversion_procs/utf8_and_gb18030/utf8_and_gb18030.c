/*-------------------------------------------------------------------------
 *
 *	  GB18030 <--> UTF8
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mb/conversion_procs/utf8_and_gb18030/utf8_and_gb18030.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "../../Unicode/gb18030_to_utf8.map"
#include "../../Unicode/utf8_to_gb18030.map"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(gb18030_to_utf8);
PG_FUNCTION_INFO_V1(utf8_to_gb18030);

extern Datum gb18030_to_utf8(PG_FUNCTION_ARGS);
extern Datum utf8_to_gb18030(PG_FUNCTION_ARGS);

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
gb18030_to_utf8(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	CHECK_ENCODING_CONVERSION_ARGS(PG_GB18030, PG_UTF8);

	LocalToUtf(src, dest, LUmapGB18030, NULL,
		 sizeof(LUmapGB18030) / sizeof(pg_local_to_utf), 0, PG_GB18030, len);

	PG_RETURN_VOID();
}

Datum
utf8_to_gb18030(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	CHECK_ENCODING_CONVERSION_ARGS(PG_UTF8, PG_GB18030);

	UtfToLocal(src, dest, ULmapGB18030, NULL,
		 sizeof(ULmapGB18030) / sizeof(pg_utf_to_local), 0, PG_GB18030, len);

	PG_RETURN_VOID();
}
