/*-------------------------------------------------------------------------
 *
 * chklocale.c
 *		Functions for handling locale-related info
 *
 *
 * Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/port/chklocale.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#ifdef HAVE_LANGINFO_H
#include <langinfo.h>
#endif

#include "mb/pg_wchar.h"


/*
 * This table needs to recognize all the CODESET spellings for supported
 * backend encodings, as well as frontend-only encodings where possible
 * (the latter case is currently only needed for initdb to recognize
 * error situations).  On Windows, we rely on entries for codepage
 * numbers (CPnnn).
 *
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
	{PG_EUC_JP, "CP20932"},

	{PG_EUC_CN, "EUC-CN"},
	{PG_EUC_CN, "eucCN"},
	{PG_EUC_CN, "IBM-eucCN"},
	{PG_EUC_CN, "GB2312"},
	{PG_EUC_CN, "dechanzi"},
	{PG_EUC_CN, "CP20936"},

	{PG_EUC_KR, "EUC-KR"},
	{PG_EUC_KR, "eucKR"},
	{PG_EUC_KR, "IBM-eucKR"},
	{PG_EUC_KR, "deckorean"},
	{PG_EUC_KR, "5601"},
	{PG_EUC_KR, "CP51949"},

	{PG_EUC_TW, "EUC-TW"},
	{PG_EUC_TW, "eucTW"},
	{PG_EUC_TW, "IBM-eucTW"},
	{PG_EUC_TW, "cns11643"},
	/* No codepage for EUC-TW ? */

	{PG_UTF8, "UTF-8"},
	{PG_UTF8, "utf8"},
	{PG_UTF8, "CP65001"},

	{PG_LATIN1, "ISO-8859-1"},
	{PG_LATIN1, "ISO8859-1"},
	{PG_LATIN1, "iso88591"},
	{PG_LATIN1, "CP28591"},

	{PG_LATIN2, "ISO-8859-2"},
	{PG_LATIN2, "ISO8859-2"},
	{PG_LATIN2, "iso88592"},
	{PG_LATIN2, "CP28592"},

	{PG_LATIN3, "ISO-8859-3"},
	{PG_LATIN3, "ISO8859-3"},
	{PG_LATIN3, "iso88593"},
	{PG_LATIN3, "CP28593"},

	{PG_LATIN4, "ISO-8859-4"},
	{PG_LATIN4, "ISO8859-4"},
	{PG_LATIN4, "iso88594"},
	{PG_LATIN4, "CP28594"},

	{PG_LATIN5, "ISO-8859-9"},
	{PG_LATIN5, "ISO8859-9"},
	{PG_LATIN5, "iso88599"},
	{PG_LATIN5, "CP28599"},

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
	{PG_LATIN9, "CP28605"},

	{PG_LATIN10, "ISO-8859-16"},
	{PG_LATIN10, "ISO8859-16"},
	{PG_LATIN10, "iso885916"},

	{PG_KOI8R, "KOI8-R"},
	{PG_KOI8R, "CP20866"},

	{PG_KOI8U, "KOI8-U"},
	{PG_KOI8U, "CP21866"},

	{PG_WIN866, "CP866"},
	{PG_WIN874, "CP874"},
	{PG_WIN1250, "CP1250"},
	{PG_WIN1251, "CP1251"},
	{PG_WIN1251, "ansi-1251"},
	{PG_WIN1252, "CP1252"},
	{PG_WIN1253, "CP1253"},
	{PG_WIN1254, "CP1254"},
	{PG_WIN1255, "CP1255"},
	{PG_WIN1256, "CP1256"},
	{PG_WIN1257, "CP1257"},
	{PG_WIN1258, "CP1258"},

	{PG_ISO_8859_5, "ISO-8859-5"},
	{PG_ISO_8859_5, "ISO8859-5"},
	{PG_ISO_8859_5, "iso88595"},
	{PG_ISO_8859_5, "CP28595"},

	{PG_ISO_8859_6, "ISO-8859-6"},
	{PG_ISO_8859_6, "ISO8859-6"},
	{PG_ISO_8859_6, "iso88596"},
	{PG_ISO_8859_6, "CP28596"},

	{PG_ISO_8859_7, "ISO-8859-7"},
	{PG_ISO_8859_7, "ISO8859-7"},
	{PG_ISO_8859_7, "iso88597"},
	{PG_ISO_8859_7, "CP28597"},

	{PG_ISO_8859_8, "ISO-8859-8"},
	{PG_ISO_8859_8, "ISO8859-8"},
	{PG_ISO_8859_8, "iso88598"},
	{PG_ISO_8859_8, "CP28598"},

	{PG_SJIS, "SJIS"},
	{PG_SJIS, "PCK"},
	{PG_SJIS, "CP932"},
	{PG_SJIS, "SHIFT_JIS"},

	{PG_BIG5, "BIG5"},
	{PG_BIG5, "BIG5HKSCS"},
	{PG_BIG5, "Big5-HKSCS"},
	{PG_BIG5, "CP950"},

	{PG_GBK, "GBK"},
	{PG_GBK, "CP936"},

	{PG_UHC, "UHC"},
	{PG_UHC, "CP949"},

	{PG_JOHAB, "JOHAB"},
	{PG_JOHAB, "CP1361"},

	{PG_GB18030, "GB18030"},
	{PG_GB18030, "CP54936"},

	{PG_SHIFT_JIS_2004, "SJIS_2004"},

	{PG_SQL_ASCII, "US-ASCII"},

	{PG_SQL_ASCII, NULL}		/* end marker */
};

