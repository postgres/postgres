/*
 * misc conversion functions between pg_wchar and other encodings.
 * Tatsuo Ishii
 * $Id: utils.c,v 1.2 1998/04/27 17:07:53 scrappy Exp $
 */
#include <regex/pg_wchar.h>
/*
 * convert EUC to pg_wchar (EUC process code)
 * caller should allocate enough space for "to"
 */
static void pg_euc2wchar(const unsigned char *from, pg_wchar *to)
{
  while (*from) {
    if (*from == SS2) {
      from++;
      *to = *from++;
    } else if (*from == SS3) {
      from++;
      *to = *from++ << 8;
      *to |= 0x3f & *from++;
    } else if (*from & 0x80) {
      *to = *from++ << 8;
      *to |= *from++;
    } else {
      *to = *from++;
    }
    to++;
  }
  *to = 0;
}

static void pg_eucjp2wchar(const unsigned char *from, pg_wchar *to)
{
  pg_euc2wchar(from,to);
}

static void pg_euckr2wchar(const unsigned char *from, pg_wchar *to)
{
  pg_euc2wchar(from,to);
}

static void pg_eucch2wchar(const unsigned char *from, pg_wchar *to)
{
  while (*from) {
    if (*from == SS2) {
      from++;
      *to = 0x3f00 & (*from++ << 8);
      *to = *from++;
    } else if (*from == SS3) {
      from++;
      *to = *from++ << 8;
      *to |= 0x3f & *from++;
    } else if (*from & 0x80) {
      *to = *from++ << 8;
      *to |= *from++;
    } else {
      *to = *from++;
    }
    to++;
  }
  *to = 0;
}

static void pg_euccn2wchar(const unsigned char *from, pg_wchar *to)
{
  while (*from) {
    if (*from == SS2) {
      from++;
      *to = *from++ << 16;
      *to |= *from++ << 8;
      *to |= *from++;
    } else if (*from == SS3) {
      from++;
      *to = *from++ << 8;
      *to |= 0x3f & *from++;
    } else if (*from & 0x80) {
      *to = *from++ << 8;
      *to |= *from++;
    } else {
      *to = *from++;
    }
    to++;
  }
  *to = 0;
}

/*
 * convert UTF-8 to pg_wchar (UCS-2)
 * caller should allocate enough space for "to"
 */
static void pg_utf2wchar(const unsigned char *from, pg_wchar *to)
{
  unsigned char c1,c2,c3;
  while (*from) {
    if ((*from & 0x80) == 0) {
      *to = *from++;
    } else if ((*from & 0xe0) == 0xc0) {
      c1 = *from++ & 0x1f;
      c2 = *from++ & 0x3f;
      *to = c1 << 6;
      *to |= c2;
    } else if ((*from & 0xe0) == 0xe0) {
      c1 = *from++ & 0x0f;
      c2 = *from++ & 0x3f;
      c3 = *from++ & 0x3f;
      *to = c1 << 12;
      *to |= c2 << 6;
      *to |= c3;
    }
    to++;
  }
  *to = 0;
}

/*
 * convert mule internal code to pg_wchar.
 * in this case pg_wchar consists of following 4 bytes:
 *
 * 0x00(unused)
 * 0x00(ASCII)|leading character (one of LC1, LC12, LC2 or LC22)
 * 0x00(ASCII,1 byte code)|other than 0x00(2 byte code)
 * the lowest byte of the code
 *
 * note that Type N (variable length byte encoding) cannot be represented by
 * this schema. sorry.
 * caller should allocate enough space for "to"
 */
static void pg_mule2wchar(const unsigned char *from, pg_wchar *to)
{
  while (*from) {
    if (IS_LC1(*from)) {
      *to = *from++ << 16;
      *to |= *from++;
    } else if (IS_LCPRV1(*from)) {
      from++;
      *to = *from++ << 16;
      *to |= *from++;
    } else if (IS_LC2(*from)) {
      *to = *from++ << 16;
      *to |= *from++ << 8;
      *to |= *from++;
    } else if (IS_LCPRV2(*from)) {
      from++;
      *to = *from++ << 16;
      *to |= *from++ << 8;
      *to |= *from++;
    } else {	/* assume ASCII */
      *to = *from++;
    }
    to++;
  }
  *to = 0;
}

