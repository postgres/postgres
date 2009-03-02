/*
 * conversion functions between pg_wchar and multibyte streams.
 * Tatsuo Ishii
 * $Id: wchar.c,v 1.34.2.7 2009/03/02 21:19:23 tgl Exp $
 *
 * WIN1250 client encoding updated by Pavel Behal
 *
 */
/* can be used in either frontend or backend */
#ifdef FRONTEND
#include "postgres_fe.h"
#define Assert(condition)
#else
#include "postgres.h"
#endif

#include "mb/pg_wchar.h"


/*
 * conversion to pg_wchar is done by "table driven."
 * to add an encoding support, define mb2wchar_with_len(), mblen()
 * for the particular encoding. Note that if the encoding is only
 * supported in the client, you don't need to define
 * mb2wchar_with_len() function (SJIS is the case).
 */

/*
 * SQL/ASCII
 */
static int	pg_ascii2wchar_with_len
			(const unsigned char *from, pg_wchar *to, int len)
{
	int			cnt = 0;

	while (len > 0 && *from)
	{
		*to++ = *from++;
		len--;
		cnt++;
	}
	*to = 0;
	return (cnt);
}

static int
pg_ascii_mblen(const unsigned char *s)
{
	return (1);
}

/*
 * EUC
 */
static int	pg_euc2wchar_with_len
			(const unsigned char *from, pg_wchar *to, int len)
{
	int			cnt = 0;

	while (len > 0 && *from)
	{
		if (*from == SS2 && len >= 2)	/* JIS X 0201 (so called "1 byte KANA") */
		{
			from++;
			*to = (SS2 << 8) | *from++;
			len -= 2;
		}
		else if (*from == SS3 && len >= 3)		/* JIS X 0212 KANJI */
		{
			from++;
			*to = (SS3 << 16) | (*from++ << 8);
			*to |= *from++;
			len -= 3;
		}
		else if ((*from & 0x80) && len >= 2)	/* JIS X 0208 KANJI */
		{
			*to = *from++ << 8;
			*to |= *from++;
			len -= 2;
		}
		else	/* must be ASCII */
		{
			*to = *from++;
			len--;
		}
		to++;
		cnt++;
	}
	*to = 0;
	return (cnt);
}

static inline int
pg_euc_mblen(const unsigned char *s)
{
	int			len;

	if (*s == SS2)
		len = 2;
	else if (*s == SS3)
		len = 3;
	else if (*s & 0x80)
		len = 2;
	else
		len = 1;
	return (len);
}

/*
 * EUC_JP
 */
static int	pg_eucjp2wchar_with_len
			(const unsigned char *from, pg_wchar *to, int len)
{
	return (pg_euc2wchar_with_len(from, to, len));
}

static int
pg_eucjp_mblen(const unsigned char *s)
{
	return (pg_euc_mblen(s));
}

/*
 * EUC_KR
 */
static int	pg_euckr2wchar_with_len
			(const unsigned char *from, pg_wchar *to, int len)
{
	return (pg_euc2wchar_with_len(from, to, len));
}

static int
pg_euckr_mblen(const unsigned char *s)
{
	return (pg_euc_mblen(s));
}

/*
 * EUC_CN
 *
 */
static int	pg_euccn2wchar_with_len
			(const unsigned char *from, pg_wchar *to, int len)
{
	int			cnt = 0;

	while (len > 0 && *from)
	{
		if (*from == SS2 && len >= 3)	/* code set 2 (unused?) */
		{
			from++;
			*to = (SS2 << 16) | (*from++ << 8);
			*to |= *from++;
			len -= 3;
		}
		else if (*from == SS3 && len >= 3)		/* code set 3 (unsed ?) */
		{
			from++;
			*to = (SS3 << 16) | (*from++ << 8);
			*to |= *from++;
			len -= 3;
		}
		else if ((*from & 0x80) && len >= 2)	/* code set 1 */
		{
			*to = *from++ << 8;
			*to |= *from++;
			len -= 2;
		}
		else
		{
			*to = *from++;
			len--;
		}
		to++;
		cnt++;
	}
	*to = 0;
	return (cnt);
}

static int
pg_euccn_mblen(const unsigned char *s)
{
	int			len;

	if (*s & 0x80)
		len = 2;
	else
		len = 1;
	return (len);
}

/*
 * EUC_TW
 *
 */
