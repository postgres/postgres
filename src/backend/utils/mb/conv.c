/*-------------------------------------------------------------------------
 *
 *	  Utility functions for conversion procs.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/mb/conv.c,v 1.41 2002/07/19 11:09:25 ishii Exp $
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
 * EUC_KR ---> MIC
 */
static void
euc_kr2mic(unsigned char *euc, unsigned char *p, int len)
{
	int			c1;

	while (len > 0 && (c1 = *euc++))
	{
		if (c1 & 0x80)
		{
			len -= 2;
			*p++ = LC_KS5601;
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
 * MIC ---> EUC_KR
 */
static void
mic2euc_kr(unsigned char *mic, unsigned char *p, int len)
{
	int			c1;

	while (len > 0 && (c1 = *mic))
	{
		len -= pg_mic_mblen(mic++);

		if (c1 == LC_KS5601)
		{
			*p++ = *mic++;
			*p++ = *mic++;
		}
		else if (c1 > 0x7f)
		{						/* cannot convert to EUC_KR! */
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
 * EUC_CN ---> MIC
 */
static void
euc_cn2mic(unsigned char *euc, unsigned char *p, int len)
{
	int			c1;

	while (len > 0 && (c1 = *euc++))
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

	while (len > 0 && (c1 = *mic))
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
		else if(c1 >= 0x81 && c1 <= 0xfe)
		{
			c2 = *gb18030++;
			
			if(c2 >= 0x30 && c2 <= 0x69){
				len -= 4;
				*p++ = c1;
				*p++ = c2;
				*p++ = *gb18030++;
				*p++ = *gb18030++;
				*p++ = *gb18030++;
			}
			else if ((c2 >=0x40 && c2 <= 0x7e) ||(c2 >=0x80 && c2 <= 0xfe)){
				len -= 2;
				*p++ = c1;
				*p++ = c2;
				*p++ = *gb18030++;
			}
			else{	/*throw the strange code*/
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

		if (c1 <= 0x7f) /*ASCII*/
		{					
			*p++ = c1;
		}
		else if (c1 >= 0x81 && c1 <= 0xfe)
		{		
			c2 = *mic++;
			
			if((c2 >= 0x40 && c2 <= 0x7e) || (c2 >= 0x80 && c2 <= 0xfe)){
				*p++ = c1;
				*p++ = c2;
			}
			else if(c2 >= 0x30 && c2 <= 0x39){
				*p++ = c1;
				*p++ = c2;
				*p++ = *mic++;
				*p++ = *mic++;
			}	
			else{
				mic--;
				pg_print_bogus_char(&mic, &p);
				mic--;
				pg_print_bogus_char(&mic, &p);
			}		
		}
		else{
			mic--;
			pg_print_bogus_char(&mic, &p);
		}
	}
	*p = '\0';
}

/*
 * LATINn ---> MIC
 */
static void
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
static void
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

static void
latin12mic(unsigned char *l, unsigned char *p, int len)
{
	latin2mic(l, p, len, LC_ISO8859_1);
}
static void
mic2latin1(unsigned char *mic, unsigned char *p, int len)
{
	mic2latin(mic, p, len, LC_ISO8859_1);
}
static void
latin22mic(unsigned char *l, unsigned char *p, int len)
{
	latin2mic(l, p, len, LC_ISO8859_2);
}
static void
mic2latin2(unsigned char *mic, unsigned char *p, int len)
{
	mic2latin(mic, p, len, LC_ISO8859_2);
}
static void
latin32mic(unsigned char *l, unsigned char *p, int len)
{
	latin2mic(l, p, len, LC_ISO8859_3);
}
static void
mic2latin3(unsigned char *mic, unsigned char *p, int len)
{
	mic2latin(mic, p, len, LC_ISO8859_3);
}
static void
latin42mic(unsigned char *l, unsigned char *p, int len)
{
	latin2mic(l, p, len, LC_ISO8859_4);
}
static void
mic2latin4(unsigned char *mic, unsigned char *p, int len)
{
	mic2latin(mic, p, len, LC_ISO8859_4);
}
#endif

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

#ifdef NOT_USED
/*
 * Cyrillic support
 * currently supported Cyrillic encodings:
 *
 * KOI8-R (this is the charset for the mule internal code
 *		for Cyrillic)
 * ISO-8859-5
 * Microsoft's CP1251(windows-1251)
 * Alternativny Variant (MS-DOS CP866)
 */

/* koi8r2mic: KOI8-R to Mule internal code */
static void
koi8r2mic(unsigned char *l, unsigned char *p, int len)
{
	latin2mic(l, p, len, LC_KOI8_R);
}

/* mic2koi8r: Mule internal code to KOI8-R */
static void
mic2koi8r(unsigned char *mic, unsigned char *p, int len)
{
	mic2latin(mic, p, len, LC_KOI8_R);
}

/*
 * latin2mic_with_table: a generic single byte charset encoding
 * conversion from a local charset to the mule internal code.
 * with a encoding conversion table.
 * the table is ordered according to the local charset,
 * starting from 128 (0x80). each entry in the table
 * holds the corresponding code point for the mule internal code.
 */
static void
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
static void
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

/* iso2mic: ISO-8859-5 to Mule internal code */
static void
iso2mic(unsigned char *l, unsigned char *p, int len)
{
	static unsigned char iso2koi[] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xe1, 0xe2, 0xf7, 0xe7, 0xe4, 0xe5, 0xf6, 0xfa,
		0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, 0xf0,
		0xf2, 0xf3, 0xf4, 0xf5, 0xe6, 0xe8, 0xe3, 0xfe,
		0xfb, 0xfd, 0xff, 0xf9, 0xf8, 0xfc, 0xe0, 0xf1,
		0xc1, 0xc2, 0xd7, 0xc7, 0xc4, 0xc5, 0xd6, 0xda,
		0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xd0,
		0xd2, 0xd3, 0xd4, 0xd5, 0xc6, 0xc8, 0xc3, 0xde,
		0xdb, 0xdd, 0xdf, 0xd9, 0xd8, 0xdc, 0xc0, 0xd1,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};

	latin2mic_with_table(l, p, len, LC_KOI8_R, iso2koi);
}

/* mic2iso: Mule internal code to ISO8859-5 */
static void
mic2iso(unsigned char *mic, unsigned char *p, int len)
{
	static unsigned char koi2iso[] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xee, 0xd0, 0xd1, 0xe6, 0xd4, 0xd5, 0xe4, 0xd3,
		0xe5, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde,
		0xdf, 0xef, 0xe0, 0xe1, 0xe2, 0xe3, 0xd6, 0xd2,
		0xec, 0xeb, 0xd7, 0xe8, 0xed, 0xe9, 0xe7, 0xea,
		0xce, 0xb0, 0xb1, 0xc6, 0xb4, 0xb5, 0xc4, 0xb3,
		0xc5, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe,
		0xbf, 0xcf, 0xc0, 0xc1, 0xc2, 0xc3, 0xb6, 0xb2,
		0xcc, 0xcb, 0xb7, 0xc8, 0xcd, 0xc9, 0xc7, 0xca
	};

	mic2latin_with_table(mic, p, len, LC_KOI8_R, koi2iso);
}

/* win2mic: CP1251 to Mule internal code */
static void
win12512mic(unsigned char *l, unsigned char *p, int len)
{
	static unsigned char win2koi[] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0xbd, 0x00, 0x00,
		0xb3, 0x00, 0xb4, 0x00, 0x00, 0x00, 0x00, 0xb7,
		0x00, 0x00, 0xb6, 0xa6, 0xad, 0x00, 0x00, 0x00,
		0xa3, 0x00, 0xa4, 0x00, 0x00, 0x00, 0x00, 0xa7,
		0xe1, 0xe2, 0xf7, 0xe7, 0xe4, 0xe5, 0xf6, 0xfa,
		0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, 0xf0,
		0xf2, 0xf3, 0xf4, 0xf5, 0xe6, 0xe8, 0xe3, 0xfe,
		0xfb, 0xfd, 0xff, 0xf9, 0xf8, 0xfc, 0xe0, 0xf1,
		0xc1, 0xc2, 0xd7, 0xc7, 0xc4, 0xc5, 0xd6, 0xda,
		0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xd0,
		0xd2, 0xd3, 0xd4, 0xd5, 0xc6, 0xc8, 0xc3, 0xde,
		0xdb, 0xdd, 0xdf, 0xd9, 0xd8, 0xdc, 0xc0, 0xd1
	};

	latin2mic_with_table(l, p, len, LC_KOI8_R, win2koi);
}

/* mic2win: Mule internal code to CP1251 */
static void
mic2win1251(unsigned char *mic, unsigned char *p, int len)
{
	static unsigned char koi2win[] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0xb8, 0xba, 0x00, 0xb3, 0xbf,
		0x00, 0x00, 0x00, 0x00, 0x00, 0xb4, 0x00, 0x00,
		0x00, 0x00, 0x00, 0xa8, 0xaa, 0x00, 0xb2, 0xaf,
		0x00, 0x00, 0x00, 0x00, 0x00, 0xa5, 0x00, 0x00,
		0xfe, 0xe0, 0xe1, 0xf6, 0xe4, 0xe5, 0xf4, 0xe3,
		0xf5, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee,
		0xef, 0xff, 0xf0, 0xf1, 0xf2, 0xf3, 0xe6, 0xe2,
		0xfc, 0xfb, 0xe7, 0xf8, 0xfd, 0xf9, 0xf7, 0xfa,
		0xde, 0xc0, 0xc1, 0xd6, 0xc4, 0xc5, 0xd4, 0xc3,
		0xd5, 0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce,
		0xcf, 0xdf, 0xd0, 0xd1, 0xd2, 0xd3, 0xc6, 0xc2,
		0xdc, 0xdb, 0xc7, 0xd8, 0xdd, 0xd9, 0xd7, 0xda
	};

	mic2latin_with_table(mic, p, len, LC_KOI8_R, koi2win);
}

/* alt2mic: CP866 to Mule internal code */
static void
alt2mic(unsigned char *l, unsigned char *p, int len)
{
	static unsigned char alt2koi[] = {
		0xe1, 0xe2, 0xf7, 0xe7, 0xe4, 0xe5, 0xf6, 0xfa,
		0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, 0xf0,
		0xf2, 0xf3, 0xf4, 0xf5, 0xe6, 0xe8, 0xe3, 0xfe,
		0xfb, 0xfd, 0xff, 0xf9, 0xf8, 0xfc, 0xe0, 0xf1,
		0xc1, 0xc2, 0xd7, 0xc7, 0xc4, 0xc5, 0xd6, 0xda,
		0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xd0,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0xbd, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xd2, 0xd3, 0xd4, 0xd5, 0xc6, 0xc8, 0xc3, 0xde,
		0xdb, 0xdd, 0xdf, 0xd9, 0xd8, 0xdc, 0xc0, 0xd1,
		0xb3, 0xa3, 0xb4, 0xa4, 0xb7, 0xa7, 0x00, 0x00,
		0xb6, 0xa6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};

	latin2mic_with_table(l, p, len, LC_KOI8_R, alt2koi);
}

/* mic2alt: Mule internal code to CP866 */
static void
mic2alt(unsigned char *mic, unsigned char *p, int len)
{
	static unsigned char koi2alt[] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0xf1, 0xf3, 0x00, 0xf9, 0xf5,
		0x00, 0x00, 0x00, 0x00, 0x00, 0xad, 0x00, 0x00,
		0x00, 0x00, 0x00, 0xf0, 0xf2, 0x00, 0xf8, 0xf4,
		0x00, 0x00, 0x00, 0x00, 0x00, 0xbd, 0x00, 0x00,
		0xee, 0xa0, 0xa1, 0xe6, 0xa4, 0xa5, 0xe4, 0xa3,
		0xe5, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae,
		0xaf, 0xef, 0xe0, 0xe1, 0xe2, 0xe3, 0xa6, 0xa2,
		0xec, 0xeb, 0xa7, 0xe8, 0xed, 0xe9, 0xe7, 0xea,
		0x9e, 0x80, 0x81, 0x96, 0x84, 0x85, 0x94, 0x83,
		0x95, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e,
		0x8f, 0x9f, 0x90, 0x91, 0x92, 0x93, 0x86, 0x82,
		0x9c, 0x9b, 0x87, 0x98, 0x9d, 0x99, 0x97, 0x9a
	};

	mic2latin_with_table(mic, p, len, LC_KOI8_R, koi2alt);
}

/*
 * end of Cyrillic support
 */


/*-----------------------------------------------------------------
 * WIN1250
 * Microsoft's CP1250(windows-1250)
 *-----------------------------------------------------------------*/
static void
win12502mic(unsigned char *l, unsigned char *p, int len)
{
	static unsigned char win1250_2_iso88592[] = {
		0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
		0x88, 0x89, 0xA9, 0x8B, 0xA6, 0xAB, 0xAE, 0xAC,
		0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
		0x98, 0x99, 0xB9, 0x9B, 0xB6, 0xBB, 0xBE, 0xBC,
		0xA0, 0xB7, 0xA2, 0xA3, 0xA4, 0xA1, 0x00, 0xA7,
		0xA8, 0x00, 0xAA, 0x00, 0x00, 0xAD, 0x00, 0xAF,
		0xB0, 0x00, 0xB2, 0xB3, 0xB4, 0x00, 0x00, 0x00,
		0xB8, 0xB1, 0xBA, 0x00, 0xA5, 0xBD, 0xB5, 0xBF,
		0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7,
		0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF,
		0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7,
		0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF,
		0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7,
		0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF,
		0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7,
		0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF
	};

	latin2mic_with_table(l, p, len, LC_ISO8859_2, win1250_2_iso88592);
}
static void
mic2win1250(unsigned char *mic, unsigned char *p, int len)
{
	static unsigned char iso88592_2_win1250[] = {
		0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
		0x88, 0x89, 0x00, 0x8B, 0x00, 0x00, 0x00, 0x00,
		0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
		0x98, 0x99, 0x00, 0x9B, 0x00, 0x00, 0x00, 0x00,
		0xA0, 0xA5, 0xA2, 0xA3, 0xA4, 0xBC, 0x8C, 0xA7,
		0xA8, 0x8A, 0xAA, 0x8D, 0x8F, 0xAD, 0x8E, 0xAF,
		0xB0, 0xB9, 0xB2, 0xB3, 0xB4, 0xBE, 0x9C, 0xA1,
		0xB8, 0x9A, 0xBA, 0x9D, 0x9F, 0xBD, 0x9E, 0xBF,
		0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7,
		0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF,
		0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7,
		0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF,
		0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7,
		0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF,
		0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7,
		0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF
	};

	mic2latin_with_table(mic, p, len, LC_ISO8859_2, iso88592_2_win1250);
}
#endif

/*
 * comparison routine for bsearch()
 * this routine is intended for UTF-8 -> local code
 */
static int
compare1(const void *p1, const void *p2)
{
	unsigned int v1,
				v2;

	v1 = *(unsigned int *) p1;
	v2 = ((pg_utf_to_local *) p2)->utf;
	return (v1 - v2);
}

/*
 * comparison routine for bsearch()
 * this routine is intended for local code -> UTF-8
 */
static int
compare2(const void *p1, const void *p2)
{
	unsigned int v1,
				v2;

	v1 = *(unsigned int *) p1;
	v2 = ((pg_local_to_utf *) p2)->code;
	return (v1 - v2);
}

/*
 * UTF-8 ---> local code
 *
 * utf: input UTF-8 string. Its length is limited by "len" parameter
 *		or a null terminater.
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
			elog(WARNING, "utf_to_local: could not convert UTF-8 (0x%04x). Ignored", iutf);
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

#ifdef NOT_USED
/*
 * Cyrillic charsets
 */

/*
 * UTF-8 --->KOI8-R
 */
static void
utf_to_KOI8R(unsigned char *utf, unsigned char *iso, int len)

{
	utf_to_local(utf, iso, ULmap_KOI8R, sizeof(ULmap_KOI8R) / sizeof(pg_utf_to_local), len);
}

/*
 * UTF-8 --->WIN1251
 */
static void
utf_to_WIN1251(unsigned char *utf, unsigned char *iso, int len)

{
	utf_to_local(utf, iso, ULmap_WIN1251, sizeof(ULmap_WIN1251) / sizeof(pg_utf_to_local), len);
}

/*
 * UTF-8 --->ALT
 */
static void
utf_to_ALT(unsigned char *utf, unsigned char *iso, int len)

{
	utf_to_local(utf, iso, ULmap_ALT, sizeof(ULmap_ALT) / sizeof(pg_utf_to_local), len);
}

#endif

/*
 * local code ---> UTF-8
 */
void
LocalToUtf(unsigned char *iso, unsigned char *utf,
			 pg_local_to_utf *map, int size, int encoding, int len)
{
	unsigned int iiso;
	int			l;
	pg_local_to_utf *p;

	if (!PG_VALID_ENCODING(encoding))
		elog(ERROR, "Invalid encoding number %d", encoding);

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
			elog(WARNING, "local_to_utf: could not convert (0x%04x) %s to UTF-8. Ignored",
				 iiso, (&pg_enc2name_tbl[encoding])->name);
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

#ifdef NOT_USED
/*
 * KOI8-R ---> UTF-8
 */
static void
KOI8R_to_utf(unsigned char *iso, unsigned char *utf, int len)
{
	local_to_utf(iso, utf, LUmapKOI8R, sizeof(LUmapKOI8R) / sizeof(pg_local_to_utf), PG_KOI8R, len);
}

/*
 * WIN1251 ---> UTF-8
 */
static void
WIN1251_to_utf(unsigned char *iso, unsigned char *utf, int len)
{
	local_to_utf(iso, utf, LUmapWIN1251, sizeof(LUmapWIN1251) / sizeof(pg_local_to_utf), PG_WIN1251, len);
}

/*
 * ALT ---> UTF-8
 */
static void
ALT_to_utf(unsigned char *iso, unsigned char *utf, int len)
{
	local_to_utf(iso, utf, LUmapALT, sizeof(LUmapALT) / sizeof(pg_local_to_utf), PG_ALT, len);
}

/*
 * UTF-8 ---> WIN1250
 */
static void
utf_to_win1250(unsigned char *utf, unsigned char *euc, int len)

{
		utf_to_local(utf, euc, ULmapWIN1250,
								 sizeof(ULmapWIN1250) / sizeof(pg_utf_to_local), len);
}

/*
 * WIN1250 ---> UTF-8
 */
static void
win1250_to_utf(unsigned char *euc, unsigned char *utf, int len)
{
		local_to_utf(euc, utf, LUmapWIN1250,
						  sizeof(LUmapWIN1250) / sizeof(pg_local_to_utf), PG_WIN1250, len);
}

/*
 * UTF-8 ---> WIN1256
 */
static void
utf_to_win1256(unsigned char *utf, unsigned char *euc, int len)

{
		utf_to_local(utf, euc, ULmapWIN1256,
								 sizeof(ULmapWIN1256) / sizeof(pg_utf_to_local), len);
}

/*
 * WIN1256 ---> UTF-8
 */
static void
win1256_to_utf(unsigned char *euc, unsigned char *utf, int len)
{
	local_to_utf(euc, utf, LUmapWIN1256,
			  sizeof(LUmapWIN1256) / sizeof(pg_local_to_utf), PG_WIN1256, len);
}

/*
 * UTF-8 ---> WIN874
 */
static void
utf_to_win874(unsigned char *utf, unsigned char *euc, int len)

{
	utf_to_local(utf, euc, ULmapWIN874,
				 sizeof(ULmapWIN874) / sizeof(pg_utf_to_local), len);
}

/*
 * WIN874 ---> UTF-8
 */
static void
win874_to_utf(unsigned char *euc, unsigned char *utf, int len)
{
	local_to_utf(euc, utf, LUmapWIN874,
			  sizeof(LUmapWIN874) / sizeof(pg_local_to_utf), PG_WIN874, len);
}

#endif
