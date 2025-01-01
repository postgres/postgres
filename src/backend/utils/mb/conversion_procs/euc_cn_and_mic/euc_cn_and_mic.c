/*-------------------------------------------------------------------------
 *
 *	  EUC_CN and MULE_INTERNAL
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mb/conversion_procs/euc_cn_and_mic/euc_cn_and_mic.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(euc_cn_to_mic);
PG_FUNCTION_INFO_V1(mic_to_euc_cn);

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

static int	euc_cn2mic(const unsigned char *euc, unsigned char *p, int len, bool noError);
static int	mic2euc_cn(const unsigned char *mic, unsigned char *p, int len, bool noError);

Datum
euc_cn_to_mic(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	bool		noError = PG_GETARG_BOOL(5);
	int			converted;

	CHECK_ENCODING_CONVERSION_ARGS(PG_EUC_CN, PG_MULE_INTERNAL);

	converted = euc_cn2mic(src, dest, len, noError);

	PG_RETURN_INT32(converted);
}

Datum
mic_to_euc_cn(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	bool		noError = PG_GETARG_BOOL(5);
	int			converted;

	CHECK_ENCODING_CONVERSION_ARGS(PG_MULE_INTERNAL, PG_EUC_CN);

	converted = mic2euc_cn(src, dest, len, noError);

	PG_RETURN_INT32(converted);
}

/*
 * EUC_CN ---> MIC
 */
static int
euc_cn2mic(const unsigned char *euc, unsigned char *p, int len, bool noError)
{
	const unsigned char *start = euc;
	int			c1;

	while (len > 0)
	{
		c1 = *euc;
		if (IS_HIGHBIT_SET(c1))
		{
			if (len < 2 || !IS_HIGHBIT_SET(euc[1]))
			{
				if (noError)
					break;
				report_invalid_encoding(PG_EUC_CN, (const char *) euc, len);
			}
			*p++ = LC_GB2312_80;
			*p++ = c1;
			*p++ = euc[1];
			euc += 2;
			len -= 2;
		}
		else
		{						/* should be ASCII */
			if (c1 == 0)
			{
				if (noError)
					break;
				report_invalid_encoding(PG_EUC_CN, (const char *) euc, len);
			}
			*p++ = c1;
			euc++;
			len--;
		}
	}
	*p = '\0';

	return euc - start;
}

/*
 * MIC ---> EUC_CN
 */
static int
mic2euc_cn(const unsigned char *mic, unsigned char *p, int len, bool noError)
{
	const unsigned char *start = mic;
	int			c1;

	while (len > 0)
	{
		c1 = *mic;
		if (IS_HIGHBIT_SET(c1))
		{
			if (c1 != LC_GB2312_80)
			{
				if (noError)
					break;
				report_untranslatable_char(PG_MULE_INTERNAL, PG_EUC_CN,
										   (const char *) mic, len);
			}
			if (len < 3 || !IS_HIGHBIT_SET(mic[1]) || !IS_HIGHBIT_SET(mic[2]))
			{
				if (noError)
					break;
				report_invalid_encoding(PG_MULE_INTERNAL,
										(const char *) mic, len);
			}
			mic++;
			*p++ = *mic++;
			*p++ = *mic++;
			len -= 3;
		}
		else
		{						/* should be ASCII */
			if (c1 == 0)
			{
				if (noError)
					break;
				report_invalid_encoding(PG_MULE_INTERNAL,
										(const char *) mic, len);
			}
			*p++ = c1;
			mic++;
			len--;
		}
	}
	*p = '\0';

	return mic - start;
}