#ifdef WIN32
/*
 * On Windows, use CP<code page number> instead of the nl_langinfo() result
 *
 * This routine uses GetLocaleInfoEx() to parse short locale names like
 * "de-DE", "fr-FR", etc.  If those cannot be parsed correctly process falls
 * back to the pre-VS-2010 manual parsing done with using
 * <Language>_<Country>.<CodePage> as a base.
 *
 * Returns a malloc()'d string for the caller to free.
 */
static char *
win32_langinfo(const char *ctype)
{
	char	   *r = NULL;
	char	   *codepage;

#if defined(_MSC_VER)
	uint32		cp;
	WCHAR		wctype[LOCALE_NAME_MAX_LENGTH];

	memset(wctype, 0, sizeof(wctype));
	MultiByteToWideChar(CP_ACP, 0, ctype, -1, wctype, LOCALE_NAME_MAX_LENGTH);

	if (GetLocaleInfoEx(wctype,
						LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
						(LPWSTR) &cp, sizeof(cp) / sizeof(WCHAR)) > 0)
	{
		r = malloc(16);			/* excess */
		if (r != NULL)
		{
			/*
			 * If the return value is CP_ACP that means no ANSI code page is
			 * available, so only Unicode can be used for the locale.
			 */
			if (cp == CP_ACP)
				strcpy(r, "utf8");
			else
				sprintf(r, "CP%u", cp);
		}
	}
	else
#endif
	{
		/*
		 * Locale format on Win32 is <Language>_<Country>.<CodePage>.  For
		 * example, English_United States.1252.  If we see digits after the
		 * last dot, assume it's a codepage number.  Otherwise, we might be
		 * dealing with a Unix-style locale string; Windows' setlocale() will
		 * take those even though GetLocaleInfoEx() won't, so we end up here.
		 * In that case, just return what's after the last dot and hope we can
		 * find it in our table.
		 */
		codepage = strrchr(ctype, '.');
		if (codepage != NULL)
		{
			size_t		ln;

			codepage++;
			ln = strlen(codepage);
			r = malloc(ln + 3);
			if (r != NULL)
			{
				if (strspn(codepage, "0123456789") == ln)
					sprintf(r, "CP%s", codepage);
				else
					strcpy(r, codepage);
			}
		}
	}

	return r;
}

#ifndef FRONTEND
/*
 * Given a Windows code page identifier, find the corresponding PostgreSQL
 * encoding.  Issue a warning and return -1 if none found.
 */
int
pg_codepage_to_encoding(UINT cp)
{
	char		sys[16];
	int			i;

	sprintf(sys, "CP%u", cp);

	/* Check the table */
	for (i = 0; encoding_match_list[i].system_enc_name; i++)
		if (pg_strcasecmp(sys, encoding_match_list[i].system_enc_name) == 0)
			return encoding_match_list[i].pg_enc_code;

	ereport(WARNING,
			(errmsg("could not determine encoding for codeset \"%s\"", sys)));

	return -1;
}
#endif
#endif							/* WIN32 */

