/*-------------------------------------------------------------------------
 *
 *	  EUC_TW, BIG5 and MULE_INTERNAL
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/mb/conversion_procs/euc_tw_and_big5/euc_tw_and_big5.c,v 1.8 2004/08/29 04:12:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"

#define ENCODING_GROWTH_RATE 4

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

static void big52mic(unsigned char *big5, unsigned char *p, int len);
static void mic2big5(unsigned char *mic, unsigned char *p, int len);
static void euc_tw2mic(unsigned char *euc, unsigned char *p, int len);
static void mic2euc_tw(unsigned char *mic, unsigned char *p, int len);

Datum
euc_tw_to_big5(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	unsigned char *buf;

	Assert(PG_GETARG_INT32(0) == PG_EUC_TW);
	Assert(PG_GETARG_INT32(1) == PG_BIG5);
	Assert(len >= 0);

	buf = palloc(len * ENCODING_GROWTH_RATE);
	euc_tw2mic(src, buf, len);
	mic2big5(buf, dest, strlen(buf));
	pfree(buf);

	PG_RETURN_VOID();
}

Datum
big5_to_euc_tw(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	unsigned char *buf;

	Assert(PG_GETARG_INT32(0) == PG_BIG5);
	Assert(PG_GETARG_INT32(1) == PG_EUC_TW);
	Assert(len >= 0);

	buf = palloc(len * ENCODING_GROWTH_RATE);
	big52mic(src, buf, len);
	mic2euc_tw(buf, dest, strlen(buf));
	pfree(buf);

	PG_RETURN_VOID();
}

Datum
euc_tw_to_mic(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
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
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_MULE_INTERNAL);
	Assert(PG_GETARG_INT32(1) == PG_EUC_TW);
	Assert(len >= 0);

	mic2big5(src, dest, len);

	PG_RETURN_VOID();
}

Datum
big5_to_mic(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
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
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
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
euc_tw2mic(unsigned char *euc, unsigned char *p, int len)
{
	int			c1;

	while (len >= 0 && (c1 = *euc++))
	{
		if (c1 == SS2)
		{
			len -= 4;
			c1 = *euc++;		/* plane No. */
			if (c1 == 0xa1)
				*p++ = LC_CNS11643_1;
			else if (c1 == 0xa2)
				*p++ = LC_CNS11643_2;
			else
			{
				*p++ = 0x9d;	/* LCPRV2 */
				*p++ = 0xa3 - c1 + LC_CNS11643_3;
			}
			*p++ = *euc++;
			*p++ = *euc++;
		}
		else if (c1 & 0x80)
		{						/* CNS11643-1 */
			len -= 2;
			*p++ = LC_CNS11643_1;
			*p++ = c1;
			*p++ = *euc++;
		}
		else
		{						/* should be ASCII */
			len--;
			*p++ = c1;
		}
	}
	*p = '\0';
}

/*
 * MIC ---> EUC_TW
 */
static void
mic2euc_tw(unsigned char *mic, unsigned char *p, int len)
{
	int			c1;

	while (len >= 0 && (c1 = *mic))
	{
		len -= pg_mic_mblen(mic++);

		if (c1 == LC_CNS11643_1)
		{
			*p++ = *mic++;
			*p++ = *mic++;
		}
		else if (c1 == LC_CNS11643_2)
		{
			*p++ = SS2;
			*p++ = 0xa2;
			*p++ = *mic++;
			*p++ = *mic++;
		}
		else if (c1 == 0x9d)
		{						/* LCPRV2? */
			*p++ = SS2;
			*p++ = *mic++ - LC_CNS11643_3 + 0xa3;
			*p++ = *mic++;
			*p++ = *mic++;
		}
		else if (c1 > 0x7f)
		{						/* cannot convert to EUC_TW! */
			mic--;
			pg_print_bogus_char(&mic, &p);
		}
		else
		{						/* should be ASCII */
			*p++ = c1;
		}
	}
	*p = '\0';
}

/*
 * Big5 ---> MIC
 */
static void
big52mic(unsigned char *big5, unsigned char *p, int len)
{
	unsigned short c1;
	unsigned short big5buf,
				cnsBuf;
	unsigned char lc;
	char		bogusBuf[3];
	int			i;

	while (len >= 0 && (c1 = *big5++))
	{
		if (c1 <= 0x7fU)
		{						/* ASCII */
			len--;
			*p++ = c1;
		}
		else
		{
			len -= 2;
			big5buf = c1 << 8;
			c1 = *big5++;
			big5buf |= c1;
			cnsBuf = BIG5toCNS(big5buf, &lc);
			if (lc != 0)
			{
				if (lc == LC_CNS11643_3 || lc == LC_CNS11643_4)
				{
					*p++ = 0x9d;	/* LCPRV2 */
				}
				*p++ = lc;		/* Plane No. */
				*p++ = (cnsBuf >> 8) & 0x00ff;
				*p++ = cnsBuf & 0x00ff;
			}
			else
			{					/* cannot convert */
				big5 -= 2;
				*p++ = '(';
				for (i = 0; i < 2; i++)
				{
					sprintf(bogusBuf, "%02x", *big5++);
					*p++ = bogusBuf[0];
					*p++ = bogusBuf[1];
				}
				*p++ = ')';
			}
		}
	}
	*p = '\0';
}

/*
 * MIC ---> Big5
 */
static void
mic2big5(unsigned char *mic, unsigned char *p, int len)
{
	int			l;
	unsigned short c1;
	unsigned short big5buf,
				cnsBuf;

	while (len >= 0 && (c1 = *mic))
	{
		l = pg_mic_mblen(mic++);
		len -= l;

		/* 0x9d means LCPRV2 */
		if (c1 == LC_CNS11643_1 || c1 == LC_CNS11643_2 || c1 == 0x9d)
		{
			if (c1 == 0x9d)
			{
				c1 = *mic++;	/* get plane no. */
			}
			cnsBuf = (*mic++) << 8;
			cnsBuf |= (*mic++) & 0x00ff;
			big5buf = CNStoBIG5(cnsBuf, c1);
			if (big5buf == 0)
			{					/* cannot convert to Big5! */
				mic -= l;
				pg_print_bogus_char(&mic, &p);
			}
			else
			{
				*p++ = (big5buf >> 8) & 0x00ff;
				*p++ = big5buf & 0x00ff;
			}
		}
		else if (c1 <= 0x7f)	/* ASCII */
			*p++ = c1;
		else
		{						/* cannot convert to Big5! */
			mic--;
			pg_print_bogus_char(&mic, &p);
		}
	}
	*p = '\0';
}
