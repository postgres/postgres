/*
 * Encoding names and routines for work with it. All
 * in this file is shared bedween FE and BE.
 *
 * $PostgreSQL: pgsql/src/backend/utils/mb/encnames.c,v 1.31 2006/07/14 14:52:25 momjian Exp $
 */
#ifdef FRONTEND
#include "postgres_fe.h"
#define Assert(condition)
#else
#include "postgres.h"
#include "utils/builtins.h"
#endif

#include <unistd.h>

#include "mb/pg_wchar.h"
#include <ctype.h>

/* ----------
 * All encoding names, sorted:		 *** A L P H A B E T I C ***
 *
 * All names must be without irrelevant chars, search routines use
 * isalnum() chars only. It means ISO-8859-1, iso_8859-1 and Iso8859_1
 * are always converted to 'iso88591'. All must be lower case.
 *
 * The table doesn't contain 'cs' aliases (like csISOLatin1). It's needed?
 *
 * Karel Zak, Aug 2001
 * ----------
 */
pg_encname	pg_encname_tbl[] =
{
	{
		"abc", PG_WIN1258
	},							/* alias for WIN1258 */
	{
		"alt", PG_WIN866
	},							/* IBM866 */
	{
		"big5", PG_BIG5
	},							/* Big5; Chinese for Taiwan multibyte set */
	{
		"euccn", PG_EUC_CN
	},							/* EUC-CN; Extended Unix Code for simplified
								 * Chinese */
	{
		"eucjp", PG_EUC_JP
	},							/* EUC-JP; Extended UNIX Code fixed Width for
								 * Japanese, standard OSF */
	{
		"euckr", PG_EUC_KR
	},							/* EUC-KR; Extended Unix Code for Korean , KS
								 * X 1001 standard */
	{
		"euctw", PG_EUC_TW
	},							/* EUC-TW; Extended Unix Code for
								 *
								 * traditional Chinese */
	{
		"gb18030", PG_GB18030
	},							/* GB18030;GB18030 */
	{
		"gbk", PG_GBK
	},							/* GBK; Chinese Windows CodePage 936
								 * simplified Chinese */
	{
		"iso88591", PG_LATIN1
	},							/* ISO-8859-1; RFC1345,KXS2 */
	{
		"iso885910", PG_LATIN6
	},							/* ISO-8859-10; RFC1345,KXS2 */
	{
		"iso885913", PG_LATIN7
	},							/* ISO-8859-13; RFC1345,KXS2 */
	{
		"iso885914", PG_LATIN8
	},							/* ISO-8859-14; RFC1345,KXS2 */
	{
		"iso885915", PG_LATIN9
	},							/* ISO-8859-15; RFC1345,KXS2 */
	{
		"iso885916", PG_LATIN10
	},							/* ISO-8859-16; RFC1345,KXS2 */
	{
		"iso88592", PG_LATIN2
	},							/* ISO-8859-2; RFC1345,KXS2 */
	{
		"iso88593", PG_LATIN3
	},							/* ISO-8859-3; RFC1345,KXS2 */
	{
		"iso88594", PG_LATIN4
	},							/* ISO-8859-4; RFC1345,KXS2 */
	{
		"iso88595", PG_ISO_8859_5
	},							/* ISO-8859-5; RFC1345,KXS2 */
	{
		"iso88596", PG_ISO_8859_6
	},							/* ISO-8859-6; RFC1345,KXS2 */
	{
		"iso88597", PG_ISO_8859_7
	},							/* ISO-8859-7; RFC1345,KXS2 */
	{
		"iso88598", PG_ISO_8859_8
	},							/* ISO-8859-8; RFC1345,KXS2 */
	{
		"iso88599", PG_LATIN5
	},							/* ISO-8859-9; RFC1345,KXS2 */
	{
		"johab", PG_JOHAB
	},							/* JOHAB; Extended Unix Code for simplified
								 * Chinese */
	{
		"koi8", PG_KOI8R
	},							/* _dirty_ alias for KOI8-R (backward
								 * compatibility) */
	{
		"koi8r", PG_KOI8R
	},							/* KOI8-R; RFC1489 */
	{
		"latin1", PG_LATIN1
	},							/* alias for ISO-8859-1 */
	{
		"latin10", PG_LATIN10
	},							/* alias for ISO-8859-16 */
	{
		"latin2", PG_LATIN2
	},							/* alias for ISO-8859-2 */
	{
		"latin3", PG_LATIN3
	},							/* alias for ISO-8859-3 */
	{
		"latin4", PG_LATIN4
	},							/* alias for ISO-8859-4 */
	{
		"latin5", PG_LATIN5
	},							/* alias for ISO-8859-9 */
	{
		"latin6", PG_LATIN6
	},							/* alias for ISO-8859-10 */
	{
		"latin7", PG_LATIN7
	},							/* alias for ISO-8859-13 */
	{
		"latin8", PG_LATIN8
	},							/* alias for ISO-8859-14 */
	{
		"latin9", PG_LATIN9
	},							/* alias for ISO-8859-15 */
	{
		"mskanji", PG_SJIS
	},							/* alias for Shift_JIS */
	{
		"muleinternal", PG_MULE_INTERNAL
	},
	{
		"shiftjis", PG_SJIS
	},							/* Shift_JIS; JIS X 0202-1991 */
	{
		"sjis", PG_SJIS
	},							/* alias for Shift_JIS */
	{
		"sqlascii", PG_SQL_ASCII
	},
	{
		"tcvn", PG_WIN1258
	},							/* alias for WIN1258 */
	{
		"tcvn5712", PG_WIN1258
	},							/* alias for WIN1258 */
	{
		"uhc", PG_UHC
	},							/* UHC; Korean Windows CodePage 949 */
	{
		"unicode", PG_UTF8
	},							/* alias for UTF8 */
	{
		"utf8", PG_UTF8
	},							/* alias for UTF8 */
	{
		"vscii", PG_WIN1258
	},							/* alias for WIN1258 */
	{
		"win", PG_WIN1251
	},							/* _dirty_ alias for windows-1251 (backward
								 * compatibility) */
	{
		"win1250", PG_WIN1250
	},							/* alias for Windows-1250 */
	{
		"win1251", PG_WIN1251
	},							/* alias for Windows-1251 */
	{
		"win1252", PG_WIN1252
	},							/* alias for Windows-1252 */
	{
		"win1253", PG_WIN1253
	},							/* alias for Windows-1253 */
	{
		"win1254", PG_WIN1254
	},							/* alias for Windows-1254 */
	{
		"win1255", PG_WIN1255
	},							/* alias for Windows-1255 */
	{
		"win1256", PG_WIN1256
	},							/* alias for Windows-1256 */
	{
		"win1257", PG_WIN1257
	},							/* alias for Windows-1257 */
	{
		"win1258", PG_WIN1258
	},							/* alias for Windows-1258 */
	{
		"win866", PG_WIN866
	},							/* IBM866 */
	{
		"win874", PG_WIN874
	},							/* alias for Windows-874 */
	{
		"win932", PG_SJIS
	},							/* alias for Shift_JIS */
	{
		"win936", PG_GBK
	},							/* alias for GBK */
	{
		"win949", PG_UHC
	},							/* alias for UHC */
	{
		"win950", PG_BIG5
	},							/* alias for BIG5 */
	{
		"windows1250", PG_WIN1250
	},							/* Windows-1251; Microsoft */
	{
		"windows1251", PG_WIN1251
	},							/* Windows-1251; Microsoft */
	{
		"windows1252", PG_WIN1252
	},							/* Windows-1252; Microsoft */
	{
		"windows1253", PG_WIN1253
	},							/* Windows-1253; Microsoft */
	{
		"windows1254", PG_WIN1254
	},							/* Windows-1254; Microsoft */
	{
		"windows1255", PG_WIN1255
	},							/* Windows-1255; Microsoft */
	{
		"windows1256", PG_WIN1256
	},							/* Windows-1256; Microsoft */
	{
		"windows1257", PG_WIN1257
	},							/* Windows-1257; Microsoft */
	{
		"windows1258", PG_WIN1258
	},							/* Windows-1258; Microsoft */
	{
		"windows866", PG_WIN866
	},							/* IBM866 */
	{
		"windows874", PG_WIN874
	},							/* Windows-874; Microsoft */
	{
		"windows932", PG_SJIS
	},							/* alias for Shift_JIS */
	{
		"windows936", PG_GBK
	},							/* alias for GBK */
	{
		"windows949", PG_UHC
	},							/* alias for UHC */
	{
		"windows950", PG_BIG5
	},							/* alias for BIG5 */
	{
		NULL, 0
	}							/* last */
};