static int	pg_euctw2wchar_with_len
			(const unsigned char *from, pg_wchar *to, int len)
{
	int			cnt = 0;

	while (len > 0 && *from)
	{
		if (*from == SS2 && len >= 4)	/* code set 2 */
		{
			from++;
			*to = (SS2 << 24) | (*from++ << 16) ;
			*to |= *from++ << 8;
			*to |= *from++;
			len -= 4;
		}
		else if (*from == SS3 && len >= 3)		/* code set 3 (unused?) */
		{
			from++;
			*to = (SS3 << 16) | (*from++ << 8);
			*to |= *from++;
			len -= 3;
		}
		else if ((*from & 0x80) && len >= 2)	/* code set 2 */
		{
			*to = *from++ << 8;
			*to |= *from++;
			len -= 2;
		}
		else
		{
			*to = *from++;
			len--;
		}
		to++;
		cnt++;
	}
	*to = 0;
	return (cnt);
}

static int
pg_euctw_mblen(const unsigned char *s)
{
	int			len;

	if (*s == SS2)
		len = 4;
	else if (*s == SS3)
		len = 3;
	else if (*s & 0x80)
		len = 2;
	else
		len = 1;
	return (len);
}

/*
 * JOHAB
 */
static int
pg_johab2wchar_with_len(const unsigned char *from, pg_wchar *to, int len)
{
	return (pg_euc2wchar_with_len(from, to, len));
}

static int
pg_johab_mblen(const unsigned char *s)
{
	return (pg_euc_mblen(s));
}

/*
 * convert UTF8 string to pg_wchar (UCS-4)
 * caller must allocate enough space for "to", including a trailing zero!
 * len: length of from.
 * "from" not necessarily null terminated.
 */
static int
pg_utf2wchar_with_len(const unsigned char *from, pg_wchar *to, int len)
{
	int			cnt = 0;
	uint32		c1,
				c2,
				c3;

	while (len > 0 && *from)
	{
		if ((*from & 0x80) == 0)
		{
			*to = *from++;
			len--;
		}
		else if ((*from & 0xe0) == 0xc0)
		{
			if (len < 2)
				break;			/* drop trailing incomplete char */
			c1 = *from++ & 0x1f;
			c2 = *from++ & 0x3f;
			*to = (c1 << 6) | c2;
			len -= 2;
		}
		else if ((*from & 0xf0) == 0xe0)
		{
			if (len < 3)
				break;			/* drop trailing incomplete char */
			c1 = *from++ & 0x0f;
			c2 = *from++ & 0x3f;
			c3 = *from++ & 0x3f;
			*to = (c1 << 12) | (c2 << 6) | c3;
			len -= 3;
		}
		else
		{
			/* treat a bogus char as length 1; not ours to raise error */
			*to = *from++;
			len--;
		}
		to++;
		cnt++;
	}
	*to = 0;
	return (cnt);
}

/*
 * Return the byte length of a UTF8 character pointed to by s
 *
 * Note: in the current implementation we do not support UTF8 sequences
 * of more than 3 bytes; hence do NOT return a value larger than 3.
 * We return "1" for any leading byte that is either flat-out illegal or
 * indicates a length larger than we support.
 *
 * pg_utf2wchar_with_len(), utf2ucs(), pg_utf8_islegal(), and perhaps
 * other places would need to be fixed to change this.
 */
int
pg_utf_mblen(const unsigned char *s)
{
	int			len;

	if ((*s & 0x80) == 0)
		len = 1;
	else if ((*s & 0xe0) == 0xc0)
		len = 2;
	else if ((*s & 0xf0) == 0xe0)
		len = 3;
#ifdef NOT_USED
	else if ((*s & 0xf8) == 0xf0)
		len = 4;
	else if ((*s & 0xfc) == 0xf8)
		len = 5;
	else if ((*s & 0xfe) == 0xfc)
		len = 6;
#endif
	else
		len = 1;
	return len;
}

/*
 * convert mule internal code to pg_wchar
 * caller should allocate enough space for "to"
 * len: length of from.
 * "from" not necessarily null terminated.
 */
