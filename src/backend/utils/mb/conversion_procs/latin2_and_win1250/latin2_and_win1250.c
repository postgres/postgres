/*-------------------------------------------------------------------------
 *
 *	  LATIN2 and WIN1250
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mb/conversion_procs/latin2_and_win1250/latin2_and_win1250.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"

#define ENCODING_GROWTH_RATE 4

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(latin2_to_mic);
PG_FUNCTION_INFO_V1(mic_to_latin2);
PG_FUNCTION_INFO_V1(win1250_to_mic);
PG_FUNCTION_INFO_V1(mic_to_win1250);
PG_FUNCTION_INFO_V1(latin2_to_win1250);
PG_FUNCTION_INFO_V1(win1250_to_latin2);

extern Datum latin2_to_mic(PG_FUNCTION_ARGS);
extern Datum mic_to_latin2(PG_FUNCTION_ARGS);
extern Datum win1250_to_mic(PG_FUNCTION_ARGS);
extern Datum mic_to_win1250(PG_FUNCTION_ARGS);
extern Datum latin2_to_win1250(PG_FUNCTION_ARGS);
extern Datum win1250_to_latin2(PG_FUNCTION_ARGS);

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

static void latin22mic(const unsigned char *l, unsigned char *p, int len);
static void mic2latin2(const unsigned char *mic, unsigned char *p, int len);
static void win12502mic(const unsigned char *l, unsigned char *p, int len);
static void mic2win1250(const unsigned char *mic, unsigned char *p, int len);

Datum
latin2_to_mic(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	CHECK_ENCODING_CONVERSION_ARGS(PG_LATIN2, PG_MULE_INTERNAL);

	latin22mic(src, dest, len);

	PG_RETURN_VOID();
}

Datum
mic_to_latin2(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	CHECK_ENCODING_CONVERSION_ARGS(PG_MULE_INTERNAL, PG_LATIN2);

	mic2latin2(src, dest, len);

	PG_RETURN_VOID();
}

Datum
win1250_to_mic(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	CHECK_ENCODING_CONVERSION_ARGS(PG_WIN1250, PG_MULE_INTERNAL);

	win12502mic(src, dest, len);

	PG_RETURN_VOID();
}

Datum
mic_to_win1250(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	CHECK_ENCODING_CONVERSION_ARGS(PG_MULE_INTERNAL, PG_WIN1250);

	mic2win1250(src, dest, len);

	PG_RETURN_VOID();
}

Datum
latin2_to_win1250(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	unsigned char *buf;

	CHECK_ENCODING_CONVERSION_ARGS(PG_LATIN2, PG_WIN1250);

	buf = palloc(len * ENCODING_GROWTH_RATE + 1);
	latin22mic(src, buf, len);
	mic2win1250(buf, dest, strlen((char *) buf));
	pfree(buf);

	PG_RETURN_VOID();
}

Datum
win1250_to_latin2(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	unsigned char *buf;

	CHECK_ENCODING_CONVERSION_ARGS(PG_WIN1250, PG_LATIN2);

	buf = palloc(len * ENCODING_GROWTH_RATE + 1);
	win12502mic(src, buf, len);
	mic2latin2(buf, dest, strlen((char *) buf));
	pfree(buf);

	PG_RETURN_VOID();
}

static void
latin22mic(const unsigned char *l, unsigned char *p, int len)
{
	latin2mic(l, p, len, LC_ISO8859_2, PG_LATIN2);
}

static void
mic2latin2(const unsigned char *mic, unsigned char *p, int len)
{
	mic2latin(mic, p, len, LC_ISO8859_2, PG_LATIN2);
}

/*-----------------------------------------------------------------
 * WIN1250
 * Microsoft's CP1250(windows-1250)
 *-----------------------------------------------------------------*/
static void
win12502mic(const unsigned char *l, unsigned char *p, int len)
{
	static const unsigned char win1250_2_iso88592[] = {
		0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
		0x88, 0x89, 0xA9, 0x8B, 0xA6, 0xAB, 0xAE, 0xAC,
		0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
		0x98, 0x99, 0xB9, 0x9B, 0xB6, 0xBB, 0xBE, 0xBC,
		0xA0, 0xB7, 0xA2, 0xA3, 0xA4, 0xA1, 0x00, 0xA7,
		0xA8, 0x00, 0xAA, 0x00, 0x00, 0xAD, 0x00, 0xAF,
		0xB0, 0x00, 0xB2, 0xB3, 0xB4, 0x00, 0x00, 0x00,
		0xB8, 0xB1, 0xBA, 0x00, 0xA5, 0xBD, 0xB5, 0xBF,
		0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7,
		0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF,
		0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7,
		0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF,
		0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7,
		0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF,
		0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7,
		0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF
	};

	latin2mic_with_table(l, p, len, LC_ISO8859_2, PG_WIN1250,
						 win1250_2_iso88592);
}

static void
mic2win1250(const unsigned char *mic, unsigned char *p, int len)
{
	static const unsigned char iso88592_2_win1250[] = {
		0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
		0x88, 0x89, 0x00, 0x8B, 0x00, 0x00, 0x00, 0x00,
		0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
		0x98, 0x99, 0x00, 0x9B, 0x00, 0x00, 0x00, 0x00,
		0xA0, 0xA5, 0xA2, 0xA3, 0xA4, 0xBC, 0x8C, 0xA7,
		0xA8, 0x8A, 0xAA, 0x8D, 0x8F, 0xAD, 0x8E, 0xAF,
		0xB0, 0xB9, 0xB2, 0xB3, 0xB4, 0xBE, 0x9C, 0xA1,
		0xB8, 0x9A, 0xBA, 0x9D, 0x9F, 0xBD, 0x9E, 0xBF,
		0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7,
		0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF,
		0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7,
		0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF,
		0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7,
		0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF,
		0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7,
		0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF
	};

	mic2latin_with_table(mic, p, len, LC_ISO8859_2, PG_WIN1250,
						 iso88592_2_win1250);
}
