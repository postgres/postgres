/*
 * conversion functions between pg_wchar and multibyte streams.
 * Tatsuo Ishii
 * $PostgreSQL: pgsql/src/backend/utils/mb/wchar.c,v 1.53 2006/02/10 00:39:04 momjian Exp $
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
 *
 * Note: for the display output of psql to work properly, the return values
 * of these functions must conform to the Unicode standard. In particular
 * the NUL character is zero width and control characters are generally
 * width -1. It is recommended that non-ASCII encodings refer their ASCII
 * subset to the ASCII routines to ensure consistancy.
 *
 */

/*
 * SQL/ASCII
 */
static int
pg_ascii2wchar_with_len
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
	return cnt;
}

static int
pg_ascii_mblen(const unsigned char *s)
{
	return 1;
}

static int
pg_ascii_dsplen(const unsigned char *s)
{
	if (*s == '\0')
		return 0;
	if (*s < 0x20 || *s == 0x7f)
		return -1;
		
	return 1;
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
		else if (IS_HIGHBIT_SET(*from) && len >= 2)	/* JIS X 0208 KANJI */
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
	return cnt;
}

static int
pg_euc_mblen(const unsigned char *s)
{
	int			len;

	if (*s == SS2)
		len = 2;
	else if (*s == SS3)
		len = 3;
	else if (IS_HIGHBIT_SET(*s))
		len = 2;
	else
		len = 1;
	return len;
}

static int
pg_euc_dsplen(const unsigned char *s)
{
	int			len;

	if (*s == SS2)
		len = 2;
	else if (*s == SS3)
		len = 2;
	else if (IS_HIGHBIT_SET(*s))
		len = 2;
	else
		len = pg_ascii_dsplen(s);
	return len;
}

/*
 * EUC_JP
 */
static int	pg_eucjp2wchar_with_len
			(const unsigned char *from, pg_wchar *to, int len)
{
	return pg_euc2wchar_with_len(from, to, len);
}

static int
pg_eucjp_mblen(const unsigned char *s)
{
	return pg_euc_mblen(s);
}

static int
pg_eucjp_dsplen(const unsigned char *s)
{
	int			len;

	if (*s == SS2)
		len = 1;
	else if (*s == SS3)
		len = 2;
	else if (IS_HIGHBIT_SET(*s))
		len = 2;
	else
		len = pg_ascii_dsplen(s);
	return len;
}

/*
 * EUC_KR
 */
static int	pg_euckr2wchar_with_len
			(const unsigned char *from, pg_wchar *to, int len)
{
	return pg_euc2wchar_with_len(from, to, len);
}

static int
pg_euckr_mblen(const unsigned char *s)
{
	return pg_euc_mblen(s);
}

static int
pg_euckr_dsplen(const unsigned char *s)
{
	return pg_euc_dsplen(s);
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
		else if (IS_HIGHBIT_SET(*from) && len >= 2)	/* code set 1 */
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
	return cnt;
}

static int
pg_euccn_mblen(const unsigned char *s)
{
	int			len;

	if (IS_HIGHBIT_SET(*s))
		len = 2;
	else
		len = 1;
	return len;
}

static int
pg_euccn_dsplen(const unsigned char *s)
{
	int			len;

	if (IS_HIGHBIT_SET(*s))
		len = 2;
	else
		len = pg_ascii_dsplen(s);
	return len;
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
		else if (IS_HIGHBIT_SET(*from) && len >= 2)	/* code set 2 */
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
	return cnt;
}

static int
pg_euctw_mblen(const unsigned char *s)
{
	int			len;

	if (*s == SS2)
		len = 4;
	else if (*s == SS3)
		len = 3;
	else if (IS_HIGHBIT_SET(*s))
		len = 2;
	else
		len = pg_ascii_dsplen(s);
	return len;
}

static int
pg_euctw_dsplen(const unsigned char *s)
{
	int			len;

	if (*s == SS2)
		len = 2;
	else if (*s == SS3)
		len = 2;
	else if (IS_HIGHBIT_SET(*s))
		len = 2;
	else
		len = pg_ascii_dsplen(s);
	return len;
}

/*
 * JOHAB
 */
static int
pg_johab2wchar_with_len(const unsigned char *from, pg_wchar *to, int len)
{
	return pg_euc2wchar_with_len(from, to, len);
}

