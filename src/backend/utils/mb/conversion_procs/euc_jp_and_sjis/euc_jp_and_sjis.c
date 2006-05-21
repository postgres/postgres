/*-------------------------------------------------------------------------
 *
 *	  EUC_JP, SJIS and MULE_INTERNAL
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/mb/conversion_procs/euc_jp_and_sjis/euc_jp_and_sjis.c,v 1.6.4.3 2006/05/21 20:06:44 tgl Exp $
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

#define ENCODING_GROWTH_RATE 4

PG_FUNCTION_INFO_V1(euc_jp_to_sjis);
PG_FUNCTION_INFO_V1(sjis_to_euc_jp);
PG_FUNCTION_INFO_V1(euc_jp_to_mic);
PG_FUNCTION_INFO_V1(mic_to_euc_jp);
PG_FUNCTION_INFO_V1(sjis_to_mic);
PG_FUNCTION_INFO_V1(mic_to_sjis);

extern Datum euc_jp_to_sjis(PG_FUNCTION_ARGS);
extern Datum sjis_to_euc_jp(PG_FUNCTION_ARGS);
extern Datum euc_jp_to_mic(PG_FUNCTION_ARGS);
extern Datum mic_to_euc_jp(PG_FUNCTION_ARGS);
extern Datum sjis_to_mic(PG_FUNCTION_ARGS);
extern Datum mic_to_sjis(PG_FUNCTION_ARGS);

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

static void sjis2mic(const unsigned char *sjis, unsigned char *p, int len);
static void mic2sjis(const unsigned char *mic, unsigned char *p, int len);
static void euc_jp2mic(const unsigned char *euc, unsigned char *p, int len);
static void mic2euc_jp(const unsigned char *mic, unsigned char *p, int len);

Datum
euc_jp_to_sjis(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	unsigned char *buf;

	Assert(PG_GETARG_INT32(0) == PG_EUC_JP);
	Assert(PG_GETARG_INT32(1) == PG_SJIS);
	Assert(len >= 0);

	buf = palloc(len * ENCODING_GROWTH_RATE);
	euc_jp2mic(src, buf, len);
	mic2sjis(buf, dest, strlen(buf));
	pfree(buf);

	PG_RETURN_VOID();
}

Datum
sjis_to_euc_jp(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	unsigned char *buf;

	Assert(PG_GETARG_INT32(0) == PG_SJIS);
	Assert(PG_GETARG_INT32(1) == PG_EUC_JP);
	Assert(len >= 0);

	buf = palloc(len * ENCODING_GROWTH_RATE);
	sjis2mic(src, buf, len);
	mic2euc_jp(buf, dest, strlen(buf));
	pfree(buf);

	PG_RETURN_VOID();
}

Datum
euc_jp_to_mic(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_EUC_JP);
	Assert(PG_GETARG_INT32(1) == PG_MULE_INTERNAL);
	Assert(len >= 0);

	euc_jp2mic(src, dest, len);

	PG_RETURN_VOID();
}

Datum
mic_to_euc_jp(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_MULE_INTERNAL);
	Assert(PG_GETARG_INT32(1) == PG_EUC_JP);
	Assert(len >= 0);

	mic2euc_jp(src, dest, len);

	PG_RETURN_VOID();
}

Datum
sjis_to_mic(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_SJIS);
	Assert(PG_GETARG_INT32(1) == PG_MULE_INTERNAL);
	Assert(len >= 0);

	sjis2mic(src, dest, len);

	PG_RETURN_VOID();
}

Datum
mic_to_sjis(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_MULE_INTERNAL);
	Assert(PG_GETARG_INT32(1) == PG_SJIS);
	Assert(len >= 0);

	mic2sjis(src, dest, len);

	PG_RETURN_VOID();
}

/*
 * SJIS ---> MIC
 */
static void
sjis2mic(const unsigned char *sjis, unsigned char *p, int len)
{
	int			c1,
				c2,
				i,
				k,
				k2;

	while (len > 0)
	{
		c1 = *sjis;
		if (c1 >= 0xa1 && c1 <= 0xdf)
		{
			/* JIS X0201 (1 byte kana) */
			*p++ = LC_JISX0201K;
			*p++ = c1;
			sjis++;
			len--;
		}
		else if (IS_HIGHBIT_SET(c1))
		{
			/*
			 * JIS X0208, X0212, user defined extended characters
			 */
			if (len < 2 || !ISSJISHEAD(c1) || !ISSJISTAIL(sjis[1]))
				report_invalid_encoding(PG_SJIS, (const char *) sjis, len);
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
				*p++ = LC_JISX0208;
				*p++ = ((c1 & 0x3f) << 1) + 0x9f + (c2 > 0x9e);
				*p++ = c2 + ((c2 > 0x9e) ? 2 : 0x60) + (c2 < 0x80);
			}
			else if ((k >= 0xeb40 && k < 0xf040) || (k >= 0xfc4c && k <= 0xfcfc))
			{
				/* NEC selection IBM kanji - Other undecided justice */
				*p++ = LC_JISX0208;
				*p++ = PGEUCALTCODE >> 8;
				*p++ = PGEUCALTCODE & 0xff;
			}
			else if (k >= 0xf040 && k < 0xf540)
			{
				/*
				 * UDC1 mapping to X0208 85 ku - 94 ku JIS code 0x7521 -
				 * 0x7e7e EUC 0xf5a1 - 0xfefe
				 */
				*p++ = LC_JISX0208;
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
				*p++ = LC_JISX0212;
				c1 -= 0x74;
				*p++ = ((c1 & 0x3f) << 1) + 0xf3 + (c2 > 0x9e);
				*p++ = c2 + ((c2 > 0x9e) ? 2 : 0x60) + (c2 < 0x80);
			}
			else if (k >= 0xfa40)
			{
				/*
				 * mapping IBM kanji to X0208 and X0212
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
							*p++ = LC_JISX0212;
							*p++ = 0x80 | ((k & 0xff00) >> 8);
							*p++ = 0x80 | (k & 0xff);
						}
						else
						{
							*p++ = LC_JISX0208;
							*p++ = 0x80 | (k >> 8);
							*p++ = 0x80 | (k & 0xff);
						}
					}
				}
			}
			sjis += 2;
			len -= 2;
		}
		else
		{						/* should be ASCII */
			if (c1 == 0)
				report_invalid_encoding(PG_SJIS, (const char *) sjis, len);
			*p++ = c1;
			sjis++;
			len--;
		}
	}
	*p = '\0';
}

