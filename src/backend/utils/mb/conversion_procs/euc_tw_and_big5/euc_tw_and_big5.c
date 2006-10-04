/*-------------------------------------------------------------------------
 *
 *	  EUC_TW, BIG5 and MULE_INTERNAL
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/mb/conversion_procs/euc_tw_and_big5/euc_tw_and_big5.c,v 1.16 2006/10/04 00:30:02 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"

#define ENCODING_GROWTH_RATE 4

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(euc_tw_to_big5);
PG_FUNCTION_INFO_V1(big5_to_euc_tw);
PG_FUNCTION_INFO_V1(euc_tw_to_mic);
PG_FUNCTION_INFO_V1(mic_to_euc_tw);
PG_FUNCTION_INFO_V1(big5_to_mic);
PG_FUNCTION_INFO_V1(mic_to_big5);

extern Datum euc_tw_to_big5(PG_FUNCTION_ARGS);
extern Datum big5_to_euc_tw(PG_FUNCTION_ARGS);
extern Datum euc_tw_to_mic(PG_FUNCTION_ARGS);
extern Datum mic_to_euc_tw(PG_FUNCTION_ARGS);
extern Datum big5_to_mic(PG_FUNCTION_ARGS);
extern Datum mic_to_big5(PG_FUNCTION_ARGS);

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

static void big52mic(const unsigned char *big5, unsigned char *p, int len);
static void mic2big5(const unsigned char *mic, unsigned char *p, int len);
static void euc_tw2mic(const unsigned char *euc, unsigned char *p, int len);
static void mic2euc_tw(const unsigned char *mic, unsigned char *p, int len);

Datum
euc_tw_to_big5(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	unsigned char *buf;

	Assert(PG_GETARG_INT32(0) == PG_EUC_TW);
	Assert(PG_GETARG_INT32(1) == PG_BIG5);
	Assert(len >= 0);

	buf = palloc(len * ENCODING_GROWTH_RATE);
	euc_tw2mic(src, buf, len);
	mic2big5(buf, dest, strlen((char *) buf));
	pfree(buf);

	PG_RETURN_VOID();
}

Datum
big5_to_euc_tw(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	unsigned char *buf;

	Assert(PG_GETARG_INT32(0) == PG_BIG5);
	Assert(PG_GETARG_INT32(1) == PG_EUC_TW);
	Assert(len >= 0);

	buf = palloc(len * ENCODING_GROWTH_RATE);
	big52mic(src, buf, len);
	mic2euc_tw(buf, dest, strlen((char *) buf));
	pfree(buf);

	PG_RETURN_VOID();
}

Datum
euc_tw_to_mic(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_EUC_TW);
	Assert(PG_GETARG_INT32(1) == PG_MULE_INTERNAL);
	Assert(len >= 0);

	euc_tw2mic(src, dest, len);

	PG_RETURN_VOID();
}

Datum
mic_to_euc_tw(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_MULE_INTERNAL);
	Assert(PG_GETARG_INT32(1) == PG_EUC_TW);
	Assert(len >= 0);

	mic2euc_tw(src, dest, len);

	PG_RETURN_VOID();
}

Datum
big5_to_mic(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_BIG5);
	Assert(PG_GETARG_INT32(1) == PG_MULE_INTERNAL);
	Assert(len >= 0);

	big52mic(src, dest, len);

	PG_RETURN_VOID();
}

Datum
mic_to_big5(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_MULE_INTERNAL);
	Assert(PG_GETARG_INT32(1) == PG_BIG5);
	Assert(len >= 0);

	mic2big5(src, dest, len);

	PG_RETURN_VOID();
}

/*
 * EUC_TW ---> MIC
 */
