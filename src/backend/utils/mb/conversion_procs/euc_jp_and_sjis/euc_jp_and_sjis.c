/*-------------------------------------------------------------------------
 *
 *	  EUC_JP and SJIS
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mb/conversion_procs/euc_jp_and_sjis/euc_jp_and_sjis.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"

/*
 * SJIS alternative code.
 * this code is used if a mapping EUC -> SJIS is not defined.
 */
#define PGSJISALTCODE 0x81ac
#define PGEUCALTCODE 0xa2ae

/*
 * conversion table between SJIS UDC (IBM kanji) and EUC_JP
 */
#include "sjis.map"

PG_MODULE_MAGIC_EXT(
					.name = "euc_jp_and_sjis",
					.version = PG_VERSION
);

PG_FUNCTION_INFO_V1(euc_jp_to_sjis);
PG_FUNCTION_INFO_V1(sjis_to_euc_jp);

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

static int	euc_jp2sjis(const unsigned char *euc, unsigned char *p, int len, bool noError);
static int	sjis2euc_jp(const unsigned char *sjis, unsigned char *p, int len, bool noError);

Datum
euc_jp_to_sjis(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	bool		noError = PG_GETARG_BOOL(5);
	int			converted;

	CHECK_ENCODING_CONVERSION_ARGS(PG_EUC_JP, PG_SJIS);

	converted = euc_jp2sjis(src, dest, len, noError);

	PG_RETURN_INT32(converted);
}

Datum
sjis_to_euc_jp(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	bool		noError = PG_GETARG_BOOL(5);
	int			converted;

	CHECK_ENCODING_CONVERSION_ARGS(PG_SJIS, PG_EUC_JP);

	converted = sjis2euc_jp(src, dest, len, noError);

	PG_RETURN_INT32(converted);
}

/*
 * EUC_JP -> SJIS
 */
static int
euc_jp2sjis(const unsigned char *euc, unsigned char *p, int len, bool noError)
{
	const unsigned char *start = euc;
	int			c1,
				c2,
				k;
	int			l;

	while (len > 0)
	{
		c1 = *euc;
		if (!IS_HIGHBIT_SET(c1))
		{
			/* ASCII */
			if (c1 == 0)
			{
				if (noError)
					break;
				report_invalid_encoding(PG_EUC_JP,
										(const char *) euc, len);
			}
			*p++ = c1;
			euc++;
			len--;
			continue;
		}
		l = pg_encoding_verifymbchar(PG_EUC_JP, (const char *) euc, len);
		if (l < 0)
		{
			if (noError)
				break;
			report_invalid_encoding(PG_EUC_JP,
									(const char *) euc, len);
		}
		if (c1 == SS2)
		{
			/* hankaku kana? */
			*p++ = euc[1];
		}
		else if (c1 == SS3)
		{
			/* JIS X0212 kanji? */
			c1 = euc[1];
			c2 = euc[2];
			k = c1 << 8 | c2;
			if (k >= 0xf5a1)
			{
				/* UDC2 */
				c1 -= 0x54;
				*p++ = ((c1 - 0xa1) >> 1) + ((c1 < 0xdf) ? 0x81 : 0xc1) + 0x74;
				*p++ = c2 - ((c1 & 1) ? ((c2 < 0xe0) ? 0x61 : 0x60) : 2);
			}
			else
			{
				int			i,
							k2;

				/* IBM kanji */
				for (i = 0;; i++)
				{
					k2 = ibmkanji[i].euc & 0xffff;
					if (k2 == 0xffff)
					{
						*p++ = PGSJISALTCODE >> 8;
						*p++ = PGSJISALTCODE & 0xff;
						break;
					}
					if (k2 == k)
					{
						k = ibmkanji[i].sjis;
						*p++ = k >> 8;
						*p++ = k & 0xff;
						break;
					}
				}
			}
		}
		else
		{
			/* JIS X0208 kanji? */
			c2 = euc[1];
			k = (c1 << 8) | (c2 & 0xff);
			if (k >= 0xf5a1)
			{
				/* UDC1 */
				c1 -= 0x54;
				*p++ = ((c1 - 0xa1) >> 1) + ((c1 < 0xdf) ? 0x81 : 0xc1) + 0x6f;
			}
			else
				*p++ = ((c1 - 0xa1) >> 1) + ((c1 < 0xdf) ? 0x81 : 0xc1);
			*p++ = c2 - ((c1 & 1) ? ((c2 < 0xe0) ? 0x61 : 0x60) : 2);
		}
		euc += l;
		len -= l;
	}
	*p = '\0';

	return euc - start;
}

