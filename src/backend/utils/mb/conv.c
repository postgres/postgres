/*
 * conversion between client encoding and server internal encoding
 * (currently mule internal code (mic) is used)
 * Tatsuo Ishii
 * $Id: conv.c,v 1.2 1998/08/24 01:13:59 momjian Exp $
 */
#include <stdio.h>
#include <string.h>

#include "mb/pg_wchar.h"

/*
 * convert bogus chars that cannot be represented in the current encoding
 * system.
 */
static void printBogusChar(unsigned char **mic, unsigned char **p)
{
  char strbuf[16];
  int l = pg_mic_mblen(*mic);

  *(*p)++ = '(';
  while (l--) {
    sprintf(strbuf,"%02x",*(*mic)++);
    *(*p)++ = strbuf[0];
    *(*p)++ = strbuf[1];
  }
  *(*p)++ = ')';
}

/*
 * SJIS ---> MIC
 */
static void sjis2mic(unsigned char *sjis, unsigned char *p, int len)
{
  int c1,c2;

  while (len > 0 && (c1 = *sjis++)) {
    if (c1 >= 0xa1 && c1 <= 0xdf) {	/* 1 byte kana? */
      len--;
      *p++ = LC_JISX0201K;
      *p++ = c1;
    } else if (c1 > 0x7f) {	/* kanji? */
      c2 = *sjis++;
      len -= 2;
      *p++ = LC_JISX0208;
      *p++ = ((c1 & 0x3f)<<1) + 0x9f + (c2 > 0x9e);
      *p++ = c2 + ((c2 > 0x9e)? 2 : 0x60) + (c2 < 0x80);
    } else {	/* should be ASCII */
      len--;
      *p++ = c1;
    }
  }
  *p = '\0';
}

/*
 * MIC ---> SJIS
 */
static void mic2sjis(unsigned char *mic, unsigned char *p, int len)
{
  int c1,c2;

  while (len > 0 && (c1 = *mic)) {
    len -= pg_mic_mblen(mic++);

    if (c1 == LC_JISX0201K) {
      *p++ = *mic++;
    } else if (c1 == LC_JISX0208) {
      c1 = *mic++;
      c2 = *mic++;
      *p++ = ((c1 - 0xa1)>>1) + ((c1 < 0xdf)? 0x81 : 0xc1);
      *p++ = c2 - ((c1 & 1)? ((c2 < 0xe0)? 0x61 : 0x60) : 2);
    } else if (c1 > 0x7f) {	/* cannot convert to SJIS! */
      mic--;
      printBogusChar(&mic, &p);
    } else {	/* should be ASCII */
      *p++ = c1;
    }
  }
  *p = '\0';
}

/*
 * EUC_JP ---> MIC
 */
static void euc_jp2mic(unsigned char *euc, unsigned char *p, int len)
{
  int c1;

  while (len > 0 && (c1 = *euc++)) {
    if (c1 == SS2) {	/* 1 byte kana? */
      len -= 2;
      *p++ = LC_JISX0201K;
      *p++ = *euc++;
    } else if (c1 == SS3) {	/* JIS X0212 kanji? */
      len -= 3;
      *p++ = LC_JISX0212;
      *p++ = *euc++;
      *p++ = *euc++;
    } else if (c1 & 0x80) {	/* kanji? */
      len -= 2;
      *p++ = LC_JISX0208;
      *p++ = c1;
      *p++ = *euc++;
    } else {	/* should be ASCII */
      len--;
      *p++ = c1;
    }
  }
  *p = '\0';
}

/*
 * MIC ---> EUC_JP
 */
static void mic2euc_jp(unsigned char *mic, unsigned char *p, int len)
{
  int c1;

  while (len > 0 && (c1 = *mic)) {
    len -= pg_mic_mblen(mic++);

    if (c1 == LC_JISX0201K) {
      *p++ = SS2;
      *p++ = *mic++;
    } else if (c1 == LC_JISX0212) {
      *p++ = SS3;
      *p++ = *mic++;
      *p++ = *mic++;
    } else if (c1 == LC_JISX0208) {
      *p++ = *mic++;
      *p++ = *mic++;
    } else if (c1 > 0x7f) {	/* cannot convert to EUC_JP! */
      mic--;
      printBogusChar(&mic, &p);
    } else {	/* should be ASCII */
      *p++ = c1;
    }
  }
  *p = '\0';
}

