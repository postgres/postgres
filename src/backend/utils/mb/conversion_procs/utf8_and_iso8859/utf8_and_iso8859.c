/*-------------------------------------------------------------------------
 *
 *	  ISO 8859 2-16 <--> UTF8
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mb/conversion_procs/utf8_and_iso8859/utf8_and_iso8859.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "../../Unicode/iso8859_10_to_utf8.map"
#include "../../Unicode/iso8859_13_to_utf8.map"
#include "../../Unicode/iso8859_14_to_utf8.map"
#include "../../Unicode/iso8859_15_to_utf8.map"
#include "../../Unicode/iso8859_2_to_utf8.map"
#include "../../Unicode/iso8859_3_to_utf8.map"
#include "../../Unicode/iso8859_4_to_utf8.map"
#include "../../Unicode/iso8859_5_to_utf8.map"
#include "../../Unicode/iso8859_6_to_utf8.map"
#include "../../Unicode/iso8859_7_to_utf8.map"
#include "../../Unicode/iso8859_8_to_utf8.map"
#include "../../Unicode/iso8859_9_to_utf8.map"
#include "../../Unicode/utf8_to_iso8859_10.map"
#include "../../Unicode/utf8_to_iso8859_13.map"
#include "../../Unicode/utf8_to_iso8859_14.map"
#include "../../Unicode/utf8_to_iso8859_15.map"
#include "../../Unicode/utf8_to_iso8859_16.map"
#include "../../Unicode/utf8_to_iso8859_2.map"
#include "../../Unicode/utf8_to_iso8859_3.map"
#include "../../Unicode/utf8_to_iso8859_4.map"
#include "../../Unicode/utf8_to_iso8859_5.map"
#include "../../Unicode/utf8_to_iso8859_6.map"
#include "../../Unicode/utf8_to_iso8859_7.map"
#include "../../Unicode/utf8_to_iso8859_8.map"
#include "../../Unicode/utf8_to_iso8859_9.map"
#include "../../Unicode/iso8859_16_to_utf8.map"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(iso8859_to_utf8);
PG_FUNCTION_INFO_V1(utf8_to_iso8859);

extern Datum iso8859_to_utf8(PG_FUNCTION_ARGS);
extern Datum utf8_to_iso8859(PG_FUNCTION_ARGS);

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

typedef struct
{
	pg_enc		encoding;
	pg_local_to_utf *map1;		/* to UTF8 map name */
	pg_utf_to_local *map2;		/* from UTF8 map name */
	int			size1;			/* size of map1 */
	int			size2;			/* size of map2 */
} pg_conv_map;