static int
pg_johab_mblen(const unsigned char *s)
{
	return pg_euc_mblen(s);
}

static int
pg_johab_dsplen(const unsigned char *s)
{
	return pg_euc_dsplen(s);
}

/*
 * convert UTF8 string to pg_wchar (UCS-2)
 * caller should allocate enough space for "to"
 * len: length of from.
 * "from" not necessarily null terminated.
 */
static int
pg_utf2wchar_with_len(const unsigned char *from, pg_wchar *to, int len)
{
	unsigned char c1,
				c2,
				c3;
	int			cnt = 0;

	while (len > 0 && *from)
	{
		if (!IS_HIGHBIT_SET(*from))
		{
			*to = *from++;
			len--;
		}
		else if ((*from & 0xe0) == 0xc0 && len >= 2)
		{
			c1 = *from++ & 0x1f;
			c2 = *from++ & 0x3f;
			*to = c1 << 6;
			*to |= c2;
			len -= 2;
		}
		else if ((*from & 0xe0) == 0xe0 && len >= 3)
		{
			c1 = *from++ & 0x0f;
			c2 = *from++ & 0x3f;
			c3 = *from++ & 0x3f;
			*to = c1 << 12;
			*to |= c2 << 6;
			*to |= c3;
			len -= 3;
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
	return cnt;
}

/*
 * returns the byte length of a UTF8 word pointed to by s
 */
int
pg_utf_mblen(const unsigned char *s)
{
	int			len = 1;

	if ((*s & 0x80) == 0)
		len = 1;
	else if ((*s & 0xe0) == 0xc0)
		len = 2;
	else if ((*s & 0xf0) == 0xe0)
		len = 3;
	else if ((*s & 0xf8) == 0xf0)
		len = 4;
	else if ((*s & 0xfc) == 0xf8)
		len = 5;
	else if ((*s & 0xfe) == 0xfc)
		len = 6;
	return len;
}

/*
 * This is an implementation of wcwidth() and wcswidth() as defined in
 * "The Single UNIX Specification, Version 2, The Open Group, 1997"
 * <http://www.UNIX-systems.org/online.html>
 *
 * Markus Kuhn -- 2001-09-08 -- public domain
 *
 * customised for PostgreSQL
 *
 * original available at : http://www.cl.cam.ac.uk/~mgk25/ucs/wcwidth.c
 */

struct mbinterval
{
	unsigned short first;
	unsigned short last;
};

/* auxiliary function for binary search in interval table */
static int
mbbisearch(pg_wchar ucs, const struct mbinterval *table, int max)
{
	int			min = 0;
	int			mid;

	if (ucs < table[0].first || ucs > table[max].last)
		return 0;
	while (max >= min)
	{
		mid = (min + max) / 2;
		if (ucs > table[mid].last)
			min = mid + 1;
		else if (ucs < table[mid].first)
			max = mid - 1;
		else
			return 1;
	}

	return 0;
}


/* The following functions define the column width of an ISO 10646
 * character as follows:
 *
 *	  - The null character (U+0000) has a column width of 0.
 *
 *	  - Other C0/C1 control characters and DEL will lead to a return
 *		value of -1.
 *
 *	  - Non-spacing and enclosing combining characters (general
 *		category code Mn or Me in the Unicode database) have a
 *		column width of 0.
 *
 *	  - Other format characters (general category code Cf in the Unicode
 *		database) and ZERO WIDTH SPACE (U+200B) have a column width of 0.
 *
 *	  - Hangul Jamo medial vowels and final consonants (U+1160-U+11FF)
 *		have a column width of 0.
 *
 *	  - Spacing characters in the East Asian Wide (W) or East Asian
 *		FullWidth (F) category as defined in Unicode Technical
 *		Report #11 have a column width of 2.
 *
 *	  - All remaining characters (including all printable
 *		ISO 8859-1 and WGL4 characters, Unicode control characters,
 *		etc.) have a column width of 1.
 *
 * This implementation assumes that wchar_t characters are encoded
 * in ISO 10646.
 */

static int
ucs_wcwidth(pg_wchar ucs)
{
	/* sorted list of non-overlapping intervals of non-spacing characters */
	static const struct mbinterval combining[] = {
		{0x0300, 0x034E}, {0x0360, 0x0362}, {0x0483, 0x0486},
		{0x0488, 0x0489}, {0x0591, 0x05A1}, {0x05A3, 0x05B9},
		{0x05BB, 0x05BD}, {0x05BF, 0x05BF}, {0x05C1, 0x05C2},
		{0x05C4, 0x05C4}, {0x064B, 0x0655}, {0x0670, 0x0670},
		{0x06D6, 0x06E4}, {0x06E7, 0x06E8}, {0x06EA, 0x06ED},
		{0x070F, 0x070F}, {0x0711, 0x0711}, {0x0730, 0x074A},
		{0x07A6, 0x07B0}, {0x0901, 0x0902}, {0x093C, 0x093C},
		{0x0941, 0x0948}, {0x094D, 0x094D}, {0x0951, 0x0954},
		{0x0962, 0x0963}, {0x0981, 0x0981}, {0x09BC, 0x09BC},
		{0x09C1, 0x09C4}, {0x09CD, 0x09CD}, {0x09E2, 0x09E3},
		{0x0A02, 0x0A02}, {0x0A3C, 0x0A3C}, {0x0A41, 0x0A42},
		{0x0A47, 0x0A48}, {0x0A4B, 0x0A4D}, {0x0A70, 0x0A71},
		{0x0A81, 0x0A82}, {0x0ABC, 0x0ABC}, {0x0AC1, 0x0AC5},
		{0x0AC7, 0x0AC8}, {0x0ACD, 0x0ACD}, {0x0B01, 0x0B01},
		{0x0B3C, 0x0B3C}, {0x0B3F, 0x0B3F}, {0x0B41, 0x0B43},
		{0x0B4D, 0x0B4D}, {0x0B56, 0x0B56}, {0x0B82, 0x0B82},
		{0x0BC0, 0x0BC0}, {0x0BCD, 0x0BCD}, {0x0C3E, 0x0C40},
		{0x0C46, 0x0C48}, {0x0C4A, 0x0C4D}, {0x0C55, 0x0C56},
		{0x0CBF, 0x0CBF}, {0x0CC6, 0x0CC6}, {0x0CCC, 0x0CCD},
		{0x0D41, 0x0D43}, {0x0D4D, 0x0D4D}, {0x0DCA, 0x0DCA},
		{0x0DD2, 0x0DD4}, {0x0DD6, 0x0DD6}, {0x0E31, 0x0E31},
		{0x0E34, 0x0E3A}, {0x0E47, 0x0E4E}, {0x0EB1, 0x0EB1},
		{0x0EB4, 0x0EB9}, {0x0EBB, 0x0EBC}, {0x0EC8, 0x0ECD},
		{0x0F18, 0x0F19}, {0x0F35, 0x0F35}, {0x0F37, 0x0F37},
		{0x0F39, 0x0F39}, {0x0F71, 0x0F7E}, {0x0F80, 0x0F84},
		{0x0F86, 0x0F87}, {0x0F90, 0x0F97}, {0x0F99, 0x0FBC},
		{0x0FC6, 0x0FC6}, {0x102D, 0x1030}, {0x1032, 0x1032},
		{0x1036, 0x1037}, {0x1039, 0x1039}, {0x1058, 0x1059},
		{0x1160, 0x11FF}, {0x17B7, 0x17BD}, {0x17C6, 0x17C6},
		{0x17C9, 0x17D3}, {0x180B, 0x180E}, {0x18A9, 0x18A9},
		{0x200B, 0x200F}, {0x202A, 0x202E}, {0x206A, 0x206F},
		{0x20D0, 0x20E3}, {0x302A, 0x302F}, {0x3099, 0x309A},
		{0xFB1E, 0xFB1E}, {0xFE20, 0xFE23}, {0xFEFF, 0xFEFF},
		{0xFFF9, 0xFFFB}
	};

	/* test for 8-bit control characters */
	if (ucs == 0)
		return 0;

	if (ucs < 0x20 || (ucs >= 0x7f && ucs < 0xa0) || ucs > 0x0010ffff)
		return -1;

	/* binary search in table of non-spacing characters */
	if (mbbisearch(ucs, combining,
				   sizeof(combining) / sizeof(struct mbinterval) - 1))
		return 0;

	/*
	 * if we arrive here, ucs is not a combining or C0/C1 control character
	 */

	return 1 +
		(ucs >= 0x1100 &&
		 (ucs <= 0x115f ||		/* Hangul Jamo init. consonants */
		  (ucs >= 0x2e80 && ucs <= 0xa4cf && (ucs & ~0x0011) != 0x300a &&
		   ucs != 0x303f) ||	/* CJK ... Yi */
		  (ucs >= 0xac00 && ucs <= 0xd7a3) ||	/* Hangul Syllables */
		  (ucs >= 0xf900 && ucs <= 0xfaff) ||	/* CJK Compatibility
												 * Ideographs */
		  (ucs >= 0xfe30 && ucs <= 0xfe6f) ||	/* CJK Compatibility Forms */
		  (ucs >= 0xff00 && ucs <= 0xff5f) ||	/* Fullwidth Forms */
		  (ucs >= 0xffe0 && ucs <= 0xffe6) ||
		  (ucs >= 0x20000 && ucs <= 0x2ffff)));
}

static pg_wchar
utf2ucs(const unsigned char *c)
{
	/*
	 * one char version of pg_utf2wchar_with_len. no control here, c must
	 * point to a large enough string
	 */
	if ((*c & 0x80) == 0)
		return (pg_wchar) c[0];
	else if ((*c & 0xe0) == 0xc0)
		return (pg_wchar) (((c[0] & 0x1f) << 6) |
						   (c[1] & 0x3f));
	else if ((*c & 0xf0) == 0xe0)
		return (pg_wchar) (((c[0] & 0x0f) << 12) |
						   ((c[1] & 0x3f) << 6) |
						   (c[2] & 0x3f));
	else if ((*c & 0xf0) == 0xf0)
		return (pg_wchar) (((c[0] & 0x07) << 18) |
						   ((c[1] & 0x3f) << 12) |
						   ((c[2] & 0x3f) << 6) |
						   (c[3] & 0x3f));
	else
		/* that is an invalid code on purpose */
		return 0xffffffff;
}

static int
pg_utf_dsplen(const unsigned char *s)
{
	return ucs_wcwidth(utf2ucs(s));
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
	return cnt;
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
		len = 1;	/* assume ASCII */
	return len;
}

static int
pg_mule_dsplen(const unsigned char *s)
{
	return pg_ascii_dsplen(s);					/* XXX fix me! */
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
	return cnt;
}

static int
pg_latin1_mblen(const unsigned char *s)
{
	return 1;
}

static int
pg_latin1_dsplen(const unsigned char *s)
{
	return pg_ascii_dsplen(s);
}

/*
 * SJIS
 */
static int
pg_sjis_mblen(const unsigned char *s)
{
	int			len;

	if (*s >= 0xa1 && *s <= 0xdf)
		len = 1;	/* 1 byte kana? */
	else if (IS_HIGHBIT_SET(*s))
		len = 2;	/* kanji? */
	else
		len = 1;	/* should be ASCII */
	return len;
}

static int
pg_sjis_dsplen(const unsigned char *s)
{
	int			len;

	if (*s >= 0xa1 && *s <= 0xdf)
		len = 1;	/* 1 byte kana? */
	else if (IS_HIGHBIT_SET(*s))
		len = 2;	/* kanji? */
	else
		len = pg_ascii_dsplen(s);	/* should be ASCII */
	return len;
}

/*
 * Big5
 */
static int
pg_big5_mblen(const unsigned char *s)
{
	int			len;

	if (IS_HIGHBIT_SET(*s))
		len = 2;	/* kanji? */
	else
		len = 1;	/* should be ASCII */
	return len;
}

static int
pg_big5_dsplen(const unsigned char *s)
{
	int			len;

	if (IS_HIGHBIT_SET(*s))
		len = 2;	/* kanji? */
	else
		len = pg_ascii_dsplen(s);	/* should be ASCII */
	return len;
}

/*
 * GBK
 */
static int
pg_gbk_mblen(const unsigned char *s)
{
	int			len;

	if (IS_HIGHBIT_SET(*s))
		len = 2;	/* kanji? */
	else
		len = 1;	/* should be ASCII */
	return len;
}

static int
pg_gbk_dsplen(const unsigned char *s)
{
	int			len;

	if (IS_HIGHBIT_SET(*s))
		len = 2;	/* kanji? */
	else
		len = pg_ascii_dsplen(s);	/* should be ASCII */
	return len;
}

/*
 * UHC
 */
static int
pg_uhc_mblen(const unsigned char *s)
{
	int			len;

	if (IS_HIGHBIT_SET(*s))
		len = 2;	/* 2byte? */
	else
		len = 1;	/* should be ASCII */
	return len;
}

static int
pg_uhc_dsplen(const unsigned char *s)
{
	int			len;

	if (IS_HIGHBIT_SET(*s))
		len = 2;	/* 2byte? */
	else
		len = pg_ascii_dsplen(s);	/* should be ASCII */
	return len;
}

/*
 *	* GB18030
 *	 * Added by Bill Huang <bhuang@redhat.com>,<bill_huanghb@ybb.ne.jp>
 *	  */
static int
pg_gb18030_mblen(const unsigned char *s)
{
	int			len;

	if (!IS_HIGHBIT_SET(*s))
		len = 1;	/* ASCII */
	else
	{
		if ((*(s + 1) >= 0x40 && *(s + 1) <= 0x7e) || (*(s + 1) >= 0x80 && *(s + 1) <= 0xfe))
			len = 2;
		else if (*(s + 1) >= 0x30 && *(s + 1) <= 0x39)
			len = 4;
		else
			len = 2;
	}
	return len;
}

static int
pg_gb18030_dsplen(const unsigned char *s)
{
	int			len;

	if (IS_HIGHBIT_SET(*s))
		len = 2;
	else
		len = pg_ascii_dsplen(s);	/* ASCII */
	return len;
}


pg_wchar_tbl pg_wchar_table[] = {
	{pg_ascii2wchar_with_len, pg_ascii_mblen, pg_ascii_dsplen, 1},		/* 0; PG_SQL_ASCII	*/
	{pg_eucjp2wchar_with_len, pg_eucjp_mblen, pg_eucjp_dsplen, 3},		/* 1; PG_EUC_JP */
	{pg_euccn2wchar_with_len, pg_euccn_mblen, pg_euccn_dsplen, 3},		/* 2; PG_EUC_CN */
	{pg_euckr2wchar_with_len, pg_euckr_mblen, pg_euckr_dsplen, 3},		/* 3; PG_EUC_KR */
	{pg_euctw2wchar_with_len, pg_euctw_mblen, pg_euctw_dsplen, 3},		/* 4; PG_EUC_TW */
	{pg_johab2wchar_with_len, pg_johab_mblen, pg_johab_dsplen, 3},		/* 5; PG_JOHAB */
	{pg_utf2wchar_with_len, pg_utf_mblen, pg_utf_dsplen, 4},	/* 6; PG_UTF8 */
	{pg_mule2wchar_with_len, pg_mule_mblen, pg_mule_dsplen, 3}, /* 7; PG_MULE_INTERNAL */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 8; PG_LATIN1 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 9; PG_LATIN2 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 10; PG_LATIN3 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 11; PG_LATIN4 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 12; PG_LATIN5 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 13; PG_LATIN6 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 14; PG_LATIN7 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 15; PG_LATIN8 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 16; PG_LATIN9 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 17; PG_LATIN10 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 18; PG_WIN1256 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 19; PG_WIN1258 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 20; PG_WIN874 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 21; PG_KOI8 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 22; PG_WIN1251 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 22; PG_WIN1252 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 23; PG_WIN866 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 24; ISO-8859-5 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 25; ISO-8859-6 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 26; ISO-8859-7 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 27; ISO-8859-8 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 28; PG_WIN1250 */
	{0, pg_sjis_mblen, pg_sjis_dsplen, 2},		/* 29; PG_SJIS */
	{0, pg_big5_mblen, pg_big5_dsplen, 2},		/* 30; PG_BIG5 */
	{0, pg_gbk_mblen, pg_gbk_dsplen, 2},		/* 31; PG_GBK */
	{0, pg_uhc_mblen, pg_uhc_dsplen, 2},		/* 32; PG_UHC */
	{0, pg_gb18030_mblen, pg_gb18030_dsplen, 2} /* 33; PG_GB18030 */
};

/* returns the byte length of a word for mule internal code */
int
pg_mic_mblen(const unsigned char *mbstr)
{
	return pg_mule_mblen(mbstr);
}

/*
 * Returns the byte length of a multibyte word.
 */
int
pg_encoding_mblen(int encoding, const char *mbstr)
{
	Assert(PG_VALID_ENCODING(encoding));

	return ((encoding >= 0 &&
			 encoding < sizeof(pg_wchar_table) / sizeof(pg_wchar_tbl)) ?
		((*pg_wchar_table[encoding].mblen) ((const unsigned char *) mbstr)) :
	((*pg_wchar_table[PG_SQL_ASCII].mblen) ((const unsigned char *) mbstr)));
}

/*
 * Returns the display length of a multibyte word.
 */
int
pg_encoding_dsplen(int encoding, const char *mbstr)
{
	Assert(PG_VALID_ENCODING(encoding));

	return ((encoding >= 0 &&
			 encoding < sizeof(pg_wchar_table) / sizeof(pg_wchar_tbl)) ?
	   ((*pg_wchar_table[encoding].dsplen) ((const unsigned char *) mbstr)) :
	((*pg_wchar_table[PG_SQL_ASCII].dsplen) ((const unsigned char *) mbstr)));
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

bool
pg_utf8_islegal(const unsigned char *source, int length)
{
	unsigned char a;
	const unsigned char *srcptr = source + length;

	switch (length)
	{
		default:
			return false;
			/* Everything else falls through when "true"... */
		case 4:
			if ((a = (*--srcptr)) < 0x80 || a > 0xBF)
				return false;
		case 3:
			if ((a = (*--srcptr)) < 0x80 || a > 0xBF)
				return false;
		case 2:
			if ((a = (*--srcptr)) > 0xBF)
				return false;
			switch (*source)
			{
					/* no fall-through in this inner switch */
				case 0xE0:
					if (a < 0xA0)
						return false;
					break;
				case 0xED:
					if (a > 0x9F)
						return false;
					break;
				case 0xF0:
					if (a < 0x90)
						return false;
					break;
				case 0xF4:
					if (a > 0x8F)
						return false;
					break;
				default:
					if (a < 0x80)
						return false;
			}

		case 1:
			if (*source >= 0x80 && *source < 0xC2)
				return false;
	}
	if (*source > 0xF4)
		return false;
	return true;
}


/*
 * Verify mbstr to make sure that it has a valid character sequence.
 * mbstr is not necessarily NULL terminated; length of mbstr is
 * specified by len.
 *
 * If OK, return TRUE.	If a problem is found, return FALSE when noError is
 * true; when noError is false, ereport() a descriptive message.
 */
bool
pg_verifymbstr(const char *mbstr, int len, bool noError)
{
	int			l;
	int			i;
	int			encoding;

	/* we do not need any check in single-byte encodings */
	if (pg_database_encoding_max_length() <= 1)
		return true;

	encoding = GetDatabaseEncoding();

	while (len > 0 && *mbstr)
	{
		l = pg_mblen(mbstr);

		/* special UTF-8 check */
		if (encoding == PG_UTF8)
		{
			if (!pg_utf8_islegal((const unsigned char *) mbstr, l))
			{
				if (noError)
					return false;
				ereport(ERROR,
						(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
						 errmsg("invalid UTF-8 byte sequence detected near byte 0x%02x",
								(unsigned char) *mbstr)));
			}
		}
		else
		{
			for (i = 1; i < l; i++)
			{
				/*
				 * we expect that every multibyte char consists of bytes
				 * having the 8th bit set
				 */
				if (i >= len || !IS_HIGHBIT_SET(mbstr[i]))
				{
					char		buf[8 * 2 + 1];
					char	   *p = buf;
					int			j,
								jlimit;

					if (noError)
						return false;

					jlimit = Min(l, len);
					jlimit = Min(jlimit, 8);	/* prevent buffer overrun */

					for (j = 0; j < jlimit; j++)
						p += sprintf(p, "%02x", (unsigned char) mbstr[j]);

					ereport(ERROR,
							(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
					errmsg("invalid byte sequence for encoding \"%s\": 0x%s",
						   GetDatabaseEncodingName(), buf)));
				}
			}
		}
		len -= l;
		mbstr += l;
	}
	return true;
}

/*
 * fetch maximum length of a char encoding for the current database
 */
int
pg_database_encoding_max_length(void)
{
	return pg_wchar_table[GetDatabaseEncoding()].maxmblen;
}

#endif
