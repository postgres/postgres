/*-------------------------------------------------------------------------
 *
 *	  LATINn and MULE_INTERNAL
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mb/conversion_procs/latin_and_mic/latin_and_mic.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"

PG_MODULE_MAGIC_EXT(
					.name = "latin_and_mic",
					.version = PG_VERSION
);

PG_FUNCTION_INFO_V1(latin1_to_mic);
PG_FUNCTION_INFO_V1(mic_to_latin1);
PG_FUNCTION_INFO_V1(latin3_to_mic);
PG_FUNCTION_INFO_V1(mic_to_latin3);
PG_FUNCTION_INFO_V1(latin4_to_mic);
PG_FUNCTION_INFO_V1(mic_to_latin4);

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
latin1_to_mic(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	bool		noError = PG_GETARG_BOOL(5);
	int			converted;

	CHECK_ENCODING_CONVERSION_ARGS(PG_LATIN1, PG_MULE_INTERNAL);

	converted = latin2mic(src, dest, len, LC_ISO8859_1, PG_LATIN1, noError);

	PG_RETURN_INT32(converted);
}

Datum
mic_to_latin1(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	bool		noError = PG_GETARG_BOOL(5);
	int			converted;

	CHECK_ENCODING_CONVERSION_ARGS(PG_MULE_INTERNAL, PG_LATIN1);

	converted = mic2latin(src, dest, len, LC_ISO8859_1, PG_LATIN1, noError);

	PG_RETURN_INT32(converted);
}

Datum
latin3_to_mic(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	bool		noError = PG_GETARG_BOOL(5);
	int			converted;

	CHECK_ENCODING_CONVERSION_ARGS(PG_LATIN3, PG_MULE_INTERNAL);

	converted = latin2mic(src, dest, len, LC_ISO8859_3, PG_LATIN3, noError);

	PG_RETURN_INT32(converted);
}

Datum
mic_to_latin3(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	bool		noError = PG_GETARG_BOOL(5);
	int			converted;

	CHECK_ENCODING_CONVERSION_ARGS(PG_MULE_INTERNAL, PG_LATIN3);

	converted = mic2latin(src, dest, len, LC_ISO8859_3, PG_LATIN3, noError);

	PG_RETURN_INT32(converted);
}

Datum
latin4_to_mic(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	bool		noError = PG_GETARG_BOOL(5);
	int			converted;

	CHECK_ENCODING_CONVERSION_ARGS(PG_LATIN4, PG_MULE_INTERNAL);

	converted = latin2mic(src, dest, len, LC_ISO8859_4, PG_LATIN4, noError);

	PG_RETURN_INT32(converted);
}

Datum
mic_to_latin4(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	bool		noError = PG_GETARG_BOOL(5);
	int			converted;

	CHECK_ENCODING_CONVERSION_ARGS(PG_MULE_INTERNAL, PG_LATIN4);

	converted = mic2latin(src, dest, len, LC_ISO8859_4, PG_LATIN4, noError);

	PG_RETURN_INT32(converted);
}
