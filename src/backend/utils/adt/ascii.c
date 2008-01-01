/*-----------------------------------------------------------------------
 * ascii.c
 *	 The PostgreSQL routine for string to ascii conversion.
 *
 *	 Portions Copyright (c) 1999-2008, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/ascii.c,v 1.32 2008/01/01 19:45:52 momjian Exp $
 *
 *-----------------------------------------------------------------------
 */
#include "postgres.h"

#include "mb/pg_wchar.h"
#include "utils/ascii.h"

static void pg_to_ascii(unsigned char *src, unsigned char *src_end,
			unsigned char *dest, int enc);
static text *encode_to_ascii(text *data, int enc);


/* ----------
 * to_ascii
 * ----------
 */
static void
pg_to_ascii(unsigned char *src, unsigned char *src_end, unsigned char *dest, int enc)
{
	unsigned char *x;
	const unsigned char *ascii;
	int			range;

	/*
	 * relevant start for an encoding
	 */
#define RANGE_128	128
#define RANGE_160	160

	if (enc == PG_LATIN1)
	{
		/*
		 * ISO-8859-1 <range: 160 -- 255>
		 */
		ascii = (const unsigned char *) "  cL Y  \"Ca  -R     'u .,      ?AAAAAAACEEEEIIII NOOOOOxOUUUUYTBaaaaaaaceeeeiiii nooooo/ouuuuyty";
		range = RANGE_160;
	}
	else if (enc == PG_LATIN2)
	{
		/*
		 * ISO-8859-2 <range: 160 -- 255>
		 */
		ascii = (const unsigned char *) " A L LS \"SSTZ-ZZ a,l'ls ,sstz\"zzRAAAALCCCEEEEIIDDNNOOOOxRUUUUYTBraaaalccceeeeiiddnnoooo/ruuuuyt.";
		range = RANGE_160;
	}
	else if (enc == PG_LATIN9)
	{
		/*
		 * ISO-8859-15 <range: 160 -- 255>
		 */
		ascii = (const unsigned char *) "  cL YS sCa  -R     Zu .z   EeY?AAAAAAACEEEEIIII NOOOOOxOUUUUYTBaaaaaaaceeeeiiii nooooo/ouuuuyty";
		range = RANGE_160;
	}
	else if (enc == PG_WIN1250)
	{
		/*
		 * Window CP1250 <range: 128 -- 255>
		 */
		ascii = (const unsigned char *) "  ' \"    %S<STZZ `'\"\".--  s>stzz   L A  \"CS  -RZ  ,l'u .,as L\"lzRAAAALCCCEEEEIIDDNNOOOOxRUUUUYTBraaaalccceeeeiiddnnoooo/ruuuuyt ";
		range = RANGE_128;
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("encoding conversion from %s to ASCII not supported",
						pg_encoding_to_char(enc))));
		return;					/* keep compiler quiet */
	}

	/*
	 * Encode
	 */
	for (x = src; x < src_end; x++)
	{
		if (*x < 128)
			*dest++ = *x;
		else if (*x < range)
			*dest++ = ' ';		/* bogus 128 to 'range' */
		else
			*dest++ = ascii[*x - range];
	}
}

/* ----------
 * encode text
 *
 * The text datum is overwritten in-place, therefore this coding method
 * cannot support conversions that change the string length!
 * ----------
 */
static text *
encode_to_ascii(text *data, int enc)
{
	pg_to_ascii((unsigned char *) VARDATA(data),		/* src */
				(unsigned char *) (data) + VARSIZE(data),		/* src end */
				(unsigned char *) VARDATA(data),		/* dest */
				enc);			/* encoding */

	return data;
}

/* ----------
 * convert to ASCII - enc is set as 'name' arg.
 * ----------
 */
Datum
to_ascii_encname(PG_FUNCTION_ARGS)
{
	text	   *data = PG_GETARG_TEXT_P_COPY(0);
	char	   *encname = NameStr(*PG_GETARG_NAME(1));
	int			enc = pg_char_to_encoding(encname);

	if (enc < 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("%s is not a valid encoding name", encname)));

	PG_RETURN_TEXT_P(encode_to_ascii(data, enc));
}

/* ----------
 * convert to ASCII - enc is set as int4
 * ----------
 */
Datum
to_ascii_enc(PG_FUNCTION_ARGS)
{
	text	   *data = PG_GETARG_TEXT_P_COPY(0);
	int			enc = PG_GETARG_INT32(1);

	if (!PG_VALID_ENCODING(enc))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("%d is not a valid encoding code", enc)));

	PG_RETURN_TEXT_P(encode_to_ascii(data, enc));
}

/* ----------
 * convert to ASCII - current enc is DatabaseEncoding
 * ----------
 */
Datum
to_ascii_default(PG_FUNCTION_ARGS)
{
	text	   *data = PG_GETARG_TEXT_P_COPY(0);
	int			enc = GetDatabaseEncoding();

	PG_RETURN_TEXT_P(encode_to_ascii(data, enc));
}
