/*
 * Encoding names and routines for work with it. All
 * in this file is shared bedween FE and BE.
 *
 * $Id: encnames.c,v 1.3 2001/10/11 14:20:35 ishii Exp $
 */
#ifdef FRONTEND
#include "postgres_fe.h"
#define Assert(condition)
#else
#include "postgres.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#endif

#ifdef WIN32
#include "win32.h"
#else
#include <unistd.h>
#endif

#include "mb/pg_wchar.h"
#include <ctype.h>

/* ----------
 * All encoding names, sorted:       *** A L P H A B E T I C ***
 *
 * All names must be without irrelevan chars, search routines use
 * isalnum() chars only. It means ISO-8859-1, iso_8859-1 and Iso8859_1
 * are always converted to 'iso88591'. All must be lower case.
 *
 * The table doesn't contain 'cs' aliases (like csISOLatin1). It's needful?
 *
 * Karel Zak, Aug 2001
 * ----------
 */
pg_encname pg_encname_tbl[] =
{
	{ "alt",	PG_ALT },		/* IBM866 */
	{ "big5",	PG_BIG5 },		/* Big5; Chinese for Taiwan Multi-byte set */
	{ "euccn",	PG_EUC_CN },		/* EUC-CN; ??? */
	{ "eucjp",	PG_EUC_JP },		/* EUC-JP; Extended UNIX Code Fixed Width for Japanese, stdandard OSF */
	{ "euckr",	PG_EUC_KR },		/* EUC-KR; RFC1557,Choi */
	{ "euctw",	PG_EUC_TW },		/* EUC-TW; ???  */
	{ "iso88591",	PG_LATIN1 },		/* ISO-8859-1; RFC1345,KXS2 */
	{ "iso885910",	PG_ISO_8859_10 },	/* ISO-8859-10; RFC1345,KXS2 */
	{ "iso885913",	PG_ISO_8859_13 },	/* ISO-8859-13; RFC1345,KXS2 */
	{ "iso885914",	PG_ISO_8859_14 },	/* ISO-8859-14; RFC1345,KXS2 */
	{ "iso885915",	PG_ISO_8859_15 },	/* ISO-8859-15; RFC1345,KXS2 */
	{ "iso885916",	PG_ISO_8859_16 },	/* ISO-8859-15; RFC1345,KXS2 */
	{ "iso88592",	PG_LATIN2 },		/* ISO-8859-2; RFC1345,KXS2 */
	{ "iso88593",	PG_LATIN3 },		/* ISO-8859-3; RFC1345,KXS2 */
	{ "iso88594",	PG_LATIN4 },		/* ISO-8859-4; RFC1345,KXS2 */
	{ "iso88595",	PG_ISO_8859_5 },	/* ISO-8859-5; RFC1345,KXS2 */
	{ "iso88596",	PG_ISO_8859_6 },	/* ISO-8859-6; RFC1345,KXS2 */
	{ "iso88597",	PG_ISO_8859_7 },	/* ISO-8859-7; RFC1345,KXS2 */
	{ "iso88598",	PG_ISO_8859_8 },	/* ISO-8859-8; RFC1345,KXS2 */
	{ "iso88599",	PG_LATIN5 },		/* ISO-8859-9; RFC1345,KXS2 */
	{ "koi8",	PG_KOI8R },		/* _dirty_ alias for KOI8-R (backward compatibility) */
	{ "koi8r",	PG_KOI8R },		/* KOI8-R; RFC1489 */
	{ "latin1",	PG_LATIN1 },		/* alias for ISO-8859-1 */
	{ "latin2",	PG_LATIN2 },		/* alias for ISO-8859-2 */
	{ "latin3",	PG_LATIN3 },		/* alias for ISO-8859-3 */
	{ "latin4",	PG_LATIN4 },		/* alias for ISO-8859-4 */
	{ "latin5",	PG_LATIN5 },		/* alias for ISO-8859-9 */
	{ "latin6",	PG_ISO_8859_10},	/* alias for ISO-8859-10 */
	{ "latin7",	PG_ISO_8859_13},	/* alias for ISO-8859-13 */
	{ "latin8",	PG_ISO_8859_14},	/* alias for ISO-8859-14 */
	{ "latin9",	PG_ISO_8859_15},	/* alias for ISO-8859-15 */
	{ "mskanji",	PG_SJIS },		/* alias for Shift_JIS */
	{ "muleinternal",PG_MULE_INTERNAL },
	{ "shiftjis",	PG_SJIS },		/* Shift_JIS; JIS X 0202-1991 */
	{ "sjis",	PG_SJIS },		/* alias for Shift_JIS */
	{ "sqlascii",	PG_SQL_ASCII },
	{ "unicode",	PG_UTF8 },		/* alias for UTF-8 */
	{ "utf8",	PG_UTF8 },		/* UTF-8; RFC2279 */
	{ "win", 	PG_WIN1251 },		/* _dirty_ alias for windows-1251 (backward compatibility) */
	{ "win1250",	PG_WIN1250 },		/* alias for Windows-1250 */
	{ "win1251",	PG_WIN1251 },		/* alias for Windows-1251 */
	{ "windows1250",PG_WIN1250 },		/* Windows-1251; Microsoft */
	{ "windows1251",PG_WIN1251 },		/* Windows-1251; Microsoft */

	{ NULL, 0 }	/* last */
};

