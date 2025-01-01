/*-------------------------------------------------------------------------
 *
 *	  GB18030 <--> UTF8
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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

/*
 * Convert 4-byte GB18030 characters to and from a linear code space
 *
 * The first and third bytes can range from 0x81 to 0xfe (126 values),
 * while the second and fourth bytes can range from 0x30 to 0x39 (10 values).
 */
static inline uint32
gb_linear(uint32 gb)
{
	uint32		b0 = (gb & 0xff000000) >> 24;
	uint32		b1 = (gb & 0x00ff0000) >> 16;
	uint32		b2 = (gb & 0x0000ff00) >> 8;
	uint32		b3 = (gb & 0x000000ff);

	return b0 * 12600 + b1 * 1260 + b2 * 10 + b3 -
		(0x81 * 12600 + 0x30 * 1260 + 0x81 * 10 + 0x30);
}

static inline uint32
gb_unlinear(uint32 lin)
{
	uint32		r0 = 0x81 + lin / 12600;
	uint32		r1 = 0x30 + (lin / 1260) % 10;
	uint32		r2 = 0x81 + (lin / 10) % 126;
	uint32		r3 = 0x30 + lin % 10;

	return (r0 << 24) | (r1 << 16) | (r2 << 8) | r3;
}

/*
 * Convert word-formatted UTF8 to and from Unicode code points
 *
 * Probably this should be somewhere else ...
 */
static inline uint32
unicode_to_utf8word(uint32 c)
{
	uint32		word;

	if (c <= 0x7F)
	{
		word = c;
	}
	else if (c <= 0x7FF)
	{
		word = (0xC0 | ((c >> 6) & 0x1F)) << 8;
		word |= 0x80 | (c & 0x3F);
	}
	else if (c <= 0xFFFF)
	{
		word = (0xE0 | ((c >> 12) & 0x0F)) << 16;
		word |= (0x80 | ((c >> 6) & 0x3F)) << 8;
		word |= 0x80 | (c & 0x3F);
	}
	else
	{
		word = (0xF0 | ((c >> 18) & 0x07)) << 24;
		word |= (0x80 | ((c >> 12) & 0x3F)) << 16;
		word |= (0x80 | ((c >> 6) & 0x3F)) << 8;
		word |= 0x80 | (c & 0x3F);
	}

	return word;
}

static inline uint32
utf8word_to_unicode(uint32 c)
{
	uint32		ucs;

	if (c <= 0x7F)
	{
		ucs = c;
	}
	else if (c <= 0xFFFF)
	{
		ucs = ((c >> 8) & 0x1F) << 6;
		ucs |= c & 0x3F;
	}
	else if (c <= 0xFFFFFF)
	{
		ucs = ((c >> 16) & 0x0F) << 12;
		ucs |= ((c >> 8) & 0x3F) << 6;
		ucs |= c & 0x3F;
	}
	else
	{
		ucs = ((c >> 24) & 0x07) << 18;
		ucs |= ((c >> 16) & 0x3F) << 12;
		ucs |= ((c >> 8) & 0x3F) << 6;
		ucs |= c & 0x3F;
	}

	return ucs;
}

/*
 * Perform mapping of GB18030 ranges to UTF8
 *
 * The ranges we need to convert are specified in gb-18030-2000.xml.
 * All are ranges of 4-byte GB18030 codes.
 */
static uint32
conv_18030_to_utf8(uint32 code)
{
#define conv18030(minunicode, mincode, maxcode) \
	if (code >= mincode && code <= maxcode) \
		return unicode_to_utf8word(gb_linear(code) - gb_linear(mincode) + minunicode)

	conv18030(0x0452, 0x8130D330, 0x8136A531);
	conv18030(0x2643, 0x8137A839, 0x8138FD38);
	conv18030(0x361B, 0x8230A633, 0x8230F237);
	conv18030(0x3CE1, 0x8231D438, 0x8232AF32);
	conv18030(0x4160, 0x8232C937, 0x8232F837);
	conv18030(0x44D7, 0x8233A339, 0x8233C931);
	conv18030(0x478E, 0x8233E838, 0x82349638);
	conv18030(0x49B8, 0x8234A131, 0x8234E733);
	conv18030(0x9FA6, 0x82358F33, 0x8336C738);
	conv18030(0xE865, 0x8336D030, 0x84308534);
	conv18030(0xFA2A, 0x84309C38, 0x84318537);
	conv18030(0xFFE6, 0x8431A234, 0x8431A439);
	conv18030(0x10000, 0x90308130, 0xE3329A35);
	/* No mapping exists */
	return 0;
}

/*
 * Perform mapping of UTF8 ranges to GB18030
 */
static uint32
conv_utf8_to_18030(uint32 code)
{
	uint32		ucs = utf8word_to_unicode(code);

#define convutf8(minunicode, maxunicode, mincode) \
	if (ucs >= minunicode && ucs <= maxunicode) \
		return gb_unlinear(ucs - minunicode + gb_linear(mincode))

	convutf8(0x0452, 0x200F, 0x8130D330);
	convutf8(0x2643, 0x2E80, 0x8137A839);
	convutf8(0x361B, 0x3917, 0x8230A633);
	convutf8(0x3CE1, 0x4055, 0x8231D438);
	convutf8(0x4160, 0x4336, 0x8232C937);
	convutf8(0x44D7, 0x464B, 0x8233A339);
	convutf8(0x478E, 0x4946, 0x8233E838);
	convutf8(0x49B8, 0x4C76, 0x8234A131);
	convutf8(0x9FA6, 0xD7FF, 0x82358F33);
	convutf8(0xE865, 0xF92B, 0x8336D030);
	convutf8(0xFA2A, 0xFE2F, 0x84309C38);
	convutf8(0xFFE6, 0xFFFF, 0x8431A234);
	convutf8(0x10000, 0x10FFFF, 0x90308130);
	/* No mapping exists */
	return 0;
}

/* ----------
 * conv_proc(
 *		INTEGER,	-- source encoding id
 *		INTEGER,	-- destination encoding id
 *		CSTRING,	-- source string (null terminated C string)
 *		CSTRING,	-- destination string (null terminated C string)
 *		INTEGER,	-- source string length
 *		BOOL		-- if true, don't throw an error if conversion fails
 * ) returns INTEGER;
 *
 * Returns the number of bytes successfully converted.
 * ----------
 */
Datum
gb18030_to_utf8(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	bool		noError = PG_GETARG_BOOL(5);
	int			converted;

	CHECK_ENCODING_CONVERSION_ARGS(PG_GB18030, PG_UTF8);

	converted = LocalToUtf(src, len, dest,
						   &gb18030_to_unicode_tree,
						   NULL, 0,
						   conv_18030_to_utf8,
						   PG_GB18030,
						   noError);

	PG_RETURN_INT32(converted);
}

Datum
utf8_to_gb18030(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	bool		noError = PG_GETARG_BOOL(5);
	int			converted;

	CHECK_ENCODING_CONVERSION_ARGS(PG_UTF8, PG_GB18030);

	converted = UtfToLocal(src, len, dest,
						   &gb18030_from_unicode_tree,
						   NULL, 0,
						   conv_utf8_to_18030,
						   PG_GB18030,
						   noError);

	PG_RETURN_INT32(converted);
}
