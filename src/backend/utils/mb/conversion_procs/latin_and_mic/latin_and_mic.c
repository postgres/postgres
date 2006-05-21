/*-------------------------------------------------------------------------
 *
 *	  LATINn and MULE_INTERNAL
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/mb/conversion_procs/latin_and_mic/latin_and_mic.c,v 1.6.4.1 2006/05/21 20:06:44 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"

PG_FUNCTION_INFO_V1(latin1_to_mic);
PG_FUNCTION_INFO_V1(mic_to_latin1);
PG_FUNCTION_INFO_V1(latin3_to_mic);
PG_FUNCTION_INFO_V1(mic_to_latin3);
PG_FUNCTION_INFO_V1(latin4_to_mic);
PG_FUNCTION_INFO_V1(mic_to_latin4);

extern Datum latin1_to_mic(PG_FUNCTION_ARGS);
extern Datum mic_to_latin1(PG_FUNCTION_ARGS);
extern Datum latin3_to_mic(PG_FUNCTION_ARGS);
extern Datum mic_to_latin3(PG_FUNCTION_ARGS);
extern Datum latin4_to_mic(PG_FUNCTION_ARGS);
extern Datum mic_to_latin4(PG_FUNCTION_ARGS);

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

static void latin12mic(const unsigned char *l, unsigned char *p, int len);
static void mic2latin1(const unsigned char *mic, unsigned char *p, int len);
static void latin32mic(const unsigned char *l, unsigned char *p, int len);
static void mic2latin3(const unsigned char *mic, unsigned char *p, int len);
static void latin42mic(const unsigned char *l, unsigned char *p, int len);
static void mic2latin4(const unsigned char *mic, unsigned char *p, int len);

Datum
latin1_to_mic(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_LATIN1);
	Assert(PG_GETARG_INT32(1) == PG_MULE_INTERNAL);
	Assert(len >= 0);

	latin12mic(src, dest, len);

	PG_RETURN_VOID();
}

Datum
mic_to_latin1(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_MULE_INTERNAL);
	Assert(PG_GETARG_INT32(1) == PG_LATIN1);
	Assert(len >= 0);

	mic2latin1(src, dest, len);

	PG_RETURN_VOID();
}

Datum
latin3_to_mic(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_LATIN3);
	Assert(PG_GETARG_INT32(1) == PG_MULE_INTERNAL);
	Assert(len >= 0);

	latin32mic(src, dest, len);

	PG_RETURN_VOID();
}

Datum
mic_to_latin3(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_MULE_INTERNAL);
	Assert(PG_GETARG_INT32(1) == PG_LATIN3);
	Assert(len >= 0);

	mic2latin3(src, dest, len);

	PG_RETURN_VOID();
}

Datum
latin4_to_mic(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_LATIN4);
	Assert(PG_GETARG_INT32(1) == PG_MULE_INTERNAL);
	Assert(len >= 0);

	latin42mic(src, dest, len);

	PG_RETURN_VOID();
}

Datum
mic_to_latin4(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_MULE_INTERNAL);
	Assert(PG_GETARG_INT32(1) == PG_LATIN4);
	Assert(len >= 0);

	mic2latin4(src, dest, len);

	PG_RETURN_VOID();
}

static void
latin12mic(const unsigned char *l, unsigned char *p, int len)
{
	latin2mic(l, p, len, LC_ISO8859_1, PG_LATIN1);
}

static void
mic2latin1(const unsigned char *mic, unsigned char *p, int len)
{
	mic2latin(mic, p, len, LC_ISO8859_1, PG_LATIN1);
}

static void
latin32mic(const unsigned char *l, unsigned char *p, int len)
{
	latin2mic(l, p, len, LC_ISO8859_3, PG_LATIN3);
}

static void
mic2latin3(const unsigned char *mic, unsigned char *p, int len)
{
	mic2latin(mic, p, len, LC_ISO8859_3, PG_LATIN3);
}

static void
latin42mic(const unsigned char *l, unsigned char *p, int len)
{
	latin2mic(l, p, len, LC_ISO8859_4, PG_LATIN4);
}

static void
mic2latin4(const unsigned char *mic, unsigned char *p, int len)
{
	mic2latin(mic, p, len, LC_ISO8859_4, PG_LATIN4);
}