static int
pg_mule2wchar_with_len(const unsigned char *from, pg_wchar *to, int len)
{
	int			cnt = 0;

	while (len > 0 && *from)
	{
		if (IS_LC1(*from) && len >= 2)
		{
			*to = *from++ << 16;
			*to |= *from++;
			len -= 2;
		}
		else if (IS_LCPRV1(*from) && len >= 3)
		{
			from++;
			*to = *from++ << 16;
			*to |= *from++;
			len -= 3;
		}
		else if (IS_LC2(*from) && len >= 3)
		{
			*to = *from++ << 16;
			*to |= *from++ << 8;
			*to |= *from++;
			len -= 3;
		}
		else if (IS_LCPRV2(*from) && len >= 4)
		{
			from++;
			*to = *from++ << 16;
			*to |= *from++ << 8;
			*to |= *from++;
			len -= 4;
		}
		else
		{						/* assume ASCII */
			*to = (unsigned char) *from++;
			len--;
		}
		to++;
		cnt++;
	}
	*to = 0;
	return (cnt);
}

int
pg_mule_mblen(const unsigned char *s)
{
	int			len;

	if (IS_LC1(*s))
		len = 2;
	else if (IS_LCPRV1(*s))
		len = 3;
	else if (IS_LC2(*s))
		len = 3;
	else if (IS_LCPRV2(*s))
		len = 4;
	else
	{							/* assume ASCII */
		len = 1;
	}
	return (len);
}

/*
 * ISO8859-1
 */
static int
pg_latin12wchar_with_len(const unsigned char *from, pg_wchar *to, int len)
{
	int			cnt = 0;

	while (len > 0 && *from)
	{
		*to++ = *from++;
		len--;
		cnt++;
	}
	*to = 0;
	return (cnt);
}

static int
pg_latin1_mblen(const unsigned char *s)
{
	return (1);
}

/*
 * SJIS
 */
static int
pg_sjis_mblen(const unsigned char *s)
{
	int			len;

	if (*s >= 0xa1 && *s <= 0xdf)
	{							/* 1 byte kana? */
		len = 1;
	}
	else if (*s > 0x7f)
	{							/* kanji? */
		len = 2;
	}
	else
	{							/* should be ASCII */
		len = 1;
	}
	return (len);
}

/*
 * Big5
 */
static int
pg_big5_mblen(const unsigned char *s)
{
	int			len;

	if (*s > 0x7f)
	{							/* kanji? */
		len = 2;
	}
	else
	{							/* should be ASCII */
		len = 1;
	}
	return (len);
}

/*
 * GBK
 */
static int
pg_gbk_mblen(const unsigned char *s)
{
	int			len;

	if (*s > 0x7f)
	{							/* kanji? */
		len = 2;
	}
	else
	{							/* should be ASCII */
		len = 1;
	}
	return (len);
}

/*
 * UHC
 */
static int
pg_uhc_mblen(const unsigned char *s)
{
	int			len;

	if (*s > 0x7f)
	{							/* 2byte? */
		len = 2;
	}
	else
	{							/* should be ASCII */
		len = 1;
	}
	return (len);
}

/*
 *	* GB18030
 *	 * Added by Bill Huang <bhuang@redhat.com>,<bill_huanghb@ybb.ne.jp>
 *	  */
static int
pg_gb18030_mblen(const unsigned char *s)
{
	int			len;

	if (*s <= 0x7f)
	{							/* ASCII */
		len = 1;
	}
	else
	{
		if ((*(s + 1) >= 0x40 && *(s + 1) <= 0x7e) || (*(s + 1) >= 0x80 && *(s + 1) <= 0xfe))
			len = 2;
		else if (*(s + 1) >= 0x30 && *(s + 1) <= 0x39)
			len = 4;
		else
			len = 2;
	}
	return (len);
}

/*
 *-------------------------------------------------------------------
 * multibyte sequence validators
 *
 * These functions accept "s", a pointer to the first byte of a string,
 * and "len", the remaining length of the string.  If there is a validly
 * encoded character beginning at *s, return its length in bytes; else
 * return -1.
 *
 * The functions can assume that len > 0 and that *s != '\0', but they must
 * test for and reject zeroes in any additional bytes of a multibyte character.
 *
 * Note that this definition allows the function for a single-byte
 * encoding to be just "return 1".
 *-------------------------------------------------------------------
 */

static int
pg_ascii_verifier(const unsigned char *s, int len)
{
	return 1;
}

#define IS_EUC_RANGE_VALID(c)	((c) >= 0xa1 && (c) <= 0xfe)

