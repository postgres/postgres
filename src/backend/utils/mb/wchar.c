/*
 * conversion functions between pg_wchar and multibyte streams.
 * Tatsuo Ishii
 * $PostgreSQL: pgsql/src/backend/utils/mb/wchar.c,v 1.39 2004/12/02 22:37:13 momjian Exp $
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

static int
pg_ascii_dsplen(const unsigned char *s)
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
		if (*from == SS2 && len >= 2)
		{
			from++;
			*to = 0xff & *from++;
			len -= 2;
		}
		else if (*from == SS3 && len >= 3)
		{
			from++;
			*to = *from++ << 8;
			*to |= 0x3f & *from++;
			len -= 3;
		}
		else if ((*from & 0x80) && len >= 2)
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

static int
pg_euc_dsplen(const unsigned char *s)
{
	int			len;

	if (*s == SS2)
		len = 2;
	else if (*s == SS3)
		len = 2;
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

static int
pg_eucjp_dsplen(const unsigned char *s)
{
	int			len;

	if (*s == SS2)
		len = 1;
	else if (*s == SS3)
		len = 2;
	else if (*s & 0x80)
		len = 2;
	else
		len = 1;
	return (len);
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

static int
pg_euckr_dsplen(const unsigned char *s)
{
	return (pg_euc_dsplen(s));
}

/*
 * EUC_CN
 */
