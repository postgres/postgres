/*-------------------------------------------------------------------------
 *
 *	  ISO 8859 2-16 <--> UTF8
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
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
	const pg_local_to_utf *map1;	/* to UTF8 map name */
	const pg_utf_to_local *map2;	/* from UTF8 map name */
	int			size1;			/* size of map1 */
	int			size2;			/* size of map2 */
} pg_conv_map;

static const pg_conv_map maps[] = {
	{PG_LATIN2, LUmapISO8859_2, ULmapISO8859_2,
		lengthof(LUmapISO8859_2),
	lengthof(ULmapISO8859_2)},	/* ISO-8859-2 Latin 2 */
	{PG_LATIN3, LUmapISO8859_3, ULmapISO8859_3,
		lengthof(LUmapISO8859_3),
	lengthof(ULmapISO8859_3)},	/* ISO-8859-3 Latin 3 */
	{PG_LATIN4, LUmapISO8859_4, ULmapISO8859_4,
		lengthof(LUmapISO8859_4),
	lengthof(ULmapISO8859_4)},	/* ISO-8859-4 Latin 4 */
	{PG_LATIN5, LUmapISO8859_9, ULmapISO8859_9,
		lengthof(LUmapISO8859_9),
	lengthof(ULmapISO8859_9)},	/* ISO-8859-9 Latin 5 */
	{PG_LATIN6, LUmapISO8859_10, ULmapISO8859_10,
		lengthof(LUmapISO8859_10),
	lengthof(ULmapISO8859_10)}, /* ISO-8859-10 Latin 6 */
	{PG_LATIN7, LUmapISO8859_13, ULmapISO8859_13,
		lengthof(LUmapISO8859_13),
	lengthof(ULmapISO8859_13)}, /* ISO-8859-13 Latin 7 */
	{PG_LATIN8, LUmapISO8859_14, ULmapISO8859_14,
		lengthof(LUmapISO8859_14),
	lengthof(ULmapISO8859_14)}, /* ISO-8859-14 Latin 8 */
	{PG_LATIN9, LUmapISO8859_15, ULmapISO8859_15,
		lengthof(LUmapISO8859_15),
	lengthof(ULmapISO8859_15)}, /* ISO-8859-15 Latin 9 */
	{PG_LATIN10, LUmapISO8859_16, ULmapISO8859_16,
		lengthof(LUmapISO8859_16),
	lengthof(ULmapISO8859_16)}, /* ISO-8859-16 Latin 10 */
	{PG_ISO_8859_5, LUmapISO8859_5, ULmapISO8859_5,
		lengthof(LUmapISO8859_5),
	lengthof(ULmapISO8859_5)},	/* ISO-8859-5 */
	{PG_ISO_8859_6, LUmapISO8859_6, ULmapISO8859_6,
		lengthof(LUmapISO8859_6),
	lengthof(ULmapISO8859_6)},	/* ISO-8859-6 */
	{PG_ISO_8859_7, LUmapISO8859_7, ULmapISO8859_7,
		lengthof(LUmapISO8859_7),
	lengthof(ULmapISO8859_7)},	/* ISO-8859-7 */
	{PG_ISO_8859_8, LUmapISO8859_8, ULmapISO8859_8,
		lengthof(LUmapISO8859_8),
	lengthof(ULmapISO8859_8)},	/* ISO-8859-8 */
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

	for (i = 0; i < lengthof(maps); i++)
	{
		if (encoding == maps[i].encoding)
		{
			LocalToUtf(src, len, dest,
					   maps[i].map1, maps[i].size1,
					   NULL, 0,
					   NULL,
					   encoding);
			PG_RETURN_VOID();
		}
	}

	ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("unexpected encoding ID %d for ISO 8859 character sets",
					encoding)));

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

	for (i = 0; i < lengthof(maps); i++)
	{
		if (encoding == maps[i].encoding)
		{
			UtfToLocal(src, len, dest,
					   maps[i].map2, maps[i].size2,
					   NULL, 0,
					   NULL,
					   encoding);
			PG_RETURN_VOID();
		}
	}

	ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("unexpected encoding ID %d for ISO 8859 character sets",
					encoding)));

	PG_RETURN_VOID();
}