/*
 * EUC_KR ---> MIC
 */
static void euc_kr2mic(unsigned char *euc, unsigned char *p, int len)
{
  int c1;

  while (len > 0 && (c1 = *euc++)) {
    if (c1 & 0x80) {
      len -= 2;
      *p++ = LC_KS5601;
      *p++ = c1;
      *p++ = *euc++;
    } else {	/* should be ASCII */
      len--;
      *p++ = c1;
    }
  }
  *p = '\0';
}

/*
 * MIC ---> EUC_KR
 */
static void mic2euc_kr(unsigned char *mic, unsigned char *p, int len)
{
  int c1;

  while (len > 0 && (c1 = *mic)) {
    len -= pg_mic_mblen(mic++);

    if (c1 == LC_KS5601) {
      *p++ = *mic++;
      *p++ = *mic++;
    } else if (c1 > 0x7f) {	/* cannot convert to EUC_KR! */
      mic--;
      printBogusChar(&mic, &p);
    } else {	/* should be ASCII */
      *p++ = c1;
    }
  }
  *p = '\0';
}

/*
 * EUC_CN ---> MIC
 */
static void euc_cn2mic(unsigned char *euc, unsigned char *p, int len)
{
  int c1;

  while (len > 0 && (c1 = *euc++)) {
    if (c1 & 0x80) {
      len -= 2;
      *p++ = LC_GB2312_80;
      *p++ = c1;
      *p++ = *euc++;
    } else {	/* should be ASCII */
      len--;
      *p++ = c1;
    }
  }
  *p = '\0';
}

/*
 * MIC ---> EUC_CN
 */
static void mic2euc_cn(unsigned char *mic, unsigned char *p, int len)
{
  int c1;

  while (len > 0 && (c1 = *mic)) {
    len -= pg_mic_mblen(mic++);

    if (c1 == LC_GB2312_80) {
      *p++ = *mic++;
      *p++ = *mic++;
    } else if (c1 > 0x7f) {	/* cannot convert to EUC_CN! */
      mic--;
      printBogusChar(&mic, &p);
    } else {	/* should be ASCII */
      *p++ = c1;
    }
  }
  *p = '\0';
}

/*
 * EUC_TW ---> MIC
 */
static void euc_tw2mic(unsigned char *euc, unsigned char *p, int len)
{
  int c1;

  while (len > 0 && (c1 = *euc++)) {
    if (c1 == SS2) {
      len -= 4;
      c1 = *euc++;	/* plane No. */
      if (c1 == 0xa1) {
	*p++ = LC_CNS11643_1;
      } else if (c1 == 0xa2) {
	*p++ = LC_CNS11643_2;
      } else {
	*p++ = 0x9d;	/* LCPRV2 */
	*p++ = 0xa3 - c1 + LC_CNS11643_3;
      }
      *p++ = *euc++;
      *p++ = *euc++;
    } else if (c1 & 0x80) {	/* CNS11643-1 */
      len -= 2;
      *p++ = LC_CNS11643_1;
      *p++ = c1;
      *p++ = *euc++;      
    } else {	/* should be ASCII */
      len --;
      *p++ = c1;
    }
  }
  *p = '\0';
}

/*
 * MIC ---> EUC_TW
 */
static void mic2euc_tw(unsigned char *mic, unsigned char *p, int len)
{
  int c1;

  while (len > 0 && (c1 = *mic)) {
    len -= pg_mic_mblen(mic++);

    if (c1 == LC_CNS11643_1 || c1 == LC_CNS11643_2) {
      *p++ = *mic++;
      *p++ = *mic++;
    } else if (c1 == 0x9d) {	/* LCPRV2? */
      *p++ = SS2;
      *p++ = c1 - LC_CNS11643_3 + 0xa3;
      *p++ = *mic++;
      *p++ = *mic++;
    } else if (c1 > 0x7f) {	/* cannot convert to EUC_TW! */
      mic--;
      printBogusChar(&mic, &p);
    } else {	/* should be ASCII */
      *p++ = c1;
    }
  }
  *p = '\0';
}

/*
 * LATINn ---> MIC
 */
static void latin2mic(unsigned char *l, unsigned char *p, int len, int lc)
{
  int c1;

  while (len-- > 0 && (c1 = *l++)) {
    if (c1 > 0x7f) {	/* Latin1? */
      *p++ = lc;
    }
    *p++ = c1;
  }
  *p = '\0';
}

