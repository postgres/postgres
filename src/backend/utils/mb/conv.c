/*-------------------------------------------------------------------------
 *
 *	  Utility functions for conversion procs.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/mb/conv.c,v 1.52 2005/03/07 04:30:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "mb/pg_wchar.h"

/*
 * convert bogus chars that cannot be represented in the current
 * encoding system.
 */
void
pg_print_bogus_char(unsigned char **mic, unsigned char **p)
{
	char		strbuf[16];
	int			l = pg_mic_mblen(*mic);

	*(*p)++ = '(';
	while (l--)
	{
		sprintf(strbuf, "%02x", *(*mic)++);
		*(*p)++ = strbuf[0];
		*(*p)++ = strbuf[1];
	}
	*(*p)++ = ')';
}

#ifdef NOT_USED

/*
 * GB18030 ---> MIC
 * Added by Bill Huang <bhuang@redhat.com>,<bill_huanghb@ybb.ne.jp>
 */
static void
gb180302mic(unsigned char *gb18030, unsigned char *p, int len)
{
	int			c1;
	int			c2;

	while (len > 0 && (c1 = *gb18030++))
	{
		if (c1 < 0x80)
		{						/* should be ASCII */
			len--;
			*p++ = c1;
		}
		else if (c1 >= 0x81 && c1 <= 0xfe)
		{
			c2 = *gb18030++;

			if (c2 >= 0x30 && c2 <= 0x69)
			{
				len -= 4;
				*p++ = c1;
				*p++ = c2;
				*p++ = *gb18030++;
				*p++ = *gb18030++;
				*p++ = *gb18030++;
			}
			else if ((c2 >= 0x40 && c2 <= 0x7e) || (c2 >= 0x80 && c2 <= 0xfe))
			{
				len -= 2;
				*p++ = c1;
				*p++ = c2;
				*p++ = *gb18030++;
			}
			else
			{					/* throw the strange code */
				len--;
			}
		}
	}
	*p = '\0';
}

/*
 * MIC ---> GB18030
 * Added by Bill Huang <bhuang@redhat.com>,<bill_huanghb@ybb.ne.jp>
 */
static void
mic2gb18030(unsigned char *mic, unsigned char *p, int len)
{
	int			c1;
	int			c2;

	while (len > 0 && (c1 = *mic))
	{
		len -= pg_mic_mblen(mic++);

		if (c1 <= 0x7f)			/* ASCII */
			*p++ = c1;
		else if (c1 >= 0x81 && c1 <= 0xfe)
		{
			c2 = *mic++;

			if ((c2 >= 0x40 && c2 <= 0x7e) || (c2 >= 0x80 && c2 <= 0xfe))
			{
				*p++ = c1;
				*p++ = c2;
			}
			else if (c2 >= 0x30 && c2 <= 0x39)
			{
				*p++ = c1;
				*p++ = c2;
				*p++ = *mic++;
				*p++ = *mic++;
			}
			else
			{
				mic--;
				pg_print_bogus_char(&mic, &p);
				mic--;
				pg_print_bogus_char(&mic, &p);
			}
		}
		else
		{
			mic--;
			pg_print_bogus_char(&mic, &p);
		}
	}
	*p = '\0';
}
#endif

/*
 * LATINn ---> MIC
 */
void
latin2mic(unsigned char *l, unsigned char *p, int len, int lc)
{
	int			c1;

	while (len-- > 0 && (c1 = *l++))
	{
		if (c1 > 0x7f)
		{						/* Latin? */
			*p++ = lc;
		}
		*p++ = c1;
	}
	*p = '\0';
}

/*
 * MIC ---> LATINn
 */
