/*-------------------------------------------------------------------------
 *
 *	  EUC_CN and MULE_INTERNAL
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/mb/conversion_procs/euc_cn_and_mic/euc_cn_and_mic.c,v 1.7 2003/11/29 19:52:02 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"

PG_FUNCTION_INFO_V1(euc_cn_to_mic);
PG_FUNCTION_INFO_V1(mic_to_euc_cn);

extern Datum euc_cn_to_mic(PG_FUNCTION_ARGS);
extern Datum mic_to_euc_cn(PG_FUNCTION_ARGS);

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

static void euc_cn2mic(unsigned char *euc, unsigned char *p, int len);
static void mic2euc_cn(unsigned char *mic, unsigned char *p, int len);

Datum
euc_cn_to_mic(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_EUC_CN);
	Assert(PG_GETARG_INT32(1) == PG_MULE_INTERNAL);
	Assert(len >= 0);

	euc_cn2mic(src, dest, len);

	PG_RETURN_VOID();
}

Datum
mic_to_euc_cn(PG_FUNCTION_ARGS)
{
	unsigned char *src = PG_GETARG_CSTRING(2);
	unsigned char *dest = PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);

	Assert(PG_GETARG_INT32(0) == PG_MULE_INTERNAL);
	Assert(PG_GETARG_INT32(1) == PG_EUC_CN);
	Assert(len >= 0);

	mic2euc_cn(src, dest, len);

	PG_RETURN_VOID();
}

/*
 * EUC_CN ---> MIC
 */
static void
euc_cn2mic(unsigned char *euc, unsigned char *p, int len)
{
	int			c1;

	while (len >= 0 && (c1 = *euc++))
	{
		if (c1 & 0x80)
		{
			len -= 2;
			*p++ = LC_GB2312_80;
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
 * MIC ---> EUC_CN
 */
static void
mic2euc_cn(unsigned char *mic, unsigned char *p, int len)
{
	int			c1;

	while (len >= 0 && (c1 = *mic))
	{
		len -= pg_mic_mblen(mic++);

		if (c1 == LC_GB2312_80)
		{
			*p++ = *mic++;
			*p++ = *mic++;
		}
		else if (c1 > 0x7f)
		{						/* cannot convert to EUC_CN! */
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
