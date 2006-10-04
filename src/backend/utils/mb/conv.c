/*-------------------------------------------------------------------------
 *
 *	  Utility functions for conversion procs.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/mb/conv.c,v 1.61 2006/10/04 00:30:02 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "mb/pg_wchar.h"


/*
 * LATINn ---> MIC when the charset's local codes map directly to MIC
 *
 * l points to the source string of length len
 * p is the output area (must be large enough!)
 * lc is the mule character set id for the local encoding
 * encoding is the PG identifier for the local encoding
 */
void
latin2mic(const unsigned char *l, unsigned char *p, int len,
		  int lc, int encoding)
{
	int			c1;

	while (len > 0)
	{
		c1 = *l;
		if (c1 == 0)
			report_invalid_encoding(encoding, (const char *) l, len);
		if (IS_HIGHBIT_SET(c1))
			*p++ = lc;
		*p++ = c1;
		l++;
		len--;
	}
	*p = '\0';
}

/*
 * MIC ---> LATINn when the charset's local codes map directly to MIC
 *
 * mic points to the source string of length len
 * p is the output area (must be large enough!)
 * lc is the mule character set id for the local encoding
 * encoding is the PG identifier for the local encoding
 */
void
mic2latin(const unsigned char *mic, unsigned char *p, int len,
		  int lc, int encoding)
{
	int			c1;

	while (len > 0)
	{
		c1 = *mic;
		if (c1 == 0)
			report_invalid_encoding(PG_MULE_INTERNAL, (const char *) mic, len);
		if (!IS_HIGHBIT_SET(c1))
		{
			/* easy for ASCII */
			*p++ = c1;
			mic++;
			len--;
		}
		else
		{
			int			l = pg_mic_mblen(mic);

			if (len < l)
				report_invalid_encoding(PG_MULE_INTERNAL, (const char *) mic,
										len);
			if (l != 2 || c1 != lc || !IS_HIGHBIT_SET(mic[1]))
				report_untranslatable_char(PG_MULE_INTERNAL, encoding,
										   (const char *) mic, len);
			*p++ = mic[1];
			mic += 2;
			len -= 2;
		}
	}
	*p = '\0';
}


/*
 * ASCII ---> MIC
 *
 * While ordinarily SQL_ASCII encoding is forgiving of high-bit-set
 * characters, here we must take a hard line because we don't know
 * the appropriate MIC equivalent.
 */
void
pg_ascii2mic(const unsigned char *l, unsigned char *p, int len)
{
	int			c1;

	while (len > 0)
	{
		c1 = *l;
		if (c1 == 0 || IS_HIGHBIT_SET(c1))
			report_invalid_encoding(PG_SQL_ASCII, (const char *) l, len);
		*p++ = c1;
		l++;
		len--;
	}
	*p = '\0';
}

/*
 * MIC ---> ASCII
 */
void
pg_mic2ascii(const unsigned char *mic, unsigned char *p, int len)
{
	int			c1;

	while (len > 0)
	{
		c1 = *mic;
		if (c1 == 0 || IS_HIGHBIT_SET(c1))
			report_untranslatable_char(PG_MULE_INTERNAL, PG_SQL_ASCII,
									   (const char *) mic, len);
		*p++ = c1;
		mic++;
		len--;
	}
	*p = '\0';
}

/*
 * latin2mic_with_table: a generic single byte charset encoding
 * conversion from a local charset to the mule internal code.
 *
 * l points to the source string of length len
 * p is the output area (must be large enough!)
 * lc is the mule character set id for the local encoding
 * encoding is the PG identifier for the local encoding
 * tab holds conversion entries for the local charset
 * starting from 128 (0x80). each entry in the table
 * holds the corresponding code point for the mule internal code.
 */
void
latin2mic_with_table(const unsigned char *l,
					 unsigned char *p,
					 int len,
					 int lc,
					 int encoding,
					 const unsigned char *tab)
{
	unsigned char c1,
				c2;

	while (len > 0)
	{
		c1 = *l;
		if (c1 == 0)
			report_invalid_encoding(encoding, (const char *) l, len);
		if (!IS_HIGHBIT_SET(c1))
			*p++ = c1;
		else
		{
			c2 = tab[c1 - HIGHBIT];
			if (c2)
			{
				*p++ = lc;
				*p++ = c2;
			}
			else
				report_untranslatable_char(encoding, PG_MULE_INTERNAL,
										   (const char *) l, len);
		}
		l++;
		len--;
	}
	*p = '\0';
}

/*
 * mic2latin_with_table: a generic single byte charset encoding
 * conversion from the mule internal code to a local charset.
 *
 * mic points to the source string of length len
 * p is the output area (must be large enough!)
 * lc is the mule character set id for the local encoding
 * encoding is the PG identifier for the local encoding
 * tab holds conversion entries for the mule internal code's
 * second byte, starting from 128 (0x80). each entry in the table
 * holds the corresponding code point for the local charset.
 */
void
mic2latin_with_table(const unsigned char *mic,
					 unsigned char *p,
					 int len,
					 int lc,
					 int encoding,
					 const unsigned char *tab)
{
	unsigned char c1,
				c2;

	while (len > 0)
	{
		c1 = *mic;
		if (c1 == 0)
			report_invalid_encoding(PG_MULE_INTERNAL, (const char *) mic, len);
		if (!IS_HIGHBIT_SET(c1))
		{
			/* easy for ASCII */
			*p++ = c1;
			mic++;
			len--;
		}
		else
		{
			int			l = pg_mic_mblen(mic);

			if (len < l)
				report_invalid_encoding(PG_MULE_INTERNAL, (const char *) mic,
										len);
			if (l != 2 || c1 != lc || !IS_HIGHBIT_SET(mic[1]) ||
				(c2 = tab[mic[1] - HIGHBIT]) == 0)
			{
				report_untranslatable_char(PG_MULE_INTERNAL, encoding,
										   (const char *) mic, len);
				break;			/* keep compiler quiet */
			}
			*p++ = c2;
			mic += 2;
			len -= 2;
		}
	}
	*p = '\0';
}