unsigned int pg_encname_tbl_sz = \
sizeof(pg_encname_tbl) / sizeof(pg_encname_tbl[0]) - 1;

/* ----------
 * These are "official" encoding names.
 * XXX must be sorted by the same order as pg_enc type (see mb/pg_wchar.h)
 * ----------
 */
pg_enc2name pg_enc2name_tbl[] =
{
	{
		"SQL_ASCII", PG_SQL_ASCII
	},
	{
		"EUC_JP", PG_EUC_JP
	},
	{
		"EUC_CN", PG_EUC_CN
	},
	{
		"EUC_KR", PG_EUC_KR
	},
	{
		"EUC_TW", PG_EUC_TW
	},
	{
		"JOHAB", PG_JOHAB
	},
	{
		"UTF8", PG_UTF8
	},
	{
		"MULE_INTERNAL", PG_MULE_INTERNAL
	},
	{
		"LATIN1", PG_LATIN1
	},
	{
		"LATIN2", PG_LATIN2
	},
	{
		"LATIN3", PG_LATIN3
	},
	{
		"LATIN4", PG_LATIN4
	},
	{
		"LATIN5", PG_LATIN5
	},
	{
		"LATIN6", PG_LATIN6
	},
	{
		"LATIN7", PG_LATIN7
	},
	{
		"LATIN8", PG_LATIN8
	},
	{
		"LATIN9", PG_LATIN9
	},
	{
		"LATIN10", PG_LATIN10
	},
	{
		"WIN1256", PG_WIN1256
	},
	{
		"WIN1258", PG_WIN1258
	},
	{
		"WIN866", PG_WIN866
	},
	{
		"WIN874", PG_WIN874
	},
	{
		"KOI8", PG_KOI8R
	},
	{
		"WIN1251", PG_WIN1251
	},
	{
		"WIN1252", PG_WIN1252
	},
	{
		"ISO_8859_5", PG_ISO_8859_5
	},
	{
		"ISO_8859_6", PG_ISO_8859_6
	},
	{
		"ISO_8859_7", PG_ISO_8859_7
	},
	{
		"ISO_8859_8", PG_ISO_8859_8
	},
	{
		"WIN1250", PG_WIN1250
	},
	{
		"WIN1253", PG_WIN1253
	},
	{
		"WIN1254", PG_WIN1254
	},
	{
		"WIN1255", PG_WIN1255
	},
	{
		"WIN1257", PG_WIN1257
	},
	{
		"SJIS", PG_SJIS
	},
	{
		"BIG5", PG_BIG5
	},
	{
		"GBK", PG_GBK
	},
	{
		"UHC", PG_UHC
	},
	{
		"GB18030", PG_GB18030
	}
};