static int
pg_eucjp_verifier(const unsigned char *s, int len)
{
	int			l;
	unsigned char c1, c2;

	c1 = *s++;

	switch (c1)
	{
		case SS2:		/* JIS X 0201 */
			l = 2;
			if (l > len)
				return -1;
			c2 = *s++;
			if (c2 < 0xa1 || c2 > 0xdf)
				return -1;
			break;

		case SS3:		/* JIS X 0212 */
			l = 3;
			if (l > len)
				return -1;
			c2 = *s++;
			if (!IS_EUC_RANGE_VALID(c2))
				return -1;
			c2 = *s++;
			if (!IS_EUC_RANGE_VALID(c2))
				return -1;
			break;

		default:
			if (IS_HIGHBIT_SET(c1))		/* JIS X 0208? */
			{
				l = 2;
				if (l > len)
					return -1;
				if (!IS_EUC_RANGE_VALID(c1))
					return -1;
				c2 = *s++;
				if (!IS_EUC_RANGE_VALID(c2))
					return -1;
			}
			else		/* must be ASCII */
			{
				l = 1;
			}
			break;
	}

	return l;
}

static int
pg_euckr_verifier(const unsigned char *s, int len)
{
	int			l;
	unsigned char c1, c2;

	c1 = *s++;

	if (IS_HIGHBIT_SET(c1))
	{
		l = 2;
		if (l > len)
			return -1;
		if (!IS_EUC_RANGE_VALID(c1))
			return -1;
		c2 = *s++;
		if (!IS_EUC_RANGE_VALID(c2))
			return -1;
	}
	else		/* must be ASCII */
	{
		l = 1;
	}

	return l;
}

/* EUC-CN byte sequences are exactly same as EUC-KR */
#define pg_euccn_verifier	pg_euckr_verifier

static int
pg_euctw_verifier(const unsigned char *s, int len)
{
	int			l;
	unsigned char c1, c2;

	c1 = *s++;

	switch (c1)
	{
		case SS2:		/* CNS 11643 Plane 1-7 */
			l = 4;
			if (l > len)
				return -1;
			c2 = *s++;
			if (c2 < 0xa1 || c2 > 0xa7)
				return -1;
			c2 = *s++;
			if (!IS_EUC_RANGE_VALID(c2))
				return -1;
			c2 = *s++;
			if (!IS_EUC_RANGE_VALID(c2))
				return -1;
			break;

		case SS3:		/* unused */
			return -1;

		default:
			if (IS_HIGHBIT_SET(c1))		/* CNS 11643 Plane 1 */
			{
				l = 2;
				if (l > len)
					return -1;
				/* no further range check on c1? */
				c2 = *s++;
				if (!IS_EUC_RANGE_VALID(c2))
					return -1;
			}
			else		/* must be ASCII */
			{
				l = 1;
			}
			break;
	}
	return l;
}

static int
pg_johab_verifier(const unsigned char *s, int len)
{
	int l, mbl;
	unsigned char c;

	l = mbl = pg_johab_mblen(s);

	if (len < l)
		return -1;

	if (!IS_HIGHBIT_SET(*s))
		return mbl;

	while (--l > 0)
	{
		c = *++s;
		if (!IS_EUC_RANGE_VALID(c))
			return -1;
	}
	return mbl;
}

static int
pg_mule_verifier(const unsigned char *s, int len)
{
	int l, mbl;
	unsigned char c;

	l = mbl = pg_mule_mblen(s);

	if (len < l)
		return -1;

	while (--l > 0)
	{
		c = *++s;
		if (!IS_HIGHBIT_SET(c))
			return -1;
	}
	return mbl;
}

static int
pg_latin1_verifier(const unsigned char *s, int len)
{
	return 1;
}

static int
pg_sjis_verifier(const unsigned char *s, int len)
{
	int l, mbl;
	unsigned char c1, c2;

	l = mbl = pg_sjis_mblen(s);

	if (len < l)
		return -1;

	if (l == 1)					/* pg_sjis_mblen already verified it */
		return mbl;

	c1 = *s++;
	c2 = *s;
	if (!ISSJISHEAD(c1) || !ISSJISTAIL(c2))
		return -1;
	return mbl;
}

static int
pg_big5_verifier(const unsigned char *s, int len)
{
	int l, mbl;

	l = mbl = pg_big5_mblen(s);

	if (len < l)
		return -1;

	while (--l > 0)
	{
		if (*++s == '\0')
			return -1;
	}

	return mbl;
}

