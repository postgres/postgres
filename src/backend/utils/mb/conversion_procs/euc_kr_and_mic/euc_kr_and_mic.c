/*-------------------------------------------------------------------------
 *
 *	  EUC_KR and MULE_INTERNAL
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/mb/conversion_procs/euc_kr_and_mic/euc_kr_and_mic.c,v 1.15 2006/05/30 22:12:14 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(euc_kr_to_mic);
PG_FUNCTION_INFO_V1(mic_to_euc_kr);

extern Datum euc_kr_to_mic(PG_FUNCTION_ARGS);
extern Datum mic_to_euc_kr(PG_FUNCTION_ARGS);

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

static void euc_kr2mic(const unsigned char *euc, unsigned char *p, int len);
static void mic2euc_kr(const unsigned char *mic, unsigned char *p, int len);

Datum
euc_kr_to_mic(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_EUC_KR);
	Assert(PG_GETARG_INT32(1) == PG_MULE_INTERNAL);
	Assert(len >= 0);

	euc_kr2mic(src, dest, len);

	PG_RETURN_VOID();
}

Datum
mic_to_euc_kr(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_MULE_INTERNAL);
	Assert(PG_GETARG_INT32(1) == PG_EUC_KR);
	Assert(len >= 0);

	mic2euc_kr(src, dest, len);

	PG_RETURN_VOID();
}

/*
 * EUC_KR ---> MIC
 */
static void
euc_kr2mic(const unsigned char *euc, unsigned char *p, int len)
{
	int			c1;
	int			l;

	while (len > 0)
	{
		c1 = *euc;
		if (IS_HIGHBIT_SET(c1))
		{
			l = pg_encoding_verifymb(PG_EUC_KR, (const char *) euc, len);
			if (l != 2)
				report_invalid_encoding(PG_EUC_KR,
										(const char *) euc, len);
			*p++ = LC_KS5601;
			*p++ = c1;
			*p++ = euc[1];
			euc += 2;
			len -= 2;
		}
		else
		{						/* should be ASCII */
			if (c1 == 0)
				report_invalid_encoding(PG_EUC_KR,
										(const char *) euc, len);
			*p++ = c1;
			euc++;
			len--;
		}
	}
	*p = '\0';
}

/*
 * MIC ---> EUC_KR
 */
static void
mic2euc_kr(const unsigned char *mic, unsigned char *p, int len)
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
		if (c1 == LC_KS5601)
		{
			*p++ = mic[1];
			*p++ = mic[2];
		}
		else
			report_untranslatable_char(PG_MULE_INTERNAL, PG_EUC_KR,
									   (const char *) mic, len);
		mic += l;
		len -= l;
	}
	*p = '\0';
}