/*
 * SJIS ---> EUC_JP
 */
static int
sjis2euc_jp(const unsigned char *sjis, unsigned char *p, int len, bool noError)
{
	const unsigned char *start = sjis;
	int			c1,
				c2,
				i,
				k,
				k2;
	int			l;

	while (len > 0)
	{
		c1 = *sjis;
		if (!IS_HIGHBIT_SET(c1))
		{
			/* ASCII */
			if (c1 == 0)
			{
				if (noError)
					break;
				report_invalid_encoding(PG_SJIS,
										(const char *) sjis, len);
			}
			*p++ = c1;
			sjis++;
			len--;
			continue;
		}
		l = pg_encoding_verifymbchar(PG_SJIS, (const char *) sjis, len);
		if (l < 0)
		{
			if (noError)
				break;
			report_invalid_encoding(PG_SJIS,
									(const char *) sjis, len);
		}
		if (c1 >= 0xa1 && c1 <= 0xdf)
		{
			/* JIS X0201 (1 byte kana) */
			*p++ = SS2;
			*p++ = c1;
		}
		else
		{
			/*
			 * JIS X0208, X0212, user defined extended characters
			 */
			c2 = sjis[1];
			k = (c1 << 8) + c2;
			if (k >= 0xed40 && k < 0xf040)
			{
				/* NEC selection IBM kanji */
				for (i = 0;; i++)
				{
					k2 = ibmkanji[i].nec;
					if (k2 == 0xffff)
						break;
					if (k2 == k)
					{
						k = ibmkanji[i].sjis;
						c1 = (k >> 8) & 0xff;
						c2 = k & 0xff;
					}
				}
			}

			if (k < 0xeb3f)
			{
				/* JIS X0208 */
				*p++ = ((c1 & 0x3f) << 1) + 0x9f + (c2 > 0x9e);
				*p++ = c2 + ((c2 > 0x9e) ? 2 : 0x60) + (c2 < 0x80);
			}
			else if ((k >= 0xeb40 && k < 0xf040) || (k >= 0xfc4c && k <= 0xfcfc))
			{
				/* NEC selection IBM kanji - Other undecided justice */
				*p++ = PGEUCALTCODE >> 8;
				*p++ = PGEUCALTCODE & 0xff;
			}
			else if (k >= 0xf040 && k < 0xf540)
			{
				/*
				 * UDC1 mapping to X0208 85 ku - 94 ku JIS code 0x7521 -
				 * 0x7e7e EUC 0xf5a1 - 0xfefe
				 */
				c1 -= 0x6f;
				*p++ = ((c1 & 0x3f) << 1) + 0xf3 + (c2 > 0x9e);
				*p++ = c2 + ((c2 > 0x9e) ? 2 : 0x60) + (c2 < 0x80);
			}
			else if (k >= 0xf540 && k < 0xfa40)
			{
				/*
				 * UDC2 mapping to X0212 85 ku - 94 ku JIS code 0x7521 -
				 * 0x7e7e EUC 0x8ff5a1 - 0x8ffefe
				 */
				*p++ = SS3;
				c1 -= 0x74;
				*p++ = ((c1 & 0x3f) << 1) + 0xf3 + (c2 > 0x9e);
				*p++ = c2 + ((c2 > 0x9e) ? 2 : 0x60) + (c2 < 0x80);
			}
			else if (k >= 0xfa40)
			{
				/*
				 * mapping IBM kanji to X0208 and X0212
				 *
				 */
				for (i = 0;; i++)
				{
					k2 = ibmkanji[i].sjis;
					if (k2 == 0xffff)
						break;
					if (k2 == k)
					{
						k = ibmkanji[i].euc;
						if (k >= 0x8f0000)
						{
							*p++ = SS3;
							*p++ = 0x80 | ((k & 0xff00) >> 8);
							*p++ = 0x80 | (k & 0xff);
						}
						else
						{
							*p++ = 0x80 | (k >> 8);
							*p++ = 0x80 | (k & 0xff);
						}
					}
				}
			}
		}
		sjis += l;
		len -= l;
	}
	*p = '\0';

	return sjis - start;
}