static int
pg_gbk_verifier(const unsigned char *s, int len)
{
	int l, mbl;

	l = mbl = pg_gbk_mblen(s);

	if (len < l)
		return -1;

	while (--l > 0)
	{
		if (*++s == '\0')
			return -1;
	}

	return mbl;
}

static int
pg_uhc_verifier(const unsigned char *s, int len)
{
	int l, mbl;

	l = mbl = pg_uhc_mblen(s);

	if (len < l)
		return -1;

	while (--l > 0)
	{
		if (*++s == '\0')
			return -1;
	}

	return mbl;
}

static int
pg_gb18030_verifier(const unsigned char *s, int len)
{
	int l, mbl;

	l = mbl = pg_gb18030_mblen(s);

	if (len < l)
		return -1;

	while (--l > 0)
	{
		if (*++s == '\0')
			return -1;
	}

	return mbl;
}

static int
pg_utf8_verifier(const unsigned char *s, int len)
{
	int l = pg_utf_mblen(s);

	if (len < l)
		return -1;

	if (!pg_utf8_islegal(s, l))
		return -1;

	return l;
}

/*
 * Check for validity of a single UTF-8 encoded character
 *
 * This directly implements the rules in RFC3629, modified to restrict
 * us to 16-bit Unicode code points (hence, at most 3 bytes in UTF8).
 * The bizarre-looking
 * restrictions on the second byte are meant to ensure that there isn't
 * more than one encoding of a given Unicode character point; that is,
 * you may not use a longer-than-necessary byte sequence with high order
 * zero bits to represent a character that would fit in fewer bytes.
 * To do otherwise is to create security hazards (eg, create an apparent
 * non-ASCII character that decodes to plain ASCII).
 *
 * length is assumed to have been obtained by pg_utf_mblen(), and the
 * caller must have checked that that many bytes are present in the buffer.
 */
bool
pg_utf8_islegal(const unsigned char *source, int length)
{
	unsigned char a;

	switch (length)
	{
		default:
			/* reject lengths 4, 5 and 6 for now */
			return false;
		case 3:
			a = source[2];
			if (a < 0x80 || a > 0xBF)
				return false;
			/* FALL THRU */
		case 2:
			a = source[1];
			switch (*source)
			{
				case 0xE0:
					if (a < 0xA0 || a > 0xBF)
						return false;
					break;
				case 0xED:
					if (a < 0x80 || a > 0x9F)
						return false;
					break;
				default:
					if (a < 0x80 || a > 0xBF)
						return false;
					break;
			}
			/* FALL THRU */
		case 1:
			a = *source;
			if (a >= 0x80 && a < 0xC2)
				return false;
			if (a > 0xEF)
				return false;
			break;
	}
	return true;
}

/*
 *-------------------------------------------------------------------
 * encoding info table
 *-------------------------------------------------------------------
 */
