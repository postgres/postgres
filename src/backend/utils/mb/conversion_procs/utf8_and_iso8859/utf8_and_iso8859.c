/*-------------------------------------------------------------------------
 *
 *	  ISO 8859 2-16 <--> UTF-8
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/mb/conversion_procs/utf8_and_iso8859/utf8_and_iso8859.c,v 1.7.4.1 2006/05/21 20:06:45 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "../../Unicode/utf8_to_iso8859_2.map"
#include "../../Unicode/utf8_to_iso8859_3.map"
#include "../../Unicode/utf8_to_iso8859_4.map"
#include "../../Unicode/utf8_to_iso8859_5.map"
#include "../../Unicode/utf8_to_iso8859_6.map"
#include "../../Unicode/utf8_to_iso8859_7.map"
#include "../../Unicode/utf8_to_iso8859_8.map"
#include "../../Unicode/utf8_to_iso8859_9.map"
#include "../../Unicode/utf8_to_iso8859_10.map"
#include "../../Unicode/utf8_to_iso8859_13.map"
#include "../../Unicode/utf8_to_iso8859_14.map"
#include "../../Unicode/utf8_to_iso8859_15.map"
#include "../../Unicode/utf8_to_iso8859_16.map"
#include "../../Unicode/iso8859_2_to_utf8.map"
#include "../../Unicode/iso8859_3_to_utf8.map"
#include "../../Unicode/iso8859_4_to_utf8.map"
#include "../../Unicode/iso8859_5_to_utf8.map"
#include "../../Unicode/iso8859_6_to_utf8.map"
#include "../../Unicode/iso8859_7_to_utf8.map"
#include "../../Unicode/iso8859_8_to_utf8.map"
#include "../../Unicode/iso8859_9_to_utf8.map"
#include "../../Unicode/iso8859_10_to_utf8.map"
#include "../../Unicode/iso8859_13_to_utf8.map"
#include "../../Unicode/iso8859_14_to_utf8.map"
#include "../../Unicode/iso8859_15_to_utf8.map"
#include "../../Unicode/iso8859_16_to_utf8.map"

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
	pg_local_to_utf *map1;		/* to UTF-8 map name */
	pg_utf_to_local *map2;		/* from UTF-8 map name */
	int			size1;			/* size of map1 */
	int			size2;			/* size of map2 */
}	pg_conv_map;

static pg_conv_map maps[] = {
	{PG_SQL_ASCII},				/* SQL/ASCII */
	{PG_EUC_JP},				/* EUC for Japanese */
	{PG_EUC_CN},				/* EUC for Chinese */
	{PG_EUC_KR},				/* EUC for Korean */
	{PG_EUC_TW},				/* EUC for Taiwan */
	{PG_JOHAB},					/* EUC for Korean JOHAB */
	{PG_UTF8},					/* Unicode UTF-8 */
	{PG_MULE_INTERNAL},			/* Mule internal code */
	{PG_LATIN1},				/* ISO-8859-1 Latin 1 */
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
	{PG_WIN1256},				/* windows-1256 */
	{PG_TCVN},					/* TCVN (Windows-1258) */
	{PG_WIN874},				/* windows-874 */
	{PG_KOI8R},					/* KOI8-R */
	{PG_WIN1251},				/* windows-1251 (was: WIN) */
	{PG_ALT},					/* (MS-DOS CP866) */
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
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(1) == PG_UTF8);
	Assert(len >= 0);

	LocalToUtf(src, dest, maps[encoding].map1, maps[encoding].size1, encoding, len);

	PG_RETURN_VOID();
}

Datum
utf8_to_iso8859(PG_FUNCTION_ARGS)
{
	int			encoding = PG_GETARG_INT32(1);
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_UTF8);
	Assert(len >= 0);

	UtfToLocal(src, dest, maps[encoding].map2, maps[encoding].size2, encoding, len);

	PG_RETURN_VOID();
}
