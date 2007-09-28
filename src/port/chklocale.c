/*-------------------------------------------------------------------------
 *
 * chklocale.c
 *		Functions for handling locale-related info
 *
 *
 * Copyright (c) 1996-2007, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/port/chklocale.c,v 1.1 2007/09/28 22:25:49 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <locale.h>
#ifdef HAVE_LANGINFO_H
#include <langinfo.h>
#endif

#include "mb/pg_wchar.h"


#if defined(HAVE_LANGINFO_H) && defined(CODESET)

/*
 * This table needs to recognize all the CODESET spellings for supported
 * backend encodings.  We don't need to handle frontend-only encodings.
 * Note that we search the table with pg_strcasecmp(), so variant
 * capitalizations don't need their own entries.
 */
struct encoding_match
{
	enum pg_enc pg_enc_code;
	const char *system_enc_name;
};

static const struct encoding_match encoding_match_list[] = {
	{PG_EUC_JP, "EUC-JP"},
	{PG_EUC_JP, "eucJP"},
	{PG_EUC_JP, "IBM-eucJP"},
	{PG_EUC_JP, "sdeckanji"},

	{PG_EUC_CN, "EUC-CN"},
	{PG_EUC_CN, "eucCN"},
	{PG_EUC_CN, "IBM-eucCN"},
	{PG_EUC_CN, "GB2312"},
	{PG_EUC_CN, "dechanzi"},

	{PG_EUC_KR, "EUC-KR"},
	{PG_EUC_KR, "eucKR"},
	{PG_EUC_KR, "IBM-eucKR"},
	{PG_EUC_KR, "deckorean"},
	{PG_EUC_KR, "5601"},

	{PG_EUC_TW, "EUC-TW"},
	{PG_EUC_TW, "eucTW"},
	{PG_EUC_TW, "IBM-eucTW"},
	{PG_EUC_TW, "cns11643"},

	{PG_UTF8, "UTF-8"},
	{PG_UTF8, "utf8"},

	{PG_LATIN1, "ISO-8859-1"},
	{PG_LATIN1, "ISO8859-1"},
	{PG_LATIN1, "iso88591"},

	{PG_LATIN2, "ISO-8859-2"},
	{PG_LATIN2, "ISO8859-2"},
	{PG_LATIN2, "iso88592"},

	{PG_LATIN3, "ISO-8859-3"},
	{PG_LATIN3, "ISO8859-3"},
	{PG_LATIN3, "iso88593"},

	{PG_LATIN4, "ISO-8859-4"},
	{PG_LATIN4, "ISO8859-4"},
	{PG_LATIN4, "iso88594"},

	{PG_LATIN5, "ISO-8859-9"},
	{PG_LATIN5, "ISO8859-9"},
	{PG_LATIN5, "iso88599"},

	{PG_LATIN6, "ISO-8859-10"},
	{PG_LATIN6, "ISO8859-10"},
	{PG_LATIN6, "iso885910"},

	{PG_LATIN7, "ISO-8859-13"},
	{PG_LATIN7, "ISO8859-13"},
	{PG_LATIN7, "iso885913"},

	{PG_LATIN8, "ISO-8859-14"},
	{PG_LATIN8, "ISO8859-14"},
	{PG_LATIN8, "iso885914"},

	{PG_LATIN9, "ISO-8859-15"},
	{PG_LATIN9, "ISO8859-15"},
	{PG_LATIN9, "iso885915"},

	{PG_LATIN10, "ISO-8859-16"},
	{PG_LATIN10, "ISO8859-16"},
	{PG_LATIN10, "iso885916"},

	{PG_KOI8R, "KOI8-R"},

	{PG_WIN1252, "CP1252"},
	{PG_WIN1253, "CP1253"},
	{PG_WIN1254, "CP1254"},
	{PG_WIN1255, "CP1255"},
	{PG_WIN1256, "CP1256"},
	{PG_WIN1257, "CP1257"},
	{PG_WIN1258, "CP1258"},
#ifdef NOT_VERIFIED
	{PG_WIN874, "???"},
#endif
	{PG_WIN1251, "CP1251"},
	{PG_WIN866, "CP866"},

	{PG_ISO_8859_5, "ISO-8859-5"},
	{PG_ISO_8859_5, "ISO8859-5"},
	{PG_ISO_8859_5, "iso88595"},

	{PG_ISO_8859_6, "ISO-8859-6"},
	{PG_ISO_8859_6, "ISO8859-6"},
	{PG_ISO_8859_6, "iso88596"},

	{PG_ISO_8859_7, "ISO-8859-7"},
	{PG_ISO_8859_7, "ISO8859-7"},
	{PG_ISO_8859_7, "iso88597"},

	{PG_ISO_8859_8, "ISO-8859-8"},
	{PG_ISO_8859_8, "ISO8859-8"},
	{PG_ISO_8859_8, "iso88598"},

	{PG_SQL_ASCII, NULL}		/* end marker */
};