pg_wchar_tbl pg_wchar_table[] = {
	{pg_ascii2wchar_with_len, pg_ascii_mblen, pg_ascii_verifier, 1},		/* 0; PG_SQL_ASCII	*/
	{pg_eucjp2wchar_with_len, pg_eucjp_mblen, pg_eucjp_verifier, 3},		/* 1; PG_EUC_JP */
	{pg_euccn2wchar_with_len, pg_euccn_mblen, pg_euccn_verifier, 2},		/* 2; PG_EUC_CN */
	{pg_euckr2wchar_with_len, pg_euckr_mblen, pg_euckr_verifier, 3},		/* 3; PG_EUC_KR */
	{pg_euctw2wchar_with_len, pg_euctw_mblen, pg_euctw_verifier, 4},		/* 4; PG_EUC_TW */
	{pg_johab2wchar_with_len, pg_johab_mblen, pg_johab_verifier, 3},		/* 5; PG_JOHAB */
	{pg_utf2wchar_with_len, pg_utf_mblen, pg_utf8_verifier, 3},	/* 6; PG_UNICODE */
	{pg_mule2wchar_with_len, pg_mule_mblen, pg_mule_verifier, 4}, /* 7; PG_MULE_INTERNAL */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_verifier, 1},		/* 8; PG_LATIN1 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_verifier, 1},		/* 9; PG_LATIN2 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_verifier, 1},		/* 10; PG_LATIN3 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_verifier, 1},		/* 11; PG_LATIN4 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_verifier, 1},		/* 12; PG_LATIN5 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_verifier, 1},		/* 13; PG_LATIN6 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_verifier, 1},		/* 14; PG_LATIN7 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_verifier, 1},		/* 15; PG_LATIN8 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_verifier, 1},		/* 16; PG_LATIN9 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_verifier, 1},		/* 17; PG_LATIN10 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_verifier, 1},		/* 18; PG_WIN1256 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_verifier, 1},		/* 19; PG_TCVN */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_verifier, 1},		/* 20; PG_WIN874 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_verifier, 1},		/* 21; PG_KOI8 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_verifier, 1},		/* 22; PG_WIN1251 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_verifier, 1},		/* 23; PG_ALT */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_verifier, 1},		/* 24; ISO-8859-5 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_verifier, 1},		/* 25; ISO-8859-6 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_verifier, 1},		/* 26; ISO-8859-7 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_verifier, 1},		/* 27; ISO-8859-8 */
	{0, pg_sjis_mblen, pg_sjis_verifier, 2},		/* 28; PG_SJIS */
	{0, pg_big5_mblen, pg_big5_verifier, 2},		/* 29; PG_BIG5 */
	{0, pg_gbk_mblen, pg_gbk_verifier, 2},		/* 30; PG_GBK */
	{0, pg_uhc_mblen, pg_uhc_verifier, 2},		/* 31; PG_UHC */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_verifier, 1},		/* 32; PG_WIN1250 */
	{0, pg_gb18030_mblen, pg_gb18030_verifier, 4}	/* 33; PG_GB18030 */
};

/* returns the byte length of a word for mule internal code */
int
pg_mic_mblen(const unsigned char *mbstr)
{
	return (pg_mule_mblen(mbstr));
}

/*
 * Returns the byte length of a multibyte word.
 */
int
pg_encoding_mblen(int encoding, const unsigned char *mbstr)
{
	Assert(PG_VALID_ENCODING(encoding));

	return ((encoding >= 0 &&
			 encoding < sizeof(pg_wchar_table) / sizeof(pg_wchar_tbl)) ?
			((*pg_wchar_table[encoding].mblen) (mbstr)) :
			((*pg_wchar_table[PG_SQL_ASCII].mblen) (mbstr)));
}

/*
 * Verify the first multibyte character of the given string.
 * Return its byte length if good, -1 if bad.  (See comments above for
 * full details of the mbverify API.)
 */
int
pg_encoding_verifymb(int encoding, const char *mbstr, int len)
{
	Assert(PG_VALID_ENCODING(encoding));

	return ((encoding >= 0 &&
			 encoding < sizeof(pg_wchar_table) / sizeof(pg_wchar_tbl)) ?
		((*pg_wchar_table[encoding].mbverify) ((const unsigned char *) mbstr, len)) :
	((*pg_wchar_table[PG_SQL_ASCII].mbverify) ((const unsigned char *) mbstr, len)));
}

/*
 * fetch maximum length of a char encoding
 */
int
pg_encoding_max_length(int encoding)
{
	Assert(PG_VALID_ENCODING(encoding));

	return pg_wchar_table[encoding].maxmblen;
}

#ifndef FRONTEND

/*
 * fetch maximum length of the encoding for the current database
 */
int
pg_database_encoding_max_length(void)
{
	return pg_wchar_table[GetDatabaseEncoding()].maxmblen;
}

/*
 * Verify mbstr to make sure that it is validly encoded in the current
 * database encoding.  Otherwise same as pg_verify_mbstr().
 */
bool
pg_verifymbstr(const char *mbstr, int len, bool noError)
{
	return pg_verify_mbstr(GetDatabaseEncoding(), mbstr, len, noError);
}

/*
 * Verify mbstr to make sure that it is validly encoded in the specified
 * encoding.
 *
 * mbstr is not necessarily zero terminated; length of mbstr is
 * specified by len.
 *
 * If OK, return TRUE.	If a problem is found, return FALSE when noError is
 * true; when noError is false, ereport() a descriptive message.
 */
