/*
 * conversion between client encoding and server internal encoding
 * (currently mule internal code (mic) is used)
 * Tatsuo Ishii
 * $Id: mbutils.c,v 1.1 1998/06/16 07:38:18 momjian Exp $
 */
#include <stdio.h>
#include <string.h>

#include "postgres.h"
#include "miscadmin.h"
#include "regex/pg_wchar.h"
#include "commands/variable.h"

static int client_encoding = MB;	/* defalut client encoding is set to
					   same as the server encoding */
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
 * LATIN1 ---> MIC
 */
static void latin12mic(unsigned char *l, unsigned char *p, int len)
{
  int c1;

  while (len-- > 0 && (c1 = *l++)) {
    if (c1 > 0x7f) {	/* Latin1? */
      *p++ = LC_ISO8859_1;
    }
    *p++ = c1;
  }
  *p = '\0';
}

/*
 * MIC ---> LATIN1
 */
static void mic2latin1(unsigned char *mic, unsigned char *p, int len)
{
  int c1;

  while (len > 0 && (c1 = *mic)) {
    len -= pg_mic_mblen(mic++);

    if (c1 == LC_ISO8859_1) {
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

typedef struct {
  int encoding;		/* encoding symbol value */
  char *name;		/* encoding name */
  int is_client_only;	/* 0: server/client bothg supported
			   1: client only */
  void (*to_mic)();	/* client encoding to MIC */
  void (*from_mic)();	/* MIC to client encoding */
} pg_encoding_conv_tbl;

static pg_encoding_conv_tbl conv_tbl[] = {
  {EUC_JP, "EUC_JP", 0, euc_jp2mic, mic2euc_jp},	/* EUC_JP */
  {EUC_CN, "EUC_CN", 0, euc_cn2mic, mic2euc_cn},	/* EUC_CN */
  {EUC_KR, "EUC_KR", 0, euc_kr2mic, mic2euc_kr},	/* EUC_KR */
  {EUC_TW, "EUC_TW", 0, euc_tw2mic, mic2euc_tw},	/* EUC_TW */
  {UNICODE, "UNICODE", 0, 0, 0},			/* UNICODE */
  {MULE_INTERNAL, "MULE_INTERNAL", 0, 0, 0},		/* MULE_INTERNAL */
  {LATIN1, "LATIN1", 0, latin12mic, mic2latin1},	/* ISO 8859 Latin 1 */
  {SJIS, "SJIS", 1, sjis2mic, mic2sjis},		/* SJIS */
  {-1, "", 0, 0, 0}					/* end mark */
};

/*
 * find encoding table entry by encoding
 */
static pg_encoding_conv_tbl *get_enc_ent(int encoding)
{
  pg_encoding_conv_tbl *p = conv_tbl;
  for(;p->encoding >= 0;p++) {
    if (p->encoding == encoding) {
      return(p);
    }
  }
  return(0);
}

void (*client_to_mic)();	/* something to MIC */
void (*client_from_mic)();	/* MIC to something */
void (*server_to_mic)();	/* something to MIC */
void (*server_from_mic)();	/* MIC to something */

/*
 * set the client encoding. if client/server encoding is
 * not supported, returns -1
 */
int pg_set_client_encoding(int encoding)
{
  client_encoding = encoding;

  if (client_encoding == MB) {	/* server == client? */
    client_to_mic = client_from_mic = 0;
    server_to_mic = server_from_mic = 0;
  } else if (MB == MULE_INTERNAL) {	/* server == MULE_INETRNAL? */
    client_to_mic = get_enc_ent(encoding)->to_mic;
    client_from_mic = get_enc_ent(encoding)->from_mic;
    server_to_mic = server_from_mic = 0;
    if (client_to_mic == 0 || client_from_mic == 0) {
      return(-1);
    }
  } else if (encoding == MULE_INTERNAL) {	/* client == MULE_INETRNAL? */
    client_to_mic = client_from_mic = 0;
    server_to_mic = get_enc_ent(MB)->to_mic;
    server_from_mic = get_enc_ent(MB)->from_mic;
    if (server_to_mic == 0 || server_from_mic == 0) {
      return(-1);
    }
  } else {
    client_to_mic = get_enc_ent(encoding)->to_mic;
    client_from_mic = get_enc_ent(encoding)->from_mic;
    server_to_mic = get_enc_ent(MB)->to_mic;
    server_from_mic = get_enc_ent(MB)->from_mic;
    if (client_to_mic == 0 || client_from_mic == 0) {
      return(-1);
    }
    if (server_to_mic == 0 || server_from_mic == 0) {
      return(-1);
    }
  }
  return(0);
}

/*
 * returns the current client encoding
 */
int pg_get_client_encoding()
{
  return(client_encoding);
}

/*
 * convert client encoding to server encoding
 */
unsigned char *pg_client_to_server(unsigned char *s, int len)
{
  static unsigned char b1[MAX_PARSE_BUFFER*4];	/* is this enough? */
  static unsigned char b2[MAX_PARSE_BUFFER*4];	/* is this enough? */
  unsigned char *p;

  if (client_to_mic) {
    (*client_to_mic)(s, b1, len);
    len = strlen(b1);
    p = b1;
  } else {
    p = s;
  }
  if (server_from_mic) {
    (*server_from_mic)(p, b2, len);
    p = b2;
  }
  return(p);
}

/*
 * convert server encoding to client encoding
 */
unsigned char *pg_server_to_client(unsigned char *s, int len)
{
  static unsigned char b1[MAX_PARSE_BUFFER*4];	/* is this enough? */
  static unsigned char b2[MAX_PARSE_BUFFER*4];	/* is this enough? */
  unsigned char *p;

  if (server_to_mic) {
    (*server_to_mic)(s, b1, len);
    len = strlen(b1);
    p = b1;
  } else {
    p = s;
  }
  if (client_from_mic) {
    (*client_from_mic)(p, b2, len);
    p = b2;
  }
  return(p);
}

/*
 * convert encoding char to encoding symbol value.
 * case is ignored.
 * if there's no valid encoding, returns -1
 */
int pg_char_to_encoding(const char *s)
{
  pg_encoding_conv_tbl *p = conv_tbl;

  for(;p->encoding >= 0;p++) {
    if (!strcasecmp(s, p->name)) {
      break;
    }
  }
  return(p->encoding);
}

/*
 * check to see if encoding name is valid
 */
int pg_valid_client_encoding(const char *name)
{
  return(pg_char_to_encoding(name));
}

/*
 * convert encoding symbol to encoding char.
 * if there's no valid encoding symbol, returns ""
 */
const char *pg_encoding_to_char(int encoding)
{
  pg_encoding_conv_tbl *p = get_enc_ent(encoding);

  if (!p) return("");
  return(p->name);
}

#ifdef MBUTILSDEBUG
#include <stdio.h>

main()
{
  unsigned char sbuf[2048],ebuf[2048];
  unsigned char *p = sbuf;

  int c;
  while ((c = getchar()) != EOF) {
    *p++ = c;
  }
  *p = '\0';

  /*
  mic2sjis(sbuf,ebuf,2048);
  */
  euc_jp2mic(sbuf,ebuf,2048);
  printf("%s",ebuf);
}
#endif
