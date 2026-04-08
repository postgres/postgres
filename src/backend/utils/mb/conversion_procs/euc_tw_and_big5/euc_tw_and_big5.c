/*-------------------------------------------------------------------------
 *
 *	  EUC_TW and BIG5
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mb/conversion_procs/euc_tw_and_big5/euc_tw_and_big5.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"

PG_MODULE_MAGIC_EXT(
					.name = "euc_tw_and_big5",
					.version = PG_VERSION
);

PG_FUNCTION_INFO_V1(euc_tw_to_big5);
PG_FUNCTION_INFO_V1(big5_to_euc_tw);

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

static int	euc_tw2big5(const unsigned char *euc, unsigned char *p, int len, bool noError);
static int	big52euc_tw(const unsigned char *big5, unsigned char *p, int len, bool noError);

Datum
euc_tw_to_big5(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	bool		noError = PG_GETARG_BOOL(5);
	int			converted;

	CHECK_ENCODING_CONVERSION_ARGS(PG_EUC_TW, PG_BIG5);

	converted = euc_tw2big5(src, dest, len, noError);

	PG_RETURN_INT32(converted);
}

Datum
big5_to_euc_tw(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	bool		noError = PG_GETARG_BOOL(5);
	int			converted;

	CHECK_ENCODING_CONVERSION_ARGS(PG_BIG5, PG_EUC_TW);

	converted = big52euc_tw(src, dest, len, noError);

	PG_RETURN_INT32(converted);
}

static int
euc_tw2big5(const unsigned char *euc, unsigned char *p, int len, bool noError)
{
	const unsigned char *start = euc;
	unsigned char c1;
	unsigned short big5buf,
				cnsBuf;
	unsigned char lc;
	int			l;

	while (len > 0)
	{
		c1 = *euc;
		if (IS_HIGHBIT_SET(c1))
		{
			/* Verify and decode the next EUC_TW input character */
			l = pg_encoding_verifymbchar(PG_EUC_TW, (const char *) euc, len);
			if (l < 0)
			{
				if (noError)
					break;
				report_invalid_encoding(PG_EUC_TW,
										(const char *) euc, len);
			}
			if (c1 == SS2)
			{
				c1 = euc[1];	/* plane No. */
				if (c1 == 0xa1)
					lc = LC_CNS11643_1;
				else if (c1 == 0xa2)
					lc = LC_CNS11643_2;
				else
					lc = c1 - 0xa3 + LC_CNS11643_3;
				cnsBuf = (euc[2] << 8) | euc[3];
			}
			else
			{					/* CNS11643-1 */
				lc = LC_CNS11643_1;
				cnsBuf = (c1 << 8) | euc[1];
			}

			/* Write it out in Big5 */
			big5buf = CNStoBIG5(cnsBuf, lc);
			if (big5buf == 0)
			{
				if (noError)
					break;
				report_untranslatable_char(PG_EUC_TW, PG_BIG5,
										   (const char *) euc, len);
			}
			*p++ = (big5buf >> 8) & 0x00ff;
			*p++ = big5buf & 0x00ff;

			euc += l;
			len -= l;
		}
		else
		{						/* should be ASCII */
			if (c1 == 0)
			{
				if (noError)
					break;
				report_invalid_encoding(PG_EUC_TW,
										(const char *) euc, len);
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
 * Big5 ---> EUC_TW
 */
static int
big52euc_tw(const unsigned char *big5, unsigned char *p, int len, bool noError)
{
	const unsigned char *start = big5;
	unsigned short c1;
	unsigned short big5buf,
				cnsBuf;
	unsigned char lc;
	int			l;

	while (len > 0)
	{
		/* Verify and decode the next Big5 input character */
		c1 = *big5;
		if (IS_HIGHBIT_SET(c1))
		{
			l = pg_encoding_verifymbchar(PG_BIG5, (const char *) big5, len);
			if (l < 0)
			{
				if (noError)
					break;
				report_invalid_encoding(PG_BIG5,
										(const char *) big5, len);
			}
			big5buf = (c1 << 8) | big5[1];
			cnsBuf = BIG5toCNS(big5buf, &lc);

			if (lc == LC_CNS11643_1)
			{
				*p++ = (cnsBuf >> 8) & 0x00ff;
				*p++ = cnsBuf & 0x00ff;
			}
			else if (lc == LC_CNS11643_2)
			{
				*p++ = SS2;
				*p++ = 0xa2;
				*p++ = (cnsBuf >> 8) & 0x00ff;
				*p++ = cnsBuf & 0x00ff;
			}
			else if (lc >= LC_CNS11643_3 && lc <= LC_CNS11643_7)
			{
				*p++ = SS2;
				*p++ = lc - LC_CNS11643_3 + 0xa3;
				*p++ = (cnsBuf >> 8) & 0x00ff;
				*p++ = cnsBuf & 0x00ff;
			}
			else
			{
				if (noError)
					break;
				report_untranslatable_char(PG_BIG5, PG_EUC_TW,
										   (const char *) big5, len);
			}

			big5 += l;
			len -= l;
		}
		else
		{
			/* ASCII */
			if (c1 == 0)
			{
				if (noError)
					break;
				report_invalid_encoding(PG_BIG5,
										(const char *) big5, len);
			}
			*p++ = c1;
			big5++;
			len--;
			continue;
		}
	}
	*p = '\0';

	return big5 - start;
}