static int	pg_euccn2wchar_with_len
			(const unsigned char *from, pg_wchar *to, int len)
{
	int			cnt = 0;

	while (len > 0 && *from)
	{
		if (*from == SS2 && len >= 3)
		{
			from++;
			*to = 0x3f00 & (*from++ << 8);
			*to = *from++;
			len -= 3;
		}
		else if (*from == SS3 && len >= 3)
		{
			from++;
			*to = *from++ << 8;
			*to |= 0x3f & *from++;
			len -= 3;
		}
		else if ((*from & 0x80) && len >= 2)
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

static int
pg_euccn_dsplen(const unsigned char *s)
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
 */
static int	pg_euctw2wchar_with_len
			(const unsigned char *from, pg_wchar *to, int len)
{
	int			cnt = 0;

	while (len > 0 && *from)
	{
		if (*from == SS2 && len >= 4)
		{
			from++;
			*to = *from++ << 16;
			*to |= *from++ << 8;
			*to |= *from++;
			len -= 4;
		}
		else if (*from == SS3 && len >= 3)
		{
			from++;
			*to = *from++ << 8;
			*to |= 0x3f & *from++;
			len -= 3;
		}
		else if ((*from & 0x80) && len >= 2)
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

static int
pg_euctw_dsplen(const unsigned char *s)
{
	int			len;

	if (*s == SS2)
		len = 2;
	else if (*s == SS3)
		len = 2;
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

static int
pg_johab_dsplen(const unsigned char *s)
{
	return (pg_euc_dsplen(s));
}

bool isLegalUTF8(const UTF8 *source, int len) {
        UTF8 a;
        const UTF8 *srcptr = source+len;
        if(!source || (pg_utf_mblen(source) != len)) return false;
        switch (len) {
            default: return false;
            /* Everything else falls through when "true"... */
            case 6: if ((a = (*--srcptr)) < 0x80 || a > 0xBF) return false;
            case 5: if ((a = (*--srcptr)) < 0x80 || a > 0xBF) return false;
            case 4: if ((a = (*--srcptr)) < 0x80 || a > 0xBF) return false;
            case 3: if ((a = (*--srcptr)) < 0x80 || a > 0xBF) return false;
            case 2: if ((a = (*--srcptr)) > 0xBF) return false;
            switch (*source) {
                    /* no fall-through in this inner switch */
                    case 0xE0: if (a < 0xA0) return false; break;
                    case 0xF0: if (a < 0x90) return false; break;
                    case 0xF4: if (a > 0x8F) return false; break;
                    default:  if (a < 0x80) return false;
            }
            case 1: if (*source >= 0x80 && *source < 0xC2) return false;
            if (*source > 0xFD) return false;
        }
        return true;
}

/*
 * convert UTF-8 string to pg_wchar (UCS-2)
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
		if ((*from & 0x80) == 0)
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
	return (cnt);
}

/*
 * returns the byte length of a UTF-8 word pointed to by s
 */
int
pg_utf_mblen(const UTF8 *s)
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
	return (len);
}

static int
pg_utf_dsplen(const UTF8 *s)
{
	return 1;					/* XXX fix me! */
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

static int
pg_mule_dsplen(const unsigned char *s)
{
	return 1;					/* XXX fix me! */
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

static int
pg_latin1_dsplen(const unsigned char *s)
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

static int
pg_sjis_dsplen(const unsigned char *s)
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

static int
pg_big5_dsplen(const unsigned char *s)
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

static int
pg_gbk_dsplen(const unsigned char *s)
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

static int
pg_uhc_dsplen(const unsigned char *s)
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

static int
pg_gb18030_dsplen(const unsigned char *s)
{
	int			len;

	if (*s <= 0x7f)
	{							/* ASCII */
		len = 1;
	}
	else
		len = 2;
	return (len);
}


pg_wchar_tbl pg_wchar_table[] = {
	{pg_ascii2wchar_with_len, pg_ascii_mblen, pg_ascii_dsplen, 1},		/* 0; PG_SQL_ASCII	*/
	{pg_eucjp2wchar_with_len, pg_eucjp_mblen, pg_eucjp_dsplen, 3},		/* 1; PG_EUC_JP */
	{pg_euccn2wchar_with_len, pg_euccn_mblen, pg_euccn_dsplen, 3},		/* 2; PG_EUC_CN */
	{pg_euckr2wchar_with_len, pg_euckr_mblen, pg_euckr_dsplen, 3},		/* 3; PG_EUC_KR */
	{pg_euctw2wchar_with_len, pg_euctw_mblen, pg_euctw_dsplen, 3},		/* 4; PG_EUC_TW */
	{pg_johab2wchar_with_len, pg_johab_mblen, pg_johab_dsplen, 3},		/* 5; PG_JOHAB */
	{pg_utf2wchar_with_len, pg_utf_mblen, pg_utf_dsplen, 6},		/* 6; PG_UNICODE */
	{pg_mule2wchar_with_len, pg_mule_mblen, pg_mule_dsplen, 3},		/* 7; PG_MULE_INTERNAL */
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
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 19; PG_TCVN */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 20; PG_WIN874 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 21; PG_KOI8 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 22; PG_WIN1251 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 23; PG_ALT */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 24; ISO-8859-5 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 25; ISO-8859-6 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 26; ISO-8859-7 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 27; ISO-8859-8 */
	{pg_latin12wchar_with_len, pg_latin1_mblen, pg_latin1_dsplen, 1},	/* 28; PG_WIN1250 */
	{0, pg_sjis_mblen, pg_sjis_dsplen, 2},					/* 29; PG_SJIS */
	{0, pg_big5_mblen, pg_big5_dsplen, 2},					/* 30; PG_BIG5 */
	{0, pg_gbk_mblen, pg_gbk_dsplen, 2},					/* 31; PG_GBK */
	{0, pg_uhc_mblen, pg_uhc_dsplen, 2},					/* 32; PG_UHC */
	{0, pg_gb18030_mblen, pg_gb18030_dsplen, 2}				/* 33; PG_GB18030 */
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
 * Returns the display length of a multibyte word.
 */
int
pg_encoding_dsplen(int encoding, const unsigned char *mbstr)
{
	Assert(PG_VALID_ENCODING(encoding));

	return ((encoding >= 0 &&
			 encoding < sizeof(pg_wchar_table) / sizeof(pg_wchar_tbl)) ?
			((*pg_wchar_table[encoding].dsplen) (mbstr)) :
			((*pg_wchar_table[PG_SQL_ASCII].dsplen) (mbstr)));
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
 * Verify mbstr to make sure that it has a valid character sequence.
 * mbstr is not necessarily NULL terminated; length of mbstr is
 * specified by len.
 *
 * If OK, return TRUE.	If a problem is found, return FALSE when noError is
 * true; when noError is false, ereport() a descriptive message.
 */
bool
pg_verifymbstr(const unsigned char *mbstr, int len, bool noError)
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
		if (encoding == PG_UTF8) {
			if(!isLegalUTF8(mbstr,l)) {
				if (noError) return false;
				ereport(ERROR,(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),errmsg("Invalid UNICODE byte sequence detected near character %c",*mbstr)));
			}
		} else {
			for (i = 1; i < l; i++)
			{
				/*
				 * we expect that every multibyte char consists of bytes
				 * having the 8th bit set
				 */
				if (i >= len || (mbstr[i] & 0x80) == 0)
				{
					char		buf[8 * 2 + 1];
					char	   *p = buf;
					int			j,
							jlimit;

					if (noError)
						return false;

					jlimit = Min(l, len);
					jlimit = Min(jlimit, 8);		/* prevent buffer overrun */

					for (j = 0; j < jlimit; j++)
						p += sprintf(p, "%02x", mbstr[j]);

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