static void
euc_tw2mic(const unsigned char *euc, unsigned char *p, int len)
{
	int			c1;
	int			l;

	while (len > 0)
	{
		c1 = *euc;
		if (IS_HIGHBIT_SET(c1))
		{
			l = pg_encoding_verifymb(PG_EUC_TW, (const char *) euc, len);
			if (l < 0)
				report_invalid_encoding(PG_EUC_TW,
										(const char *) euc, len);
			if (c1 == SS2)
			{
				c1 = euc[1];	/* plane No. */
				if (c1 == 0xa1)
					*p++ = LC_CNS11643_1;
				else if (c1 == 0xa2)
					*p++ = LC_CNS11643_2;
				else
				{
					*p++ = 0x9d;	/* LCPRV2 */
					*p++ = c1 - 0xa3 + LC_CNS11643_3;
				}
				*p++ = euc[2];
				*p++ = euc[3];
			}
			else
			{					/* CNS11643-1 */
				*p++ = LC_CNS11643_1;
				*p++ = c1;
				*p++ = euc[1];
			}
			euc += l;
			len -= l;
		}
		else
		{						/* should be ASCII */
			if (c1 == 0)
				report_invalid_encoding(PG_EUC_TW,
										(const char *) euc, len);
			*p++ = c1;
			euc++;
			len--;
		}
	}
	*p = '\0';
}

/*
 * MIC ---> EUC_TW
 */
static void
mic2euc_tw(const unsigned char *mic, unsigned char *p, int len)
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
		if (c1 == LC_CNS11643_1)
		{
			*p++ = mic[1];
			*p++ = mic[2];
		}
		else if (c1 == LC_CNS11643_2)
		{
			*p++ = SS2;
			*p++ = 0xa2;
			*p++ = mic[1];
			*p++ = mic[2];
		}
		else if (c1 == 0x9d &&
				 mic[1] >= LC_CNS11643_3 && mic[1] <= LC_CNS11643_7)
		{						/* LCPRV2? */
			*p++ = SS2;
			*p++ = mic[1] - LC_CNS11643_3 + 0xa3;
			*p++ = mic[2];
			*p++ = mic[3];
		}
		else
			report_untranslatable_char(PG_MULE_INTERNAL, PG_EUC_TW,
									   (const char *) mic, len);
		mic += l;
		len -= l;
	}
	*p = '\0';
}

/*
 * Big5 ---> MIC
 */
static void
big52mic(const unsigned char *big5, unsigned char *p, int len)
{
	unsigned short c1;
	unsigned short big5buf,
				cnsBuf;
	unsigned char lc;
	int			l;

	while (len > 0)
	{
		c1 = *big5;
		if (!IS_HIGHBIT_SET(c1))
		{
			/* ASCII */
			if (c1 == 0)
				report_invalid_encoding(PG_BIG5,
										(const char *) big5, len);
			*p++ = c1;
			big5++;
			len--;
			continue;
		}
		l = pg_encoding_verifymb(PG_BIG5, (const char *) big5, len);
		if (l < 0)
			report_invalid_encoding(PG_BIG5,
									(const char *) big5, len);
		big5buf = (c1 << 8) | big5[1];
		cnsBuf = BIG5toCNS(big5buf, &lc);
		if (lc != 0)
		{
			if (lc == LC_CNS11643_3 || lc == LC_CNS11643_4)
			{
				*p++ = 0x9d;	/* LCPRV2 */
			}
			*p++ = lc;			/* Plane No. */
			*p++ = (cnsBuf >> 8) & 0x00ff;
			*p++ = cnsBuf & 0x00ff;
		}
		else
			report_untranslatable_char(PG_BIG5, PG_MULE_INTERNAL,
									   (const char *) big5, len);
		big5 += l;
		len -= l;
	}
	*p = '\0';
}

/*
 * MIC ---> Big5
 */
static void
mic2big5(const unsigned char *mic, unsigned char *p, int len)
{
	unsigned short c1;
	unsigned short big5buf,
				cnsBuf;
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
		/* 0x9d means LCPRV2 */
		if (c1 == LC_CNS11643_1 || c1 == LC_CNS11643_2 || c1 == 0x9d)
		{
			if (c1 == 0x9d)
			{
				c1 = mic[1];	/* get plane no. */
				cnsBuf = (mic[2] << 8) | mic[3];
			}
			else
			{
				cnsBuf = (mic[1] << 8) | mic[2];
			}
			big5buf = CNStoBIG5(cnsBuf, c1);
			if (big5buf == 0)
				report_untranslatable_char(PG_MULE_INTERNAL, PG_BIG5,
										   (const char *) mic, len);
			*p++ = (big5buf >> 8) & 0x00ff;
			*p++ = big5buf & 0x00ff;
		}
		else
			report_untranslatable_char(PG_MULE_INTERNAL, PG_BIG5,
									   (const char *) mic, len);
		mic += l;
		len -= l;
	}
	*p = '\0';
}
