/*
 * This file contains public functions for conversion between
 * client encoding and server internal encoding.
 * (currently mule internal code (mic) is used)
 * Tatsuo Ishii
 * $Id: mbutils.c,v 1.2 1998/07/26 04:31:04 scrappy Exp $ */

#include <stdio.h>
#include <string.h>

#include "mb/pg_wchar.h"

static client_encoding = -1;
static void (*client_to_mic)();	/* something to MIC */
static void (*client_from_mic)();	/* MIC to something */
static void (*server_to_mic)();	/* something to MIC */
static void (*server_from_mic)();	/* MIC to something */

/*
 * find encoding table entry by encoding
 */
static pg_encoding_conv_tbl *get_enc_ent(int encoding)
{
  pg_encoding_conv_tbl *p = pg_conv_tbl;
  for(;p->encoding >= 0;p++) {
    if (p->encoding == encoding) {
      return(p);
    }
  }
  return(0);
}

/*
 * set the client encoding. if client/server encoding is
 * not supported, returns -1
 */
int pg_set_client_encoding(int encoding)
{
  int current_server_encoding = GetDatabaseEncoding();

  client_encoding = encoding;

  if (client_encoding == current_server_encoding) {	/* server == client? */
    client_to_mic = client_from_mic = 0;
    server_to_mic = server_from_mic = 0;
  } else if (current_server_encoding == MULE_INTERNAL) {	/* server == MULE_INETRNAL? */
    client_to_mic = get_enc_ent(encoding)->to_mic;
    client_from_mic = get_enc_ent(encoding)->from_mic;
    server_to_mic = server_from_mic = 0;
    if (client_to_mic == 0 || client_from_mic == 0) {
      return(-1);
    }
  } else if (encoding == MULE_INTERNAL) {	/* client == MULE_INETRNAL? */
    client_to_mic = client_from_mic = 0;
    server_to_mic = get_enc_ent(current_server_encoding)->to_mic;
    server_from_mic = get_enc_ent(current_server_encoding)->from_mic;
    if (server_to_mic == 0 || server_from_mic == 0) {
      return(-1);
    }
  } else {
    client_to_mic = get_enc_ent(encoding)->to_mic;
    client_from_mic = get_enc_ent(encoding)->from_mic;
    server_to_mic = get_enc_ent(current_server_encoding)->to_mic;
    server_from_mic = get_enc_ent(current_server_encoding)->from_mic;
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
  if (client_encoding == -1) {
    /* this is the first time */
    client_encoding = GetDatabaseEncoding();
  }
  return(client_encoding);
}

/*
 * convert client encoding to server encoding. if server_encoding ==
 * client_encoding or no conversion function exists,
 * returns s. So be careful.
 */
unsigned char *pg_client_to_server(unsigned char *s, int len)
{
  static unsigned char b1[MAX_PARSE_BUFFER*4];	/* is this enough? */
  static unsigned char b2[MAX_PARSE_BUFFER*4];	/* is this enough? */
  unsigned char *p = s;

  if (client_encoding == GetDatabaseEncoding()) {
    return(p);
  }
  if (client_to_mic) {
    (*client_to_mic)(s, b1, len);
    len = strlen(b1);
    p = b1;
  }
  if (server_from_mic) {
    (*server_from_mic)(p, b2, len);
    p = b2;
  }
  return(p);
}

/*
 * convert server encoding to client encoding. if server_encoding ==
 * client_encoding or no conversion function exists,
 * returns s. So be careful.
 */
unsigned char *pg_server_to_client(unsigned char *s, int len)
{
  static unsigned char b1[MAX_PARSE_BUFFER*4];	/* is this enough? */
  static unsigned char b2[MAX_PARSE_BUFFER*4];	/* is this enough? */
  unsigned char *p = s;

  if (client_encoding == GetDatabaseEncoding()) {
    return(p);
  }
  if (server_to_mic) {
    (*server_to_mic)(s, b1, len);
    len = strlen(b1);
    p = b1;
  }
  if (client_from_mic) {
    (*client_from_mic)(p, b2, len);
    p = b2;
  }
  return(p);
}

/* convert a multi-byte string to a wchar */
void pg_mb2wchar(const unsigned char *from, pg_wchar *to)
{
  (*pg_wchar_table[GetDatabaseEncoding()].mb2wchar_with_len)(from,to,strlen(from));
}

/* convert a multi-byte string to a wchar with a limited length */
void pg_mb2wchar_with_len(const unsigned char *from, pg_wchar *to, int len)
{
  (*pg_wchar_table[GetDatabaseEncoding()].mb2wchar_with_len)(from,to,len);
}

/* returns the byte length of a multi-byte word */
int pg_mblen(const unsigned char *mbstr)
{
  return((*pg_wchar_table[GetDatabaseEncoding()].mblen)(mbstr));
}

/* returns the length (counted as a wchar) of a multi-byte string */
int pg_mbstrlen(const unsigned char *mbstr)
{
  int len = 0;
  while (*mbstr) {
    mbstr += pg_mblen(mbstr);
    len++;
  }
  return(len);
}

/* returns the length (counted as a wchar) of a multi-byte string 
   (not necessarily  NULL terminated) */
int pg_mbstrlen_with_len(const unsigned char *mbstr, int limit)
{
  int len = 0;
  int l;
  while (*mbstr && limit > 0) {
    l = pg_mblen(mbstr);
    limit -= l;
    mbstr += l;
    len++;
  }
  return(len);
}

/*
 * fuctions for utils/init
 */
static int DatabaseEncoding = MULTIBYTE;
void
SetDatabaseEncoding(int encoding)
{
  DatabaseEncoding = encoding;
}

int
GetDatabaseEncoding()
{
  return(DatabaseEncoding);
}

/* for builtin-function */
const char *
getdatabaseencoding()
{
  return(pg_encoding_to_char(DatabaseEncoding));
}

/* set and get template1 database encoding */
static int templateEncoding;
void SetTemplateEncoding(int encoding)
{
  templateEncoding = encoding;
}

int GetTemplateEncoding()
{
  return(templateEncoding);
}