/*
 * MIC ---> SJIS
 */
static void
mic2sjis(const unsigned char *mic, unsigned char *p, int len)
{
	int			c1,
				c2,
				k,
				l;

	while (len > 0)
	{
		c1 = *mic;
		if (!IS_HIGHBIT_SET(c1))
		{
			/* ASCII */
			if (c1 == 0)
				report_invalid_encoding(PG_MULE_INTERNAL,
										(const char *) mic, len);
			*p++ = c1;
			mic++;
			len--;
			continue;
		}
		l = pg_encoding_verifymb(PG_MULE_INTERNAL, (const char *) mic, len);
		if (l < 0)
			report_invalid_encoding(PG_MULE_INTERNAL,
									(const char *) mic, len);
		if (c1 == LC_JISX0201K)
			*p++ = mic[1];
		else if (c1 == LC_JISX0208)
		{
			c1 = mic[1];
			c2 = mic[2];
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
		else if (c1 == LC_JISX0212)
		{
			int			i,
						k2;

			c1 = mic[1];
			c2 = mic[2];
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
			report_untranslatable_char(PG_MULE_INTERNAL, PG_SJIS,
									   (const char *) mic, len);
		mic += l;
		len -= l;
	}
	*p = '\0';
}

/*
 * EUC_JP ---> MIC
 */
static void
euc_jp2mic(const unsigned char *euc, unsigned char *p, int len)
{
	int			c1;
	int			l;

	while (len > 0)
	{
		c1 = *euc;
		if (!IS_HIGHBIT_SET(c1))
		{
			/* ASCII */
			if (c1 == 0)
				report_invalid_encoding(PG_EUC_JP,
										(const char *) euc, len);
			*p++ = c1;
			euc++;
			len--;
			continue;
		}
		l = pg_encoding_verifymb(PG_EUC_JP, (const char *) euc, len);
		if (l < 0)
			report_invalid_encoding(PG_EUC_JP,
									(const char *) euc, len);
		if (c1 == SS2)
		{						/* 1 byte kana? */
			*p++ = LC_JISX0201K;
			*p++ = euc[1];
		}
		else if (c1 == SS3)
		{						/* JIS X0212 kanji? */
			*p++ = LC_JISX0212;
			*p++ = euc[1];
			*p++ = euc[2];
		}
		else
		{						/* kanji? */
			*p++ = LC_JISX0208;
			*p++ = c1;
			*p++ = euc[1];
		}
		euc += l;
		len -= l;
	}
	*p = '\0';
}

/*
 * MIC ---> EUC_JP
 */
static void
mic2euc_jp(const unsigned char *mic, unsigned char *p, int len)
{
	int			c1;
	int			l;

	while (len > 0)
	{
		c1 = *mic;
		if (!IS_HIGHBIT_SET(c1))
		{
			/* ASCII */
			if (c1 == 0)
				report_invalid_encoding(PG_MULE_INTERNAL,
										(const char *) mic, len);
			*p++ = c1;
			mic++;
			len--;
			continue;
		}
		l = pg_encoding_verifymb(PG_MULE_INTERNAL, (const char *) mic, len);
		if (l < 0)
			report_invalid_encoding(PG_MULE_INTERNAL,
									(const char *) mic, len);
		if (c1 == LC_JISX0201K)
		{
			*p++ = SS2;
			*p++ = mic[1];
		}
		else if (c1 == LC_JISX0212)
		{
			*p++ = SS3;
			*p++ = mic[1];
			*p++ = mic[2];
		}
		else if (c1 == LC_JISX0208)
		{
			*p++ = mic[1];
			*p++ = mic[2];
		}
		else
			report_untranslatable_char(PG_MULE_INTERNAL, PG_EUC_JP,
									   (const char *) mic, len);
		mic += l;
		len -= l;
	}
	*p = '\0';
}