/*
 * Given a setting for LC_CTYPE, return the Postgres ID of the associated
 * encoding, if we can determine it.
 *
 * Pass in NULL to get the encoding for the current locale setting.
 *
 * If the result is PG_SQL_ASCII, callers should treat it as being compatible
 * with any desired encoding.  We return this if the locale is C/POSIX or we
 * can't determine the encoding.
 */
int
pg_get_encoding_from_locale(const char *ctype)
{
	char	   *sys;
	int			i;

	if (ctype)
	{
		char	   *save;

		save = setlocale(LC_CTYPE, NULL);
		if (!save)
			return PG_SQL_ASCII;		/* setlocale() broken? */
		/* must copy result, or it might change after setlocale */
		save = strdup(save);
		if (!save)
			return PG_SQL_ASCII;		/* out of memory; unlikely */

		if (!setlocale(LC_CTYPE, ctype))
		{
			free(save);
			return PG_SQL_ASCII;		/* bogus ctype passed in? */
		}

		sys = nl_langinfo(CODESET);
		if (sys)
			sys = strdup(sys);

		setlocale(LC_CTYPE, save);
		free(save);
	}
	else
	{
		/* much easier... */
		ctype = setlocale(LC_CTYPE, NULL);
		if (!ctype)
			return PG_SQL_ASCII;		/* setlocale() broken? */
		sys = nl_langinfo(CODESET);
		if (sys)
			sys = strdup(sys);
	}

	if (!sys)
		return PG_SQL_ASCII;		/* out of memory; unlikely */

	if (pg_strcasecmp(ctype, "C") == 0 || pg_strcasecmp(ctype, "POSIX") == 0)
	{
		free(sys);
		return PG_SQL_ASCII;
	}

	for (i = 0; encoding_match_list[i].system_enc_name; i++)
	{
		if (pg_strcasecmp(sys, encoding_match_list[i].system_enc_name) == 0)
		{
			free(sys);
			return encoding_match_list[i].pg_enc_code;
		}
	}

	/*
	 * We print a warning if we got a CODESET string but couldn't recognize
	 * it.  This means we need another entry in the table.
	 */
#ifdef FRONTEND
	fprintf(stderr, _("could not determine encoding for locale \"%s\": codeset is \"%s\""),
			ctype, sys);
	/* keep newline separate so there's only one translatable string */
	fputc('\n', stderr);
#else
	ereport(WARNING,
			(errmsg("could not determine encoding for locale \"%s\": codeset is \"%s\"",
					ctype, sys),
			 errdetail("Please report this to <pgsql-bugs@postgresql.org>.")));
#endif

	free(sys);
	return PG_SQL_ASCII;
}

#else /* !(HAVE_LANGINFO_H && CODESET) */

/*
 * stub if no platform support
 */
int
pg_get_encoding_from_locale(const char *ctype)
{
	return PG_SQL_ASCII;
}

#endif /* HAVE_LANGINFO_H && CODESET */