/*
 * MIC ---> LATINn
 */
static void mic2latin(unsigned char *mic, unsigned char *p, int len, int lc)
{
  int c1;

  while (len > 0 && (c1 = *mic)) {
    len -= pg_mic_mblen(mic++);

    if (c1 == lc) {
      *p++ = *mic++;
    } else if (c1 > 0x7f) {
      mic--;
      printBogusChar(&mic, &p);
    } else {      /* should be ASCII */
      *p++ = c1;
    }
  }
  *p = '\0';
}

static void latin12mic(unsigned char *l, unsigned char *p, int len)
{
  latin2mic(l, p, len, LC_ISO8859_1);
}
static void mic2latin1(unsigned char *mic, unsigned char *p, int len)
{
  mic2latin(mic, p, len, LC_ISO8859_1);
}
static void latin22mic(unsigned char *l, unsigned char *p, int len)
{
  latin2mic(l, p, len, LC_ISO8859_2);
}
static void mic2latin2(unsigned char *mic, unsigned char *p, int len)
{
  mic2latin(mic, p, len, LC_ISO8859_2);
}
static void latin32mic(unsigned char *l, unsigned char *p, int len)
{
  latin2mic(l, p, len, LC_ISO8859_3);
}
static void mic2latin3(unsigned char *mic, unsigned char *p, int len)
{
  mic2latin(mic, p, len, LC_ISO8859_3);
}
static void latin42mic(unsigned char *l, unsigned char *p, int len)
{
  latin2mic(l, p, len, LC_ISO8859_4);
}
static void mic2latin4(unsigned char *mic, unsigned char *p, int len)
{
  mic2latin(mic, p, len, LC_ISO8859_4);
}
static void latin52mic(unsigned char *l, unsigned char *p, int len)
{
  latin2mic(l, p, len, LC_ISO8859_5);
}
static void mic2latin5(unsigned char *mic, unsigned char *p, int len)
{
  mic2latin(mic, p, len, LC_ISO8859_5);
}

/*
 * ASCII ---> MIC
 */
static void ascii2mic(unsigned char *l, unsigned char *p, int len)
{
  int c1;

  while (len-- > 0 && (c1 = *l++)) {
    *p++ = (c1 & 0x7f);
  }
  *p = '\0';
}

/*
 * MIC ---> ASCII
 */
static void mic2ascii(unsigned char *mic, unsigned char *p, int len)
{
  int c1;

  while (len > 0 && (c1 = *mic)) {
    if (c1 > 0x7f) {
      printBogusChar(&mic, &p);
    } else {      /* should be ASCII */
      *p++ = c1;
    }
  }
  *p = '\0';
}

pg_encoding_conv_tbl pg_conv_tbl[] = {
  {SQL_ASCII, "SQL_ASCII", 0, ascii2mic, mic2ascii},	/* SQL/ACII */
  {EUC_JP, "EUC_JP", 0, euc_jp2mic, mic2euc_jp},	/* EUC_JP */
  {EUC_CN, "EUC_CN", 0, euc_cn2mic, mic2euc_cn},	/* EUC_CN */
  {EUC_KR, "EUC_KR", 0, euc_kr2mic, mic2euc_kr},	/* EUC_KR */
  {EUC_TW, "EUC_TW", 0, euc_tw2mic, mic2euc_tw},	/* EUC_TW */
  {UNICODE, "UNICODE", 0, 0, 0},			/* UNICODE */
  {MULE_INTERNAL, "MULE_INTERNAL", 0, 0, 0},		/* MULE_INTERNAL */
  {LATIN1, "LATIN1", 0, latin12mic, mic2latin1},	/* ISO 8859 Latin 1 */
  {LATIN2, "LATIN2", 0, latin22mic, mic2latin2},	/* ISO 8859 Latin 2 */
  {LATIN3, "LATIN3", 0, latin32mic, mic2latin3},	/* ISO 8859 Latin 3 */
  {LATIN4, "LATIN4", 0, latin42mic, mic2latin4},	/* ISO 8859 Latin 4 */
  {LATIN5, "LATIN5", 0, latin52mic, mic2latin5},	/* ISO 8859 Latin 5 */
  {SJIS, "SJIS", 1, sjis2mic, mic2sjis},		/* SJIS */
  {-1, "", 0, 0, 0}					/* end mark */
};