/*
 * comparison routine for bsearch()
 * this routine is intended for UTF8 -> local code
 */
static int
compare1(const void *p1, const void *p2)
{
	unsigned int v1,
				v2;

	v1 = *(unsigned int *) p1;
	v2 = ((pg_utf_to_local *) p2)->utf;
	return (v1 > v2) ? 1 : ((v1 == v2) ? 0 : -1);
}

/*
 * comparison routine for bsearch()
 * this routine is intended for local code -> UTF8
 */
static int
compare2(const void *p1, const void *p2)
{
	unsigned int v1,
				v2;

	v1 = *(unsigned int *) p1;
	v2 = ((pg_local_to_utf *) p2)->code;
	return (v1 > v2) ? 1 : ((v1 == v2) ? 0 : -1);
}

/*
 * UTF8 ---> local code
 *
 * utf: input UTF8 string (need not be null-terminated).
 * iso: pointer to the output area (must be large enough!)
 * map: the conversion map.
 * size: the size of the conversion map.
 * encoding: the PG identifier for the local encoding.
 * len: length of input string.
 */
void
UtfToLocal(const unsigned char *utf, unsigned char *iso,
		   const pg_utf_to_local *map, int size, int encoding, int len)
{
	unsigned int iutf;
	int			l;
	pg_utf_to_local *p;

	for (; len > 0; len -= l)
	{
		/* "break" cases all represent errors */
		if (*utf == '\0')
			break;

		l = pg_utf_mblen(utf);

		if (len < l)
			break;

		if (!pg_utf8_islegal(utf, l))
			break;

		if (l == 1)
		{
			/* ASCII case is easy */
			*iso++ = *utf++;
			continue;
		}
		else if (l == 2)
		{
			iutf = *utf++ << 8;
			iutf |= *utf++;
		}
		else if (l == 3)
		{
			iutf = *utf++ << 16;
			iutf |= *utf++ << 8;
			iutf |= *utf++;
		}
		else if (l == 4)
		{
			iutf = *utf++ << 24;
			iutf |= *utf++ << 16;
			iutf |= *utf++ << 8;
			iutf |= *utf++;
		}

		p = bsearch(&iutf, map, size,
					sizeof(pg_utf_to_local), compare1);

		if (p == NULL)
			report_untranslatable_char(PG_UTF8, encoding,
									   (const char *) (utf - l), len);

		if (p->code & 0xff000000)
			*iso++ = p->code >> 24;
		if (p->code & 0x00ff0000)
			*iso++ = (p->code & 0x00ff0000) >> 16;
		if (p->code & 0x0000ff00)
			*iso++ = (p->code & 0x0000ff00) >> 8;
		if (p->code & 0x000000ff)
			*iso++ = p->code & 0x000000ff;
	}

	if (len > 0)
		report_invalid_encoding(PG_UTF8, (const char *) utf, len);

	*iso = '\0';
}

/*
 * local code ---> UTF8
 *
 * iso: input local string (need not be null-terminated).
 * utf: pointer to the output area (must be large enough!)
 * map: the conversion map.
 * size: the size of the conversion map.
 * encoding: the PG identifier for the local encoding.
 * len: length of input string.
 */
void
LocalToUtf(const unsigned char *iso, unsigned char *utf,
		   const pg_local_to_utf *map, int size, int encoding, int len)
{
	unsigned int iiso;
	int			l;
	pg_local_to_utf *p;

	if (!PG_VALID_ENCODING(encoding))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid encoding number: %d", encoding)));

	for (; len > 0; len -= l)
	{
		/* "break" cases all represent errors */
		if (*iso == '\0')
			break;

		if (!IS_HIGHBIT_SET(*iso))
		{
			/* ASCII case is easy */
			*utf++ = *iso++;
			l = 1;
			continue;
		}

		l = pg_encoding_verifymb(encoding, (const char *) iso, len);
		if (l < 0)
			break;

		if (l == 1)
			iiso = *iso++;
		else if (l == 2)
		{
			iiso = *iso++ << 8;
			iiso |= *iso++;
		}
		else if (l == 3)
		{
			iiso = *iso++ << 16;
			iiso |= *iso++ << 8;
			iiso |= *iso++;
		}
		else if (l == 4)
		{
			iiso = *iso++ << 24;
			iiso |= *iso++ << 16;
			iiso |= *iso++ << 8;
			iiso |= *iso++;
		}

		p = bsearch(&iiso, map, size,
					sizeof(pg_local_to_utf), compare2);
		if (p == NULL)
			report_untranslatable_char(encoding, PG_UTF8,
									   (const char *) (iso - l), len);

		if (p->utf & 0xff000000)
			*utf++ = p->utf >> 24;
		if (p->utf & 0x00ff0000)
			*utf++ = (p->utf & 0x00ff0000) >> 16;
		if (p->utf & 0x0000ff00)
			*utf++ = (p->utf & 0x0000ff00) >> 8;
		if (p->utf & 0x000000ff)
			*utf++ = p->utf & 0x000000ff;
	}

	if (len > 0)
		report_invalid_encoding(encoding, (const char *) iso, len);

	*utf = '\0';
}
