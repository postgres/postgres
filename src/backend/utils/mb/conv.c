/*
 * conversion between client encoding and server internal encoding
 * (currently mule internal code (mic) is used)
 * Tatsuo Ishii
 * $Id: conv.c,v 1.7 1999/04/25 18:09:54 tgl Exp $
 */
#include <stdio.h>
#include <string.h>

#include "mb/pg_wchar.h"

/*
 * convert bogus chars that cannot be represented in the current encoding
 * system.
 */
static void
printBogusChar(unsigned char **mic, unsigned char **p)
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

/*
 * SJIS ---> MIC
 */
static void
sjis2mic(unsigned char *sjis, unsigned char *p, int len)
{
	int			c1,
				c2;

	while (len > 0 && (c1 = *sjis++))
	{
		if (c1 >= 0xa1 && c1 <= 0xdf)
		{						/* 1 byte kana? */
			len--;
			*p++ = LC_JISX0201K;
			*p++ = c1;
		}
		else if (c1 > 0x7f)
		{						/* kanji? */
			c2 = *sjis++;
			len -= 2;
			*p++ = LC_JISX0208;
			*p++ = ((c1 & 0x3f) << 1) + 0x9f + (c2 > 0x9e);
			*p++ = c2 + ((c2 > 0x9e) ? 2 : 0x60) + (c2 < 0x80);
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
 * MIC ---> SJIS
 */
static void
mic2sjis(unsigned char *mic, unsigned char *p, int len)
{
	int			c1,
				c2;

	while (len > 0 && (c1 = *mic))
	{
		len -= pg_mic_mblen(mic++);

		if (c1 == LC_JISX0201K)
			*p++ = *mic++;
		else if (c1 == LC_JISX0208)
		{
			c1 = *mic++;
			c2 = *mic++;
			*p++ = ((c1 - 0xa1) >> 1) + ((c1 < 0xdf) ? 0x81 : 0xc1);
			*p++ = c2 - ((c1 & 1) ? ((c2 < 0xe0) ? 0x61 : 0x60) : 2);
		}
		else if (c1 > 0x7f)
		{						/* cannot convert to SJIS! */
			mic--;
			printBogusChar(&mic, &p);
		}
		else
		{						/* should be ASCII */
			*p++ = c1;
		}
	}
	*p = '\0';
}

/*
 * EUC_JP ---> MIC
 */
static void
euc_jp2mic(unsigned char *euc, unsigned char *p, int len)
{
	int			c1;

	while (len > 0 && (c1 = *euc++))
	{
		if (c1 == SS2)
		{						/* 1 byte kana? */
			len -= 2;
			*p++ = LC_JISX0201K;
			*p++ = *euc++;
		}
		else if (c1 == SS3)
		{						/* JIS X0212 kanji? */
			len -= 3;
			*p++ = LC_JISX0212;
			*p++ = *euc++;
			*p++ = *euc++;
		}
		else if (c1 & 0x80)
		{						/* kanji? */
			len -= 2;
			*p++ = LC_JISX0208;
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
 * MIC ---> EUC_JP
 */
static void
mic2euc_jp(unsigned char *mic, unsigned char *p, int len)
{
	int			c1;

	while (len > 0 && (c1 = *mic))
	{
		len -= pg_mic_mblen(mic++);

		if (c1 == LC_JISX0201K)
		{
			*p++ = SS2;
			*p++ = *mic++;
		}
		else if (c1 == LC_JISX0212)
		{
			*p++ = SS3;
			*p++ = *mic++;
			*p++ = *mic++;
		}
		else if (c1 == LC_JISX0208)
		{
			*p++ = *mic++;
			*p++ = *mic++;
		}
		else if (c1 > 0x7f)
		{						/* cannot convert to EUC_JP! */
			mic--;
			printBogusChar(&mic, &p);
		}
		else
		{						/* should be ASCII */
			*p++ = c1;
		}
	}
	*p = '\0';
}

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
			printBogusChar(&mic, &p);
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
			printBogusChar(&mic, &p);
		}
		else
		{						/* should be ASCII */
			*p++ = c1;
		}
	}
	*p = '\0';
}

/*
 * EUC_TW ---> MIC
 */
static void
euc_tw2mic(unsigned char *euc, unsigned char *p, int len)
{
	int			c1;

	while (len > 0 && (c1 = *euc++))
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

	while (len > 0 && (c1 = *mic))
	{
		len -= pg_mic_mblen(mic++);

		if (c1 == LC_CNS11643_1 || c1 == LC_CNS11643_2)
		{
			*p++ = *mic++;
			*p++ = *mic++;
		}
		else if (c1 == 0x9d)
		{						/* LCPRV2? */
			*p++ = SS2;
			*p++ = c1 - LC_CNS11643_3 + 0xa3;
			*p++ = *mic++;
			*p++ = *mic++;
		}
		else if (c1 > 0x7f)
		{						/* cannot convert to EUC_TW! */
			mic--;
			printBogusChar(&mic, &p);
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
  unsigned short big5buf, cnsBuf;
  unsigned char lc;
  char bogusBuf[2];
  int i;

  while (len > 0 && (c1 = *big5++))
    {
      if (c1 <= 0x007fU) {	/* ASCII */
	len--;
	*p++ = c1;
      } else {
	len  -= 2;
	big5buf = c1 << 8;
	c1 = *big5++;
	big5buf |= c1;
	cnsBuf = BIG5toCNS(big5buf, &lc);
	if (lc != 0) {
	  if (lc == LC_CNS11643_3 || lc == LC_CNS11643_4) {
	    *p++ = 0x9d;	/* LCPRV2 */
	  }
	  *p++ = lc;	/* Plane No. */
	  *p++ = (cnsBuf >> 8) & 0x00ff;
	  *p++ = cnsBuf & 0x00ff;
	} else {	/* cannot convert */
	  big5 -= 2;
	  *p++ = '(';
	  for (i=0;i<2;i++) {
	    sprintf(bogusBuf,"%02x",*big5++);
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
  int l;
  unsigned short			c1;
  unsigned short big5buf, cnsBuf;

  while (len > 0 && (c1 = *mic))
    {
      l = pg_mic_mblen(mic++);
      len -= l;

      /* 0x9d means LCPRV2 */
      if (c1 == LC_CNS11643_1 || c1 == LC_CNS11643_2 || c1 == 0x9d)
	{
	  if (c1 == 0x9d) {
	    c1 = *mic++;	/* get plane no. */
	  }
	  cnsBuf = (*mic++)<<8;
	  cnsBuf |= (*mic++) & 0x00ff;
	  big5buf = CNStoBIG5(cnsBuf, c1);
	  if (big5buf == 0) {	/* cannot convert to Big5! */
	    mic -= l;
	    printBogusChar(&mic, &p);
	  } else {
	    *p++ = (big5buf >> 8) & 0x00ff;
	    *p++ = big5buf & 0x00ff;
	  }
	}
      else if (c1 <= 0x7f) /* ASCII */
	{
	  *p++ = c1;
	} else {			/* cannot convert to Big5! */
	  mic--;
	  printBogusChar(&mic, &p);
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
		{						/* Latin1? */
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
			printBogusChar(&mic, &p);
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
#ifdef NOT_USED
static void
latin52mic(unsigned char *l, unsigned char *p, int len)
{
	latin2mic(l, p, len, LC_ISO8859_5);
}
static void
mic2latin5(unsigned char *mic, unsigned char *p, int len)
{
	mic2latin(mic, p, len, LC_ISO8859_5);
}
#endif

/*
 * ASCII ---> MIC
 */
static void
ascii2mic(unsigned char *l, unsigned char *p, int len)
{
	int			c1;

	while (len-- > 0 && (c1 = *l++))
		*p++ = (c1 & 0x7f);
	*p = '\0';
}

/*
 * MIC ---> ASCII
 */
static void
mic2ascii(unsigned char *mic, unsigned char *p, int len)
{
	int			c1;

	while (len-- > 0 && (c1 = *mic))
	{
		if (c1 > 0x7f)
			printBogusChar(&mic, &p);
		else
		{						/* should be ASCII */
			*p++ = c1;
		}
		mic++;
	}
	*p = '\0';
}

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

/* koi2mic: KOI8-R to Mule internal code */ 
static void
koi2mic(unsigned char *l, unsigned char *p, int len)
{
  latin2mic(l, p, len, LC_KOI8_R);
}

/* mic2koi: Mule internal code to KOI8-R */
static void
mic2koi(unsigned char *mic, unsigned char *p, int len)
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
		     unsigned char *p,	/* pointer to store mule internal code
					   (destination) */
		     int len,		/* length of l */
		     int lc,		/* leading character of p */
		     unsigned char *tab	/* code conversion table */
		     )
{
  unsigned char	c1,c2;
  
  while (len-- > 0 && (c1 = *l++)) {
    if (c1 < 128) {
      *p++ = c1;
    } else {
      c2 = tab[c1 - 128];
      if (c2) {
	*p++ = lc;
	*p++ = c2;
      } else {
	*p++ = ' ';	/* cannot convert */
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
		     unsigned char *mic,	/* mule internal code (source) */
		     unsigned char *p,		/* local code (destination) */
		     int len,			/* length of p */
		     int lc,			/* leading character */
		     unsigned char *tab		/* code conversion table */
		     )
{

  unsigned char	c1,c2;

  while (len-- > 0 && (c1 = *mic++)) {
    if (c1 < 128) {
      *p++ = c1;
    } else if (c1 == lc) {
      c1 = *mic++;
      len--;
      c2 = tab[c1 - 128];
      if (c2) {
	*p++ = c2;
      } else {
	*p++ = ' ';	/* cannot convert */
      }
    } else {
      *p++ = ' ';	/* bogus character */
    }
  }
  *p = '\0';
}

/* iso2mic: ISO-8859-5 to Mule internal code */ 
static void
iso2mic(unsigned char *l, unsigned char *p, int len)
{
  static char iso2koi[] = {
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
  static char koi2iso[] = {
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
win2mic(unsigned char *l, unsigned char *p, int len)
{
  static char win2koi[] = {
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
mic2win(unsigned char *mic, unsigned char *p, int len)
{
  static char koi2win[] = {
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
  static char alt2koi[] = {
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
  static char koi2alt[] = {
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

pg_encoding_conv_tbl pg_conv_tbl[] = {
	{SQL_ASCII, "SQL_ASCII", 0, ascii2mic, mic2ascii},	/* SQL/ACII */
	{EUC_JP, "EUC_JP", 0, euc_jp2mic, mic2euc_jp},		/* EUC_JP */
	{EUC_CN, "EUC_CN", 0, euc_cn2mic, mic2euc_cn},		/* EUC_CN */
	{EUC_KR, "EUC_KR", 0, euc_kr2mic, mic2euc_kr},		/* EUC_KR */
	{EUC_TW, "EUC_TW", 0, euc_tw2mic, mic2euc_tw},		/* EUC_TW */
	{UNICODE, "UNICODE", 0, 0, 0},		/* UNICODE */
	{MULE_INTERNAL, "MULE_INTERNAL", 0, 0, 0},	/* MULE_INTERNAL */
	{LATIN1, "LATIN1", 0, latin12mic, mic2latin1},		/* ISO 8859 Latin 1 */
	{LATIN2, "LATIN2", 0, latin22mic, mic2latin2},		/* ISO 8859 Latin 2 */
	{LATIN3, "LATIN3", 0, latin32mic, mic2latin3},		/* ISO 8859 Latin 3 */
	{LATIN4, "LATIN4", 0, latin42mic, mic2latin4},		/* ISO 8859 Latin 4 */
	{LATIN5, "LATIN5", 0, iso2mic, mic2iso},		/* ISO 8859 Latin 5 */
	{KOI8, "KOI8", 0, koi2mic, mic2koi},	/* KOI8-R */
	{WIN, "WIN", 0, win2mic, mic2win},	/* CP1251 */
	{ALT, "ALT", 0, alt2mic, mic2alt},	/* CP866 */
	{SJIS, "SJIS", 1, sjis2mic, mic2sjis},		/* SJIS */
	{BIG5, "BIG5", 1, big52mic, mic2big5},		/* Big5 */
	{-1, "", 0, 0, 0}			/* end mark */
};