static pg_conv_map maps[] = {
	{PG_LATIN2, LUmapISO8859_2, ULmapISO8859_2,
		sizeof(LUmapISO8859_2) / sizeof(pg_local_to_utf),
	sizeof(ULmapISO8859_2) / sizeof(pg_utf_to_local)},	/* ISO-8859-2 Latin 2 */
	{PG_LATIN3, LUmapISO8859_3, ULmapISO8859_3,
		sizeof(LUmapISO8859_3) / sizeof(pg_local_to_utf),
	sizeof(ULmapISO8859_3) / sizeof(pg_utf_to_local)},	/* ISO-8859-3 Latin 3 */
	{PG_LATIN4, LUmapISO8859_4, ULmapISO8859_4,
		sizeof(LUmapISO8859_4) / sizeof(pg_local_to_utf),
	sizeof(ULmapISO8859_4) / sizeof(pg_utf_to_local)},	/* ISO-8859-4 Latin 4 */
	{PG_LATIN5, LUmapISO8859_9, ULmapISO8859_9,
		sizeof(LUmapISO8859_9) / sizeof(pg_local_to_utf),
	sizeof(ULmapISO8859_9) / sizeof(pg_utf_to_local)},	/* ISO-8859-9 Latin 5 */
	{PG_LATIN6, LUmapISO8859_10, ULmapISO8859_10,
		sizeof(LUmapISO8859_10) / sizeof(pg_local_to_utf),
	sizeof(ULmapISO8859_10) / sizeof(pg_utf_to_local)}, /* ISO-8859-10 Latin 6 */
	{PG_LATIN7, LUmapISO8859_13, ULmapISO8859_13,
		sizeof(LUmapISO8859_13) / sizeof(pg_local_to_utf),
	sizeof(ULmapISO8859_13) / sizeof(pg_utf_to_local)}, /* ISO-8859-13 Latin 7 */
	{PG_LATIN8, LUmapISO8859_14, ULmapISO8859_14,
		sizeof(LUmapISO8859_14) / sizeof(pg_local_to_utf),
	sizeof(ULmapISO8859_14) / sizeof(pg_utf_to_local)}, /* ISO-8859-14 Latin 8 */
	{PG_LATIN9, LUmapISO8859_15, ULmapISO8859_15,
		sizeof(LUmapISO8859_15) / sizeof(pg_local_to_utf),
	sizeof(ULmapISO8859_15) / sizeof(pg_utf_to_local)}, /* ISO-8859-15 Latin 9 */
	{PG_LATIN10, LUmapISO8859_16, ULmapISO8859_16,
		sizeof(LUmapISO8859_16) / sizeof(pg_local_to_utf),
	sizeof(ULmapISO8859_16) / sizeof(pg_utf_to_local)}, /* ISO-8859-16 Latin 10 */
	{PG_ISO_8859_5, LUmapISO8859_5, ULmapISO8859_5,
		sizeof(LUmapISO8859_5) / sizeof(pg_local_to_utf),
	sizeof(ULmapISO8859_5) / sizeof(pg_utf_to_local)},	/* ISO-8859-5 */
	{PG_ISO_8859_6, LUmapISO8859_6, ULmapISO8859_6,
		sizeof(LUmapISO8859_6) / sizeof(pg_local_to_utf),
	sizeof(ULmapISO8859_6) / sizeof(pg_utf_to_local)},	/* ISO-8859-6 */
	{PG_ISO_8859_7, LUmapISO8859_7, ULmapISO8859_7,
		sizeof(LUmapISO8859_7) / sizeof(pg_local_to_utf),
	sizeof(ULmapISO8859_7) / sizeof(pg_utf_to_local)},	/* ISO-8859-7 */
	{PG_ISO_8859_8, LUmapISO8859_8, ULmapISO8859_8,
		sizeof(LUmapISO8859_8) / sizeof(pg_local_to_utf),
	sizeof(ULmapISO8859_8) / sizeof(pg_utf_to_local)},	/* ISO-8859-8 */
};

Datum
iso8859_to_utf8(PG_FUNCTION_ARGS)
{
	int			encoding = PG_GETARG_INT32(0);
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	int			i;

	CHECK_ENCODING_CONVERSION_ARGS(-1, PG_UTF8);

	for (i = 0; i < sizeof(maps) / sizeof(pg_conv_map); i++)
	{
		if (encoding == maps[i].encoding)
		{
			LocalToUtf(src, dest, maps[i].map1, NULL, maps[i].size1, 0, encoding, len);
			PG_RETURN_VOID();
		}
	}

	ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("unexpected encoding ID %d for ISO 8859 character sets", encoding)));

	PG_RETURN_VOID();
}

Datum
utf8_to_iso8859(PG_FUNCTION_ARGS)
{
	int			encoding = PG_GETARG_INT32(1);
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	int			i;

	CHECK_ENCODING_CONVERSION_ARGS(PG_UTF8, -1);

	for (i = 0; i < sizeof(maps) / sizeof(pg_conv_map); i++)
	{
		if (encoding == maps[i].encoding)
		{
			UtfToLocal(src, dest, maps[i].map2, NULL, maps[i].size2, 0, encoding, len);
			PG_RETURN_VOID();
		}
	}

	ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("unexpected encoding ID %d for ISO 8859 character sets", encoding)));

	PG_RETURN_VOID();
}