bool
pg_verify_mbstr(int encoding, const char *mbstr, int len, bool noError)
{
	mbverifier	mbverify;

	Assert(PG_VALID_ENCODING(encoding));

	/*
	 * In single-byte encodings, we need only reject nulls (\0).
	 */
	if (pg_encoding_max_length(encoding) <= 1)
	{
		const char *nullpos = memchr(mbstr, 0, len);

		if (nullpos == NULL)
			return true;
		if (noError)
			return false;
		report_invalid_encoding(encoding, nullpos, 1);
	}

	/* fetch function pointer just once */
	mbverify = pg_wchar_table[encoding].mbverify;

	while (len > 0)
	{
		int			l;

		/* fast path for ASCII-subset characters */
		if (!IS_HIGHBIT_SET(*mbstr))
		{
			if (*mbstr != '\0')
			{
				mbstr++;
				len--;
				continue;
			}
			if (noError)
				return false;
			report_invalid_encoding(encoding, mbstr, len);
		}

		l = (*mbverify) ((const unsigned char *) mbstr, len);

		if (l < 0)
		{
			if (noError)
				return false;
			report_invalid_encoding(encoding, mbstr, len);
		}

		mbstr += l;
		len -= l;
	}
	return true;
}

/*
 * check_encoding_conversion_args: check arguments of a conversion function
 *
 * "expected" arguments can be either an encoding ID or -1 to indicate that
 * the caller will check whether it accepts the ID.
 *
 * Note: the errors here are not really user-facing, so elog instead of
 * ereport seems sufficient.  Also, we trust that the "expected" encoding
 * arguments are valid encoding IDs, but we don't trust the actuals.
 */
void
check_encoding_conversion_args(int src_encoding,
							   int dest_encoding,
							   int len,
							   int expected_src_encoding,
							   int expected_dest_encoding)
{
	if (!PG_VALID_ENCODING(src_encoding))
		elog(ERROR, "invalid source encoding ID: %d", src_encoding);
	if (src_encoding != expected_src_encoding && expected_src_encoding >= 0)
		elog(ERROR, "expected source encoding \"%s\", but got \"%s\"",
			 pg_enc2name_tbl[expected_src_encoding].name,
			 pg_enc2name_tbl[src_encoding].name);
	if (!PG_VALID_ENCODING(dest_encoding))
		elog(ERROR, "invalid destination encoding ID: %d", dest_encoding);
	if (dest_encoding != expected_dest_encoding && expected_dest_encoding >= 0)
		elog(ERROR, "expected destination encoding \"%s\", but got \"%s\"",
			 pg_enc2name_tbl[expected_dest_encoding].name,
			 pg_enc2name_tbl[dest_encoding].name);
	if (len < 0)
		elog(ERROR, "encoding conversion length must not be negative");
}

/*
 * report_invalid_encoding: complain about invalid multibyte character
 *
 * note: len is remaining length of string, not length of character;
 * len must be greater than zero, as we always examine the first byte.
 */
void
report_invalid_encoding(int encoding, const char *mbstr, int len)
{
	int			l = pg_encoding_mblen(encoding, mbstr);
	char		buf[8 * 2 + 1];
	char	   *p = buf;
	int			j,
				jlimit;

	jlimit = Min(l, len);
	jlimit = Min(jlimit, 8);	/* prevent buffer overrun */

	for (j = 0; j < jlimit; j++)
		p += sprintf(p, "%02x", (unsigned char) mbstr[j]);

	ereport(ERROR,
			(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
			 errmsg("invalid byte sequence for encoding \"%s\": 0x%s",
					pg_enc2name_tbl[encoding].name,
					buf)));
}

/*
 * report_untranslatable_char: complain about untranslatable character
 *
 * note: len is remaining length of string, not length of character;
 * len must be greater than zero, as we always examine the first byte.
 */
void
report_untranslatable_char(int src_encoding, int dest_encoding,
						   const char *mbstr, int len)
{
	int			l = pg_encoding_mblen(src_encoding, mbstr);
	char		buf[8 * 2 + 1];
	char	   *p = buf;
	int			j,
				jlimit;

	jlimit = Min(l, len);
	jlimit = Min(jlimit, 8);	/* prevent buffer overrun */

	for (j = 0; j < jlimit; j++)
		p += sprintf(p, "%02x", (unsigned char) mbstr[j]);

	ereport(ERROR,
			(errcode(ERRCODE_UNTRANSLATABLE_CHARACTER),
			 errmsg("character 0x%s of encoding \"%s\" has no equivalent in \"%s\"",
					buf,
					pg_enc2name_tbl[src_encoding].name,
					pg_enc2name_tbl[dest_encoding].name)));
}

#endif