void
mic2latin(unsigned char *mic, unsigned char *p, int len, int lc)
{
	int			c1;

	while (len > 0 && (c1 = *mic))
	{
		len -= pg_mic_mblen(mic++);

		if (c1 == lc)
			*p++ = *mic++;
		else if (c1 > 0x7f)
		{
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
 * ASCII ---> MIC
 */
void
pg_ascii2mic(unsigned char *l, unsigned char *p, int len)
{
	int			c1;

	while (len-- > 0 && (c1 = *l++))
		*p++ = (c1 & 0x7f);
	*p = '\0';
}

/*
 * MIC ---> ASCII
 */
void
pg_mic2ascii(unsigned char *mic, unsigned char *p, int len)
{
	int			c1;

	while (len-- > 0 && (c1 = *mic))
	{
		if (c1 > 0x7f)
			pg_print_bogus_char(&mic, &p);
		else
		{						/* should be ASCII */
			*p++ = c1;
			mic++;
		}
	}
	*p = '\0';
}

/*
 * latin2mic_with_table: a generic single byte charset encoding
 * conversion from a local charset to the mule internal code.
 * with a encoding conversion table.
 * the table is ordered according to the local charset,
 * starting from 128 (0x80). each entry in the table
 * holds the corresponding code point for the mule internal code.
 */
void
latin2mic_with_table(
					 unsigned char *l,	/* local charset string (source) */
					 unsigned char *p,	/* pointer to store mule internal
										 * code (destination) */
					 int len,	/* length of l */
					 int lc,	/* leading character of p */
					 unsigned char *tab /* code conversion table */
)
{
	unsigned char c1,
				c2;

	while (len-- > 0 && (c1 = *l++))
	{
		if (c1 < 128)
			*p++ = c1;
		else
		{
			c2 = tab[c1 - 128];
			if (c2)
			{
				*p++ = lc;
				*p++ = c2;
			}
			else
			{
				*p++ = ' ';		/* cannot convert */
			}
		}
	}
	*p = '\0';
}

/*
 * mic2latin_with_table: a generic single byte charset encoding
 * conversion from the mule internal code to a local charset
 * with a encoding conversion table.
 * the table is ordered according to the second byte of the mule
 * internal code starting from 128 (0x80).
 * each entry in the table
 * holds the corresponding code point for the local code.
 */
void
mic2latin_with_table(
					 unsigned char *mic,		/* mule internal code
												 * (source) */
					 unsigned char *p,	/* local code (destination) */
					 int len,	/* length of p */
					 int lc,	/* leading character */
					 unsigned char *tab /* code conversion table */
)
{

	unsigned char c1,
				c2;

	while (len-- > 0 && (c1 = *mic++))
	{
		if (c1 < 128)
			*p++ = c1;
		else if (c1 == lc)
		{
			c1 = *mic++;
			len--;
			c2 = tab[c1 - 128];
			if (c2)
				*p++ = c2;
			else
			{
				*p++ = ' ';		/* cannot convert */
			}
		}
		else
		{
			*p++ = ' ';			/* bogus character */
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
 * utf: input UTF8 string. Its length is limited by "len" parameter
 *		or a null terminator.
 * iso: pointer to the output.
 * map: the conversion map.
 * size: the size of the conversion map.
 */
void
UtfToLocal(unsigned char *utf, unsigned char *iso,
		   pg_utf_to_local *map, int size, int len)
{
	unsigned int iutf;
	int			l;
	pg_utf_to_local *p;

	for (; len > 0 && *utf; len -= l)
	{
		l = pg_utf_mblen(utf);
		if (l == 1)
		{
			*iso++ = *utf++;
			continue;
		}
		else if (l == 2)
		{
			iutf = *utf++ << 8;
			iutf |= *utf++;
		}
		else
		{
			iutf = *utf++ << 16;
			iutf |= *utf++ << 8;
			iutf |= *utf++;
		}
		p = bsearch(&iutf, map, size,
					sizeof(pg_utf_to_local), compare1);
		if (p == NULL)
		{
			ereport(WARNING,
					(errcode(ERRCODE_UNTRANSLATABLE_CHARACTER),
				  errmsg("ignoring unconvertible UTF8 character 0x%04x",
						 iutf)));
			continue;
		}
		if (p->code & 0xff000000)
			*iso++ = p->code >> 24;
		if (p->code & 0x00ff0000)
			*iso++ = (p->code & 0x00ff0000) >> 16;
		if (p->code & 0x0000ff00)
			*iso++ = (p->code & 0x0000ff00) >> 8;
		if (p->code & 0x000000ff)
			*iso++ = p->code & 0x000000ff;
	}
	*iso = '\0';
}

/*
 * local code ---> UTF8
 */
void
LocalToUtf(unsigned char *iso, unsigned char *utf,
		   pg_local_to_utf *map, int size, int encoding, int len)
{
	unsigned int iiso;
	int			l;
	pg_local_to_utf *p;

	if (!PG_VALID_ENCODING(encoding))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid encoding number: %d", encoding)));

	for (; len > 0 && *iso; len -= l)
	{
		if (*iso < 0x80)
		{
			*utf++ = *iso++;
			l = 1;
			continue;
		}

		l = pg_encoding_mblen(encoding, iso);

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
		{
			ereport(WARNING,
					(errcode(ERRCODE_UNTRANSLATABLE_CHARACTER),
					 errmsg("ignoring unconvertible %s character 0x%04x",
							(&pg_enc2name_tbl[encoding])->name, iiso)));
			continue;
		}
		if (p->utf & 0xff000000)
			*utf++ = p->utf >> 24;
		if (p->utf & 0x00ff0000)
			*utf++ = (p->utf & 0x00ff0000) >> 16;
		if (p->utf & 0x0000ff00)
			*utf++ = (p->utf & 0x0000ff00) >> 8;
		if (p->utf & 0x000000ff)
			*utf++ = p->utf & 0x000000ff;
	}
	*utf = '\0';
}