unsigned int pg_encname_tbl_sz = \
		sizeof(pg_encname_tbl) / sizeof(pg_encname_tbl[0]) -1;

/* ----------
 * WARNING: sorted by pg_enc enum (pg_wchar.h)!
 * ----------
 */
pg_enc2name pg_enc2name_tbl[] =
{
	{ "SQL_ASCII",	PG_SQL_ASCII },
	{ "EUC_JP",	PG_EUC_JP },
	{ "EUC_CN",	PG_EUC_CN },
	{ "EUC_KR",	PG_EUC_KR },
	{ "EUC_TW",	PG_EUC_TW },
	{ "UNICODE",	PG_UTF8 },
	{ "MULE_INTERNAL",PG_MULE_INTERNAL },
	{ "LATIN1",	PG_LATIN1 },
	{ "LATIN2",	PG_LATIN2 },
	{ "LATIN3",	PG_LATIN3 },
	{ "LATIN4",	PG_LATIN4 },
	{ "LATIN5",	PG_LATIN5 },
	{ "KOI8",	PG_KOI8R },
	{ "WIN",	PG_WIN1251 },
	{ "ALT",	PG_ALT },
	{ "ISO_8859_5", PG_ISO_8859_5 },
	{ "ISO_8859_6", PG_ISO_8859_6 },
	{ "ISO_8859_7", PG_ISO_8859_7 },
	{ "ISO_8859_8", PG_ISO_8859_8 },
	{ "ISO_8859_10", PG_ISO_8859_10 },
	{ "ISO_8859_13", PG_ISO_8859_13 },
	{ "ISO_8859_14", PG_ISO_8859_14 },
	{ "ISO_8859_15", PG_ISO_8859_15 },
	{ "ISO_8859_16", PG_ISO_8859_16 },
	{ "SJIS",	PG_SJIS },
	{ "BIG5",	PG_BIG5 },
	{ "WIN1250",	PG_WIN1250 }
};

/* ----------
 * Encoding checks, for error returns -1 else encoding id
 * ----------
 */
int
pg_valid_client_encoding(const char *name)
{
	int enc;

	if ((enc = pg_char_to_encoding(name)) < 0)
		return -1;

	if (!PG_VALID_FE_ENCODING( enc))
		return -1;

	return enc;
}

int
pg_valid_server_encoding(const char *name)
{
	int enc;

	if ((enc = pg_char_to_encoding(name)) < 0)
		return -1;

	if (!PG_VALID_BE_ENCODING( enc))
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
	char	*p, *np;

	for(p=key, np=newkey; *p!='\0'; p++)
	{
		if (isalnum((unsigned char) *p))
			*np++=tolower((unsigned char) *p);
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
	unsigned int 	nel = pg_encname_tbl_sz;
	pg_encname	*base = pg_encname_tbl,
			*last = base + nel - 1,
			*position;
	int		result;
	char		buff[NAMEDATALEN],
			*key;

	if(name==NULL || *name=='\0')
		return NULL;

	if (strlen(name) > NAMEDATALEN)
	{
#ifdef FRONTEND
		fprintf(stderr, "pg_char_to_encname_struct(): encoding name too long");
		return NULL;
#else
		elog(ERROR, "pg_char_to_encname_struct(): encoding name too long");
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
		return (-1);

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
		pg_enc2name *p = &pg_enc2name_tbl[ encoding ];
		Assert( encoding == p->encoding );
		return p->name;
	}
	return "";
}

#ifndef FRONTEND
Datum
PG_encoding_to_char(PG_FUNCTION_ARGS)
{
	int32		encoding = PG_GETARG_INT32(0);
	const char	*encoding_name = pg_encoding_to_char(encoding);

	return DirectFunctionCall1(namein, CStringGetDatum(encoding_name));
}
#endif