/*
 * convert EUC to pg_wchar (EUC process code)
 * caller should allocate enough space for "to"
 * len: length of from.
 * "from" not necessarily null terminated.
 */
static void pg_euc2wchar_with_len(const unsigned char *from, pg_wchar *to, int len)
{
  while (*from && len > 0) {
    if (*from == SS2) {
      from++;
      len--;
      *to = 0xff & *from++;
      len--;
    } else if (*from == SS3) {
      from++;
      *to = *from++ << 8;
      *to |= 0x3f & *from++;
      len -= 3;
    } else if (*from & 0x80) {
      *to = *from++ << 8;
      *to |= *from++;
      len -= 2;
    } else {
      *to = *from++;
      len--;
    }
    to++;
  }
  *to = 0;
}

static void pg_eucjp2wchar_with_len
(const unsigned char *from, pg_wchar *to, int len)
{
  pg_euc2wchar_with_len(from,to,len);
}

static void pg_euckr2wchar_with_len
(const unsigned char *from, pg_wchar *to, int len)
{
  pg_euc2wchar_with_len(from,to,len);
}

static void pg_eucch2wchar_with_len
(const unsigned char *from, pg_wchar *to, int len)
{
  while (*from && len > 0) {
    if (*from == SS2) {
      from++;
      len--;
      *to = 0x3f00 & (*from++ << 8);
      *to = *from++;
      len -= 2;
    } else if (*from == SS3) {
      from++;
      *to = *from++ << 8;
      *to |= 0x3f & *from++;
      len -= 3;
    } else if (*from & 0x80) {
      *to = *from++ << 8;
      *to |= *from++;
      len -= 2;
    } else {
      *to = *from++;
      len--;
    }
    to++;
  }
  *to = 0;
}

static void pg_euccn2wchar_with_len
(const unsigned char *from, pg_wchar *to, int len)
{
  while (*from && len > 0) {
    if (*from == SS2) {
      from++;
      len--;
      *to = *from++ << 16;
      *to |= *from++ << 8;
      *to |= *from++;
      len -= 3;
    } else if (*from == SS3) {
      from++;
      *to = *from++ << 8;
      *to |= 0x3f & *from++;
      len -= 3;
    } else if (*from & 0x80) {
      *to = *from++ << 8;
      *to |= *from++;
      len -= 2;
    } else {
      *to = *from++;
      len--;
    }
    to++;
  }
  *to = 0;
}

/*
 * convert UTF-8 to pg_wchar (UCS-2)
 * caller should allocate enough space for "to"
 * len: length of from.
 * "from" not necessarily null terminated.
 */
static void pg_utf2wchar_with_len(const unsigned char *from, pg_wchar *to, int len)
{
  unsigned char c1,c2,c3;
  while (*from && len > 0) {
    if ((*from & 0x80) == 0) {
      *to = *from++;
      len--;
    } else if ((*from & 0xe0) == 0xc0) {
      c1 = *from++ & 0x1f;
      c2 = *from++ & 0x3f;
      len -= 2;
      *to = c1 << 6;
      *to |= c2;
    } else if ((*from & 0xe0) == 0xe0) {
      c1 = *from++ & 0x0f;
      c2 = *from++ & 0x3f;
      c3 = *from++ & 0x3f;
      len -= 3;
      *to = c1 << 12;
      *to |= c2 << 6;
      *to |= c3;
    }
    to++;
  }
  *to = 0;
}

/*
 * convert mule internal code to pg_wchar
 * caller should allocate enough space for "to"
 * len: length of from.
 * "from" not necessarily null terminated.
 */