/* ----------
 * Encoding checks, for error returns -1 else encoding id
 * ----------
 */
int
pg_valid_client_encoding(const char *name)
{
	int			enc;

	if ((enc = pg_char_to_encoding(name)) < 0)
		return -1;

	if (!PG_VALID_FE_ENCODING(enc))
		return -1;

	return enc;
}

int
pg_valid_server_encoding(const char *name)
{
	int			enc;

	if ((enc = pg_char_to_encoding(name)) < 0)
		return -1;

	if (!PG_VALID_BE_ENCODING(enc))
		return -1;

	return enc;
}

/* ----------
 * Remove irrelevant chars from encoding name
 * ----------
 */
static char *
clean_encoding_name(char *key, char *newkey)
{
	char	   *p,
			   *np;

	for (p = key, np = newkey; *p != '\0'; p++)
	{
		if (isalnum((unsigned char) *p))
		{
			if (*p >= 'A' && *p <= 'Z')
				*np++ = *p + 'a' - 'A';
			else
				*np++ = *p;
		}
	}
	*np = '\0';
	return newkey;
}

/* ----------
 * Search encoding by encoding name
 * ----------
 */
pg_encname *
pg_char_to_encname_struct(const char *name)
{
	unsigned int nel = pg_encname_tbl_sz;
	pg_encname *base = pg_encname_tbl,
			   *last = base + nel - 1,
			   *position;
	int			result;
	char		buff[NAMEDATALEN],
			   *key;

	if (name == NULL || *name == '\0')
		return NULL;

	if (strlen(name) >= NAMEDATALEN)
	{
#ifdef FRONTEND
		fprintf(stderr, "encoding name too long\n");
		return NULL;
#else
		ereport(ERROR,
				(errcode(ERRCODE_NAME_TOO_LONG),
				 errmsg("encoding name too long")));
#endif
	}
	key = clean_encoding_name((char *) name, buff);

	while (last >= base)
	{
		position = base + ((last - base) >> 1);
		result = key[0] - position->name[0];

		if (result == 0)
		{
			result = strcmp(key, position->name);
			if (result == 0)
				return position;
		}
		if (result < 0)
			last = position - 1;
		else
			base = position + 1;
	}
	return NULL;
}

/*
 * Returns encoding or -1 for error
 */
int
pg_char_to_encoding(const char *s)
{
	pg_encname *p = NULL;

	if (!s)
		return -1;

	p = pg_char_to_encname_struct(s);
	return p ? p->encoding : -1;
}

#ifndef FRONTEND
Datum
PG_char_to_encoding(PG_FUNCTION_ARGS)
{
	Name		s = PG_GETARG_NAME(0);

	PG_RETURN_INT32(pg_char_to_encoding(NameStr(*s)));
}
#endif

const char *
pg_encoding_to_char(int encoding)
{
	if (PG_VALID_ENCODING(encoding))
	{
		pg_enc2name *p = &pg_enc2name_tbl[encoding];

		Assert(encoding == p->encoding);
		return p->name;
	}
	return "";
}

#ifndef FRONTEND
Datum
PG_encoding_to_char(PG_FUNCTION_ARGS)
{
	int32		encoding = PG_GETARG_INT32(0);
	const char *encoding_name = pg_encoding_to_char(encoding);

	return DirectFunctionCall1(namein, CStringGetDatum(encoding_name));
}

#endif