#if (defined(HAVE_LANGINFO_H) && defined(CODESET)) || defined(WIN32)

/*
 * Given a setting for LC_CTYPE, return the Postgres ID of the associated
 * encoding, if we can determine it.  Return -1 if we can't determine it.
 *
 * Pass in NULL to get the encoding for the current locale setting.
 * Pass "" to get the encoding selected by the server's environment.
 *
 * If the result is PG_SQL_ASCII, callers should treat it as being compatible
 * with any desired encoding.
 *
 * If running in the backend and write_message is false, this function must
 * cope with the possibility that elog() and palloc() are not yet usable.
 */
int
pg_get_encoding_from_locale(const char *ctype, bool write_message)
{
	char	   *sys;
	int			i;

	/* Get the CODESET property, and also LC_CTYPE if not passed in */
	if (ctype)
	{
		char	   *save;
		char	   *name;

		/* If locale is C or POSIX, we can allow all encodings */
		if (pg_strcasecmp(ctype, "C") == 0 ||
			pg_strcasecmp(ctype, "POSIX") == 0)
			return PG_SQL_ASCII;

		save = setlocale(LC_CTYPE, NULL);
		if (!save)
			return -1;			/* setlocale() broken? */
		/* must copy result, or it might change after setlocale */
		save = strdup(save);
		if (!save)
			return -1;			/* out of memory; unlikely */

		name = setlocale(LC_CTYPE, ctype);
		if (!name)
		{
			free(save);
			return -1;			/* bogus ctype passed in? */
		}

#ifndef WIN32
		sys = nl_langinfo(CODESET);
		if (sys)
			sys = strdup(sys);
#else
		sys = win32_langinfo(name);
#endif

		setlocale(LC_CTYPE, save);
		free(save);
	}
	else
	{
		/* much easier... */
		ctype = setlocale(LC_CTYPE, NULL);
		if (!ctype)
			return -1;			/* setlocale() broken? */

		/* If locale is C or POSIX, we can allow all encodings */
		if (pg_strcasecmp(ctype, "C") == 0 ||
			pg_strcasecmp(ctype, "POSIX") == 0)
			return PG_SQL_ASCII;

#ifndef WIN32
		sys = nl_langinfo(CODESET);
		if (sys)
			sys = strdup(sys);
#else
		sys = win32_langinfo(ctype);
#endif
	}

	if (!sys)
		return -1;				/* out of memory; unlikely */

	/* Check the table */
	for (i = 0; encoding_match_list[i].system_enc_name; i++)
	{
		if (pg_strcasecmp(sys, encoding_match_list[i].system_enc_name) == 0)
		{
			free(sys);
			return encoding_match_list[i].pg_enc_code;
		}
	}

	/* Special-case kluges for particular platforms go here */

#ifdef __darwin__

	/*
	 * Current macOS has many locales that report an empty string for CODESET,
	 * but they all seem to actually use UTF-8.
	 */
	if (strlen(sys) == 0)
	{
		free(sys);
		return PG_UTF8;
	}
#endif

	/*
	 * We print a warning if we got a CODESET string but couldn't recognize
	 * it.  This means we need another entry in the table.
	 */
	if (write_message)
	{
#ifdef FRONTEND
		fprintf(stderr, _("could not determine encoding for locale \"%s\": codeset is \"%s\""),
				ctype, sys);
		/* keep newline separate so there's only one translatable string */
		fputc('\n', stderr);
#else
		ereport(WARNING,
				(errmsg("could not determine encoding for locale \"%s\": codeset is \"%s\"",
						ctype, sys)));
#endif
	}

	free(sys);
	return -1;
}
#else							/* (HAVE_LANGINFO_H && CODESET) || WIN32 */

/*
 * stub if no multi-language platform support
 *
 * Note: we could return -1 here, but that would have the effect of
 * forcing users to specify an encoding to initdb on such platforms.
 * It seems better to silently default to SQL_ASCII.
 */
int
pg_get_encoding_from_locale(const char *ctype, bool write_message)
{
	return PG_SQL_ASCII;
}

#endif							/* (HAVE_LANGINFO_H && CODESET) || WIN32 */