static void pg_mule2wchar_with_len(const unsigned char *from, pg_wchar *to, int len)
{
  while (*from && len > 0) {
    if (IS_LC1(*from)) {
      *to = *from++ << 16;
      *to |= *from++;
      len -= 2;
    } else if (IS_LCPRV1(*from)) {
      from++;
      *to = *from++ << 16;
      *to |= *from++;
      len -= 3;
    } else if (IS_LC2(*from)) {
      *to = *from++ << 16;
      *to |= *from++ << 8;
      *to |= *from++;
      len -= 3;
    } else if (IS_LCPRV2(*from)) {
      from++;
      *to = *from++ << 16;
      *to |= *from++ << 8;
      *to |= *from++;
      len -= 4;
    } else {	/* assume ASCII */
      *to = (unsigned char)*from++;
      len--;
    }
    to++;
  }
  *to = 0;
}

static int pg_euc_mblen(const unsigned char *s)
{
  int len;

  if (*s == SS2) {
    len = 2;
  } else if (*s == SS3) {
    len = 3;
  } else if (*s & 0x80) {
    len = 2;
  } else {
    len = 1;
  }
  return(len);
}

static int pg_eucjp_mblen(const unsigned char *s)
{
  return(pg_euc_mblen(s));
}

static int pg_euckr_mblen(const unsigned char *s)
{
  return(pg_euc_mblen(s));
}

static int pg_eucch_mblen(const unsigned char *s)
{
  int len;

  if (*s == SS2) {
    len = 3;
  } else if (*s == SS3) {
    len = 3;
  } else if (*s & 0x80) {
    len = 2;
  } else {
    len = 1;
  }
  return(len);
}

static int pg_euccn_mblen(const unsigned char *s)
{
  int len;

  if (*s == SS2) {
    len = 4;
  } else if (*s == SS3) {
    len = 3;
  } else if (*s & 0x80) {
    len = 2;
  } else {
    len = 1;
  }
  return(len);
}

static int pg_utf_mblen(const unsigned char *s)
{
  int len = 1;

  if ((*s & 0x80) == 0) {
    len = 1;
  } else if ((*s & 0xe0) == 0xc0) {
    len = 2;
  } else if ((*s & 0xe0) == 0xe0) {
    len = 3;
  }
  return(len);
}

static int pg_mule_mblen(const unsigned char *s)
{
  int len;

  if (IS_LC1(*s)) {
    len = 2;
  } else if (IS_LCPRV1(*s)) {
    len = 3;
  } else if (IS_LC2(*s)) {
    len = 3;
  } else if (IS_LCPRV2(*s)) {
    len = 4;
  } else {	/* assume ASCII */
    len = 1;
  }
  return(len);
}

typedef struct {
  void	(*mb2wchar)();		/* convert a multi-byte string to a wchar */
  void	(*mb2wchar_with_len)();	/* convert a multi-byte string to a wchar 
				   with a limited length */
  int	(*mblen)();		/* returns the length of a multi-byte word */
} pg_wchar_tbl;

static pg_wchar_tbl pg_wchar_table[] = {
  {pg_eucjp2wchar, pg_eucjp2wchar_with_len, pg_eucjp_mblen},
  {pg_eucch2wchar, pg_eucch2wchar_with_len, pg_eucch_mblen},
  {pg_euckr2wchar, pg_euckr2wchar_with_len, pg_euckr_mblen},
  {pg_euccn2wchar, pg_euccn2wchar_with_len, pg_euccn_mblen},
  {pg_utf2wchar, pg_utf2wchar_with_len, pg_utf_mblen},
  {pg_mule2wchar, pg_mule2wchar_with_len, pg_mule_mblen}};

/* convert a multi-byte string to a wchar */
void pg_mb2wchar(const unsigned char *from, pg_wchar *to)
{
  (*pg_wchar_table[MB].mb2wchar)(from,to);
}

/* convert a multi-byte string to a wchar with a limited length */
void pg_mb2wchar_with_len(const unsigned char *from, pg_wchar *to, int len)
{
  (*pg_wchar_table[MB].mb2wchar_with_len)(from,to,len);
}

/* returns the byte length of a multi-byte word */
int pg_mblen(const unsigned char *mbstr)
{
  return((*pg_wchar_table[MB].mblen)(mbstr));
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
