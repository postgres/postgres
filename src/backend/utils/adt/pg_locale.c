/*-----------------------------------------------------------------------
 *
 * PostgreSQL locale utilities
 *
 * Portions Copyright (c) 2002-2014, PostgreSQL Global Development Group
 *
 * src/backend/utils/adt/pg_locale.c
 *
 *-----------------------------------------------------------------------
 */

/*----------
 * Here is how the locale stuff is handled: LC_COLLATE and LC_CTYPE
 * are fixed at CREATE DATABASE time, stored in pg_database, and cannot
 * be changed. Thus, the effects of strcoll(), strxfrm(), isupper(),
 * toupper(), etc. are always in the same fixed locale.
 *
 * LC_MESSAGES is settable at run time and will take effect
 * immediately.
 *
 * The other categories, LC_MONETARY, LC_NUMERIC, and LC_TIME are also
 * settable at run-time.  However, we don't actually set those locale
 * categories permanently.  This would have bizarre effects like no
 * longer accepting standard floating-point literals in some locales.
 * Instead, we only set the locales briefly when needed, cache the
 * required information obtained from localeconv(), and set them back.
 * The cached information is only used by the formatting functions
 * (to_char, etc.) and the money type.  For the user, this should all be
 * transparent.
 *
 * !!! NOW HEAR THIS !!!
 *
 * We've been bitten repeatedly by this bug, so let's try to keep it in
 * mind in future: on some platforms, the locale functions return pointers
 * to static data that will be overwritten by any later locale function.
 * Thus, for example, the obvious-looking sequence
 *			save = setlocale(category, NULL);
 *			if (!setlocale(category, value))
 *				fail = true;
 *			setlocale(category, save);
 * DOES NOT WORK RELIABLY: on some platforms the second setlocale() call
 * will change the memory save is pointing at.  To do this sort of thing
 * safely, you *must* pstrdup what setlocale returns the first time.
 *
 * FYI, The Open Group locale standard is defined here:
 *
 *	http://www.opengroup.org/onlinepubs/009695399/basedefs/xbd_chap07.html
 *----------
 */


#include "postgres.h"

#include <locale.h>
#include <time.h>

#include "access/htup_details.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_control.h"
#include "mb/pg_wchar.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/syscache.h"

#ifdef WIN32
/*
 * This Windows file defines StrNCpy. We don't need it here, so we undefine
 * it to keep the compiler quiet, and undefine it again after the file is
 * included, so we don't accidentally use theirs.
 */
#undef StrNCpy
#include <shlwapi.h>
#ifdef StrNCpy
#undef STrNCpy
#endif
#endif

#define		MAX_L10N_DATA		80


/* GUC settings */
char	   *locale_messages;
char	   *locale_monetary;
char	   *locale_numeric;
char	   *locale_time;

/* lc_time localization cache */
char	   *localized_abbrev_days[7];
char	   *localized_full_days[7];
char	   *localized_abbrev_months[12];
char	   *localized_full_months[12];

/* indicates whether locale information cache is valid */
static bool CurrentLocaleConvValid = false;
static bool CurrentLCTimeValid = false;

/* Environment variable storage area */

#define LC_ENV_BUFSIZE (NAMEDATALEN + 20)

static char lc_collate_envbuf[LC_ENV_BUFSIZE];
static char lc_ctype_envbuf[LC_ENV_BUFSIZE];

#ifdef LC_MESSAGES
static char lc_messages_envbuf[LC_ENV_BUFSIZE];
#endif
static char lc_monetary_envbuf[LC_ENV_BUFSIZE];
static char lc_numeric_envbuf[LC_ENV_BUFSIZE];
static char lc_time_envbuf[LC_ENV_BUFSIZE];

/* Cache for collation-related knowledge */

typedef struct
{
	Oid			collid;			/* hash key: pg_collation OID */
	bool		collate_is_c;	/* is collation's LC_COLLATE C? */
	bool		ctype_is_c;		/* is collation's LC_CTYPE C? */
	bool		flags_valid;	/* true if above flags are valid */
	pg_locale_t locale;			/* locale_t struct, or 0 if not valid */
} collation_cache_entry;

static HTAB *collation_cache = NULL;


#if defined(WIN32) && defined(LC_MESSAGES)
static char *IsoLocaleName(const char *);		/* MSVC specific */
#endif


/*
 * pg_perm_setlocale
 *
 * This wraps the libc function setlocale(), with two additions.  First, when
 * changing LC_CTYPE, update gettext's encoding for the current message
 * domain.  GNU gettext automatically tracks LC_CTYPE on most platforms, but
 * not on Windows.  Second, if the operation is successful, the corresponding
 * LC_XXX environment variable is set to match.  By setting the environment
 * variable, we ensure that any subsequent use of setlocale(..., "") will
 * preserve the settings made through this routine.  Of course, LC_ALL must
 * also be unset to fully ensure that, but that has to be done elsewhere after
 * all the individual LC_XXX variables have been set correctly.  (Thank you
 * Perl for making this kluge necessary.)
 */
char *
pg_perm_setlocale(int category, const char *locale)
{
	char	   *result;
	const char *envvar;
	char	   *envbuf;

#ifndef WIN32
	result = setlocale(category, locale);
#else

	/*
	 * On Windows, setlocale(LC_MESSAGES) does not work, so just assume that
	 * the given value is good and set it in the environment variables. We
	 * must ignore attempts to set to "", which means "keep using the old
	 * environment value".
	 */
#ifdef LC_MESSAGES
	if (category == LC_MESSAGES)
	{
		result = (char *) locale;
		if (locale == NULL || locale[0] == '\0')
			return result;
	}
	else
#endif
		result = setlocale(category, locale);
#endif   /* WIN32 */

	if (result == NULL)
		return result;			/* fall out immediately on failure */

	/*
	 * Use the right encoding in translated messages.  Under ENABLE_NLS, let
	 * pg_bind_textdomain_codeset() figure it out.  Under !ENABLE_NLS, message
	 * format strings are ASCII, but database-encoding strings may enter the
	 * message via %s.  This makes the overall message encoding equal to the
	 * database encoding.
	 */
	if (category == LC_CTYPE)
	{
#ifdef ENABLE_NLS
		SetMessageEncoding(pg_bind_textdomain_codeset(textdomain(NULL)));
#else
		SetMessageEncoding(GetDatabaseEncoding());
#endif
	}

	switch (category)
	{
		case LC_COLLATE:
			envvar = "LC_COLLATE";
			envbuf = lc_collate_envbuf;
			break;
		case LC_CTYPE:
			envvar = "LC_CTYPE";
			envbuf = lc_ctype_envbuf;
			break;
#ifdef LC_MESSAGES
		case LC_MESSAGES:
			envvar = "LC_MESSAGES";
			envbuf = lc_messages_envbuf;
#ifdef WIN32
			result = IsoLocaleName(locale);
			if (result == NULL)
				result = (char *) locale;
#endif   /* WIN32 */
			break;
#endif   /* LC_MESSAGES */
		case LC_MONETARY:
			envvar = "LC_MONETARY";
			envbuf = lc_monetary_envbuf;
			break;
		case LC_NUMERIC:
			envvar = "LC_NUMERIC";
			envbuf = lc_numeric_envbuf;
			break;
		case LC_TIME:
			envvar = "LC_TIME";
			envbuf = lc_time_envbuf;
			break;
		default:
			elog(FATAL, "unrecognized LC category: %d", category);
			envvar = NULL;		/* keep compiler quiet */
			envbuf = NULL;
			return NULL;
	}

	snprintf(envbuf, LC_ENV_BUFSIZE - 1, "%s=%s", envvar, result);

	if (putenv(envbuf))
		return NULL;

	return result;
}


/*
 * Is the locale name valid for the locale category?
 *
 * If successful, and canonname isn't NULL, a palloc'd copy of the locale's
 * canonical name is stored there.  This is especially useful for figuring out
 * what locale name "" means (ie, the server environment value).  (Actually,
 * it seems that on most implementations that's the only thing it's good for;
 * we could wish that setlocale gave back a canonically spelled version of
 * the locale name, but typically it doesn't.)
 */
bool
check_locale(int category, const char *locale, char **canonname)
{
	char	   *save;
	char	   *res;

	if (canonname)
		*canonname = NULL;		/* in case of failure */

	save = setlocale(category, NULL);
	if (!save)
		return false;			/* won't happen, we hope */

	/* save may be pointing at a modifiable scratch variable, see above. */
	save = pstrdup(save);

	/* set the locale with setlocale, to see if it accepts it. */
	res = setlocale(category, locale);

	/* save canonical name if requested. */
	if (res && canonname)
		*canonname = pstrdup(res);

	/* restore old value. */
	if (!setlocale(category, save))
		elog(WARNING, "failed to restore old locale \"%s\"", save);
	pfree(save);

	return (res != NULL);
}


/*
 * GUC check/assign hooks
 *
 * For most locale categories, the assign hook doesn't actually set the locale
 * permanently, just reset flags so that the next use will cache the
 * appropriate values.  (See explanation at the top of this file.)
 *
 * Note: we accept value = "" as selecting the postmaster's environment
 * value, whatever it was (so long as the environment setting is legal).
 * This will have been locked down by an earlier call to pg_perm_setlocale.
 */
bool
check_locale_monetary(char **newval, void **extra, GucSource source)
{
	return check_locale(LC_MONETARY, *newval, NULL);
}

void
assign_locale_monetary(const char *newval, void *extra)
{
	CurrentLocaleConvValid = false;
}

bool
check_locale_numeric(char **newval, void **extra, GucSource source)
{
	return check_locale(LC_NUMERIC, *newval, NULL);
}

void
assign_locale_numeric(const char *newval, void *extra)
{
	CurrentLocaleConvValid = false;
}

bool
check_locale_time(char **newval, void **extra, GucSource source)
{
	return check_locale(LC_TIME, *newval, NULL);
}

void
assign_locale_time(const char *newval, void *extra)
{
	CurrentLCTimeValid = false;
}

/*
 * We allow LC_MESSAGES to actually be set globally.
 *
 * Note: we normally disallow value = "" because it wouldn't have consistent
 * semantics (it'd effectively just use the previous value).  However, this
 * is the value passed for PGC_S_DEFAULT, so don't complain in that case,
 * not even if the attempted setting fails due to invalid environment value.
 * The idea there is just to accept the environment setting *if possible*
 * during startup, until we can read the proper value from postgresql.conf.
 */
bool
check_locale_messages(char **newval, void **extra, GucSource source)
{
	if (**newval == '\0')
	{
		if (source == PGC_S_DEFAULT)
			return true;
		else
			return false;
	}

	/*
	 * LC_MESSAGES category does not exist everywhere, but accept it anyway
	 *
	 * On Windows, we can't even check the value, so accept blindly
	 */
#if defined(LC_MESSAGES) && !defined(WIN32)
	return check_locale(LC_MESSAGES, *newval, NULL);
#else
	return true;
#endif
}

void
assign_locale_messages(const char *newval, void *extra)
{
	/*
	 * LC_MESSAGES category does not exist everywhere, but accept it anyway.
	 * We ignore failure, as per comment above.
	 */
#ifdef LC_MESSAGES
	(void) pg_perm_setlocale(LC_MESSAGES, newval);
#endif
}


/*
 * Frees the malloced content of a struct lconv.  (But not the struct
 * itself.)
 */
static void
free_struct_lconv(struct lconv * s)
{
	if (s == NULL)
		return;

	if (s->currency_symbol)
		free(s->currency_symbol);
	if (s->decimal_point)
		free(s->decimal_point);
	if (s->grouping)
		free(s->grouping);
	if (s->thousands_sep)
		free(s->thousands_sep);
	if (s->int_curr_symbol)
		free(s->int_curr_symbol);
	if (s->mon_decimal_point)
		free(s->mon_decimal_point);
	if (s->mon_grouping)
		free(s->mon_grouping);
	if (s->mon_thousands_sep)
		free(s->mon_thousands_sep);
	if (s->negative_sign)
		free(s->negative_sign);
	if (s->positive_sign)
		free(s->positive_sign);
}


/*
 * Return a strdup'ed string converted from the specified encoding to the
 * database encoding.
 */
static char *
db_encoding_strdup(int encoding, const char *str)
{
	char	   *pstr;
	char	   *mstr;

	/* convert the string to the database encoding */
	pstr = pg_any_to_server(str, strlen(str), encoding);
	mstr = strdup(pstr);
	if (pstr != str)
		pfree(pstr);

	return mstr;
}


/*
 * Return the POSIX lconv struct (contains number/money formatting
 * information) with locale information for all categories.
 */
struct lconv *
PGLC_localeconv(void)
{
	static struct lconv CurrentLocaleConv;
	struct lconv *extlconv;
	char	   *save_lc_monetary;
	char	   *save_lc_numeric;
	char	   *decimal_point;
	char	   *grouping;
	char	   *thousands_sep;
	int			encoding;

#ifdef WIN32
	char	   *save_lc_ctype;
#endif

	/* Did we do it already? */
	if (CurrentLocaleConvValid)
		return &CurrentLocaleConv;

	free_struct_lconv(&CurrentLocaleConv);

	/* Save user's values of monetary and numeric locales */
	save_lc_monetary = setlocale(LC_MONETARY, NULL);
	if (save_lc_monetary)
		save_lc_monetary = pstrdup(save_lc_monetary);

	save_lc_numeric = setlocale(LC_NUMERIC, NULL);
	if (save_lc_numeric)
		save_lc_numeric = pstrdup(save_lc_numeric);

#ifdef WIN32

	/*
	 * Ideally, monetary and numeric local symbols could be returned in any
	 * server encoding.  Unfortunately, the WIN32 API does not allow
	 * setlocale() to return values in a codepage/CTYPE that uses more than
	 * two bytes per character, like UTF-8:
	 *
	 * http://msdn.microsoft.com/en-us/library/x99tb11d.aspx
	 *
	 * Evidently, LC_CTYPE allows us to control the encoding used for strings
	 * returned by localeconv().  The Open Group standard, mentioned at the
	 * top of this C file, doesn't explicitly state this.
	 *
	 * Therefore, we set LC_CTYPE to match LC_NUMERIC or LC_MONETARY (which
	 * cannot be UTF8), call localeconv(), and then convert from the
	 * numeric/monitary LC_CTYPE to the server encoding.  One example use of
	 * this is for the Euro symbol.
	 *
	 * Perhaps someday we will use GetLocaleInfoW() which returns values in
	 * UTF16 and convert from that.
	 */

	/* save user's value of ctype locale */
	save_lc_ctype = setlocale(LC_CTYPE, NULL);
	if (save_lc_ctype)
		save_lc_ctype = pstrdup(save_lc_ctype);

	/* use numeric to set the ctype */
	setlocale(LC_CTYPE, locale_numeric);
#endif

	/* Get formatting information for numeric */
	setlocale(LC_NUMERIC, locale_numeric);
	extlconv = localeconv();
	encoding = pg_get_encoding_from_locale(locale_numeric, true);

	decimal_point = db_encoding_strdup(encoding, extlconv->decimal_point);
	thousands_sep = db_encoding_strdup(encoding, extlconv->thousands_sep);
	grouping = strdup(extlconv->grouping);

#ifdef WIN32
	/* use monetary to set the ctype */
	setlocale(LC_CTYPE, locale_monetary);
#endif

	/* Get formatting information for monetary */
	setlocale(LC_MONETARY, locale_monetary);
	extlconv = localeconv();
	encoding = pg_get_encoding_from_locale(locale_monetary, true);

	/*
	 * Must copy all values since restoring internal settings may overwrite
	 * localeconv()'s results.
	 */
	CurrentLocaleConv = *extlconv;
	CurrentLocaleConv.decimal_point = decimal_point;
	CurrentLocaleConv.grouping = grouping;
	CurrentLocaleConv.thousands_sep = thousands_sep;
	CurrentLocaleConv.int_curr_symbol = db_encoding_strdup(encoding, extlconv->int_curr_symbol);
	CurrentLocaleConv.currency_symbol = db_encoding_strdup(encoding, extlconv->currency_symbol);
	CurrentLocaleConv.mon_decimal_point = db_encoding_strdup(encoding, extlconv->mon_decimal_point);
	CurrentLocaleConv.mon_grouping = strdup(extlconv->mon_grouping);
	CurrentLocaleConv.mon_thousands_sep = db_encoding_strdup(encoding, extlconv->mon_thousands_sep);
	CurrentLocaleConv.negative_sign = db_encoding_strdup(encoding, extlconv->negative_sign);
	CurrentLocaleConv.positive_sign = db_encoding_strdup(encoding, extlconv->positive_sign);

	/* Try to restore internal settings */
	if (save_lc_monetary)
	{
		if (!setlocale(LC_MONETARY, save_lc_monetary))
			elog(WARNING, "failed to restore old locale");
		pfree(save_lc_monetary);
	}

	if (save_lc_numeric)
	{
		if (!setlocale(LC_NUMERIC, save_lc_numeric))
			elog(WARNING, "failed to restore old locale");
		pfree(save_lc_numeric);
	}

#ifdef WIN32
	/* Try to restore internal ctype settings */
	if (save_lc_ctype)
	{
		if (!setlocale(LC_CTYPE, save_lc_ctype))
			elog(WARNING, "failed to restore old locale");
		pfree(save_lc_ctype);
	}
#endif

	CurrentLocaleConvValid = true;
	return &CurrentLocaleConv;
}

#ifdef WIN32
/*
 * On WIN32, strftime() returns the encoding in CP_ACP (the default
 * operating system codpage for that computer), which is likely different
 * from SERVER_ENCODING.  This is especially important in Japanese versions
 * of Windows which will use SJIS encoding, which we don't support as a
 * server encoding.
 *
 * So, instead of using strftime(), use wcsftime() to return the value in
 * wide characters (internally UTF16) and then convert it to the appropriate
 * database encoding.
 *
 * Note that this only affects the calls to strftime() in this file, which are
 * used to get the locale-aware strings. Other parts of the backend use
 * pg_strftime(), which isn't locale-aware and does not need to be replaced.
 */
static size_t
strftime_win32(char *dst, size_t dstlen, const wchar_t *format, const struct tm * tm)
{
	size_t		len;
	wchar_t		wbuf[MAX_L10N_DATA];

	len = wcsftime(wbuf, MAX_L10N_DATA, format, tm);
	if (len == 0)
	{
		/*
		 * strftime call failed - return 0 with the contents of dst
		 * unspecified
		 */
		return 0;
	}

	len = WideCharToMultiByte(CP_UTF8, 0, wbuf, len, dst, dstlen, NULL, NULL);
	if (len == 0)
		elog(ERROR, "could not convert string to UTF-8: error code %lu",
			 GetLastError());

	dst[len] = '\0';
	if (GetDatabaseEncoding() != PG_UTF8)
	{
		char	   *convstr = pg_any_to_server(dst, len, PG_UTF8);

		if (convstr != dst)
		{
			strlcpy(dst, convstr, dstlen);
			len = strlen(dst);
			pfree(convstr);
		}
	}

	return len;
}

/* redefine strftime() */
#define strftime(a,b,c,d) strftime_win32(a,b,L##c,d)
#endif   /* WIN32 */


/*
 * Update the lc_time localization cache variables if needed.
 */
void
cache_locale_time(void)
{
	char	   *save_lc_time;
	time_t		timenow;
	struct tm  *timeinfo;
	char		buf[MAX_L10N_DATA];
	char	   *ptr;
	int			i;

#ifdef WIN32
	char	   *save_lc_ctype;
#endif

	/* did we do this already? */
	if (CurrentLCTimeValid)
		return;

	elog(DEBUG3, "cache_locale_time() executed; locale: \"%s\"", locale_time);

	/* save user's value of time locale */
	save_lc_time = setlocale(LC_TIME, NULL);
	if (save_lc_time)
		save_lc_time = pstrdup(save_lc_time);

#ifdef WIN32

	/*
	 * On WIN32, there is no way to get locale-specific time values in a
	 * specified locale, like we do for monetary/numeric.  We can only get
	 * CP_ACP (see strftime_win32) or UTF16.  Therefore, we get UTF16 and
	 * convert it to the database locale.  However, wcsftime() internally uses
	 * LC_CTYPE, so we set it here.  See the WIN32 comment near the top of
	 * PGLC_localeconv().
	 */

	/* save user's value of ctype locale */
	save_lc_ctype = setlocale(LC_CTYPE, NULL);
	if (save_lc_ctype)
		save_lc_ctype = pstrdup(save_lc_ctype);

	/* use lc_time to set the ctype */
	setlocale(LC_CTYPE, locale_time);
#endif

	setlocale(LC_TIME, locale_time);

	timenow = time(NULL);
	timeinfo = localtime(&timenow);

	/* localized days */
	for (i = 0; i < 7; i++)
	{
		timeinfo->tm_wday = i;
		strftime(buf, MAX_L10N_DATA, "%a", timeinfo);
		ptr = MemoryContextStrdup(TopMemoryContext, buf);
		if (localized_abbrev_days[i])
			pfree(localized_abbrev_days[i]);
		localized_abbrev_days[i] = ptr;

		strftime(buf, MAX_L10N_DATA, "%A", timeinfo);
		ptr = MemoryContextStrdup(TopMemoryContext, buf);
		if (localized_full_days[i])
			pfree(localized_full_days[i]);
		localized_full_days[i] = ptr;
	}

	/* localized months */
	for (i = 0; i < 12; i++)
	{
		timeinfo->tm_mon = i;
		timeinfo->tm_mday = 1;	/* make sure we don't have invalid date */
		strftime(buf, MAX_L10N_DATA, "%b", timeinfo);
		ptr = MemoryContextStrdup(TopMemoryContext, buf);
		if (localized_abbrev_months[i])
			pfree(localized_abbrev_months[i]);
		localized_abbrev_months[i] = ptr;

		strftime(buf, MAX_L10N_DATA, "%B", timeinfo);
		ptr = MemoryContextStrdup(TopMemoryContext, buf);
		if (localized_full_months[i])
			pfree(localized_full_months[i]);
		localized_full_months[i] = ptr;
	}

	/* try to restore internal settings */
	if (save_lc_time)
	{
		if (!setlocale(LC_TIME, save_lc_time))
			elog(WARNING, "failed to restore old locale");
		pfree(save_lc_time);
	}

#ifdef WIN32
	/* try to restore internal ctype settings */
	if (save_lc_ctype)
	{
		if (!setlocale(LC_CTYPE, save_lc_ctype))
			elog(WARNING, "failed to restore old locale");
		pfree(save_lc_ctype);
	}
#endif

	CurrentLCTimeValid = true;
}


#if defined(WIN32) && defined(LC_MESSAGES)
/*
 * Convert a Windows setlocale() argument to a Unix-style one.
 *
 * Regardless of platform, we install message catalogs under a Unix-style
 * LL[_CC][.ENCODING][@VARIANT] naming convention.  Only LC_MESSAGES settings
 * following that style will elicit localized interface strings.
 *
 * Before Visual Studio 2012 (msvcr110.dll), Windows setlocale() accepted "C"
 * (but not "c") and strings of the form <Language>[_<Country>][.<CodePage>],
 * case-insensitive.  setlocale() returns the fully-qualified form; for
 * example, setlocale("thaI") returns "Thai_Thailand.874".  Internally,
 * setlocale() and _create_locale() select a "locale identifier"[1] and store
 * it in an undocumented _locale_t field.  From that LCID, we can retrieve the
 * ISO 639 language and the ISO 3166 country.  Character encoding does not
 * matter, because the server and client encodings govern that.
 *
 * Windows Vista introduced the "locale name" concept[2], closely following
 * RFC 4646.  Locale identifiers are now deprecated.  Starting with Visual
 * Studio 2012, setlocale() accepts locale names in addition to the strings it
 * accepted historically.  It does not standardize them; setlocale("Th-tH")
 * returns "Th-tH".  setlocale(category, "") still returns a traditional
 * string.  Furthermore, msvcr110.dll changed the undocumented _locale_t
 * content to carry locale names instead of locale identifiers.
 *
 * MinGW headers declare _create_locale(), but msvcrt.dll lacks that symbol.
 * IsoLocaleName() always fails in a MinGW-built postgres.exe, so only
 * Unix-style values of the lc_messages GUC can elicit localized messages.  In
 * particular, every lc_messages setting that initdb can select automatically
 * will yield only C-locale messages.  XXX This could be fixed by running the
 * fully-qualified locale name through a lookup table.
 *
 * This function returns a pointer to a static buffer bearing the converted
 * name or NULL if conversion fails.
 *
 * [1] http://msdn.microsoft.com/en-us/library/windows/desktop/dd373763.aspx
 * [2] http://msdn.microsoft.com/en-us/library/windows/desktop/dd373814.aspx
 */
static char *
IsoLocaleName(const char *winlocname)
{
#if (_MSC_VER >= 1400)			/* VC8.0 or later */
	static char iso_lc_messages[32];
	_locale_t	loct = NULL;

	if (pg_strcasecmp("c", winlocname) == 0 ||
		pg_strcasecmp("posix", winlocname) == 0)
	{
		strcpy(iso_lc_messages, "C");
		return iso_lc_messages;
	}

	loct = _create_locale(LC_CTYPE, winlocname);
	if (loct != NULL)
	{
#if (_MSC_VER >= 1700)			/* Visual Studio 2012 or later */
		size_t		rc;
		char	   *hyphen;

		/* Locale names use only ASCII, any conversion locale suffices. */
		rc = wchar2char(iso_lc_messages, loct->locinfo->locale_name[LC_CTYPE],
						sizeof(iso_lc_messages), NULL);
		_free_locale(loct);
		if (rc == -1 || rc == sizeof(iso_lc_messages))
			return NULL;

		/*
		 * Since the message catalogs sit on a case-insensitive filesystem, we
		 * need not standardize letter case here.  So long as we do not ship
		 * message catalogs for which it would matter, we also need not
		 * translate the script/variant portion, e.g. uz-Cyrl-UZ to
		 * uz_UZ@cyrillic.  Simply replace the hyphen with an underscore.
		 *
		 * Note that the locale name can be less-specific than the value we
		 * would derive under earlier Visual Studio releases.  For example,
		 * French_France.1252 yields just "fr".  This does not affect any of
		 * the country-specific message catalogs available as of this writing
		 * (pt_BR, zh_CN, zh_TW).
		 */
		hyphen = strchr(iso_lc_messages, '-');
		if (hyphen)
			*hyphen = '_';
#else
		char		isolang[32],
					isocrty[32];
		LCID		lcid;

		lcid = loct->locinfo->lc_handle[LC_CTYPE];
		if (lcid == 0)
			lcid = MAKELCID(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), SORT_DEFAULT);
		_free_locale(loct);

		if (!GetLocaleInfoA(lcid, LOCALE_SISO639LANGNAME, isolang, sizeof(isolang)))
			return NULL;
		if (!GetLocaleInfoA(lcid, LOCALE_SISO3166CTRYNAME, isocrty, sizeof(isocrty)))
			return NULL;
		snprintf(iso_lc_messages, sizeof(iso_lc_messages) - 1, "%s_%s", isolang, isocrty);
#endif
		return iso_lc_messages;
	}
	return NULL;
#else
	return NULL;				/* Not supported on this version of msvc/mingw */
#endif   /* _MSC_VER >= 1400 */
}
#endif   /* WIN32 && LC_MESSAGES */


/*
 * Cache mechanism for collation information.
 *
 * We cache two flags: whether the collation's LC_COLLATE or LC_CTYPE is C
 * (or POSIX), so we can optimize a few code paths in various places.
 * For the built-in C and POSIX collations, we can know that without even
 * doing a cache lookup, but we want to support aliases for C/POSIX too.
 * For the "default" collation, there are separate static cache variables,
 * since consulting the pg_collation catalog doesn't tell us what we need.
 *
 * Also, if a pg_locale_t has been requested for a collation, we cache that
 * for the life of a backend.
 *
 * Note that some code relies on the flags not reporting false negatives
 * (that is, saying it's not C when it is).  For example, char2wchar()
 * could fail if the locale is C, so str_tolower() shouldn't call it
 * in that case.
 *
 * Note that we currently lack any way to flush the cache.  Since we don't
 * support ALTER COLLATION, this is OK.  The worst case is that someone
 * drops a collation, and a useless cache entry hangs around in existing
 * backends.
 */

static collation_cache_entry *
lookup_collation_cache(Oid collation, bool set_flags)
{
	collation_cache_entry *cache_entry;
	bool		found;

	Assert(OidIsValid(collation));
	Assert(collation != DEFAULT_COLLATION_OID);

	if (collation_cache == NULL)
	{
		/* First time through, initialize the hash table */
		HASHCTL		ctl;

		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(collation_cache_entry);
		ctl.hash = oid_hash;
		collation_cache = hash_create("Collation cache", 100, &ctl,
									  HASH_ELEM | HASH_FUNCTION);
	}

	cache_entry = hash_search(collation_cache, &collation, HASH_ENTER, &found);
	if (!found)
	{
		/*
		 * Make sure cache entry is marked invalid, in case we fail before
		 * setting things.
		 */
		cache_entry->flags_valid = false;
		cache_entry->locale = 0;
	}

	if (set_flags && !cache_entry->flags_valid)
	{
		/* Attempt to set the flags */
		HeapTuple	tp;
		Form_pg_collation collform;
		const char *collcollate;
		const char *collctype;

		tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collation));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for collation %u", collation);
		collform = (Form_pg_collation) GETSTRUCT(tp);

		collcollate = NameStr(collform->collcollate);
		collctype = NameStr(collform->collctype);

		cache_entry->collate_is_c = ((strcmp(collcollate, "C") == 0) ||
									 (strcmp(collcollate, "POSIX") == 0));
		cache_entry->ctype_is_c = ((strcmp(collctype, "C") == 0) ||
								   (strcmp(collctype, "POSIX") == 0));

		cache_entry->flags_valid = true;

		ReleaseSysCache(tp);
	}

	return cache_entry;
}


/*
 * Detect whether collation's LC_COLLATE property is C
 */
bool
lc_collate_is_c(Oid collation)
{
	/*
	 * If we're asked about "collation 0", return false, so that the code will
	 * go into the non-C path and report that the collation is bogus.
	 */
	if (!OidIsValid(collation))
		return false;

	/*
	 * If we're asked about the default collation, we have to inquire of the C
	 * library.  Cache the result so we only have to compute it once.
	 */
	if (collation == DEFAULT_COLLATION_OID)
	{
		static int	result = -1;
		char	   *localeptr;

		if (result >= 0)
			return (bool) result;
		localeptr = setlocale(LC_COLLATE, NULL);
		if (!localeptr)
			elog(ERROR, "invalid LC_COLLATE setting");

		if (strcmp(localeptr, "C") == 0)
			result = true;
		else if (strcmp(localeptr, "POSIX") == 0)
			result = true;
		else
			result = false;
		return (bool) result;
	}

	/*
	 * If we're asked about the built-in C/POSIX collations, we know that.
	 */
	if (collation == C_COLLATION_OID ||
		collation == POSIX_COLLATION_OID)
		return true;

	/*
	 * Otherwise, we have to consult pg_collation, but we cache that.
	 */
	return (lookup_collation_cache(collation, true))->collate_is_c;
}

/*
 * Detect whether collation's LC_CTYPE property is C
 */
bool
lc_ctype_is_c(Oid collation)
{
	/*
	 * If we're asked about "collation 0", return false, so that the code will
	 * go into the non-C path and report that the collation is bogus.
	 */
	if (!OidIsValid(collation))
		return false;

	/*
	 * If we're asked about the default collation, we have to inquire of the C
	 * library.  Cache the result so we only have to compute it once.
	 */
	if (collation == DEFAULT_COLLATION_OID)
	{
		static int	result = -1;
		char	   *localeptr;

		if (result >= 0)
			return (bool) result;
		localeptr = setlocale(LC_CTYPE, NULL);
		if (!localeptr)
			elog(ERROR, "invalid LC_CTYPE setting");

		if (strcmp(localeptr, "C") == 0)
			result = true;
		else if (strcmp(localeptr, "POSIX") == 0)
			result = true;
		else
			result = false;
		return (bool) result;
	}

	/*
	 * If we're asked about the built-in C/POSIX collations, we know that.
	 */
	if (collation == C_COLLATION_OID ||
		collation == POSIX_COLLATION_OID)
		return true;

	/*
	 * Otherwise, we have to consult pg_collation, but we cache that.
	 */
	return (lookup_collation_cache(collation, true))->ctype_is_c;
}


/* simple subroutine for reporting errors from newlocale() */
#ifdef HAVE_LOCALE_T
static void
report_newlocale_failure(const char *localename)
{
	/* copy errno in case one of the ereport auxiliary functions changes it */
	int			save_errno = errno;

	/*
	 * ENOENT means "no such locale", not "no such file", so clarify that
	 * errno with an errdetail message.
	 */
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("could not create locale \"%s\": %m",
					localename),
			 (save_errno == ENOENT ?
			  errdetail("The operating system could not find any locale data for the locale name \"%s\".",
						localename) : 0)));
}
#endif   /* HAVE_LOCALE_T */


/*
 * Create a locale_t from a collation OID.  Results are cached for the
 * lifetime of the backend.  Thus, do not free the result with freelocale().
 *
 * As a special optimization, the default/database collation returns 0.
 * Callers should then revert to the non-locale_t-enabled code path.
 * In fact, they shouldn't call this function at all when they are dealing
 * with the default locale.  That can save quite a bit in hotspots.
 * Also, callers should avoid calling this before going down a C/POSIX
 * fastpath, because such a fastpath should work even on platforms without
 * locale_t support in the C library.
 *
 * For simplicity, we always generate COLLATE + CTYPE even though we
 * might only need one of them.  Since this is called only once per session,
 * it shouldn't cost much.
 */
pg_locale_t
pg_newlocale_from_collation(Oid collid)
{
	collation_cache_entry *cache_entry;

	/* Callers must pass a valid OID */
	Assert(OidIsValid(collid));

	/* Return 0 for "default" collation, just in case caller forgets */
	if (collid == DEFAULT_COLLATION_OID)
		return (pg_locale_t) 0;

	cache_entry = lookup_collation_cache(collid, false);

	if (cache_entry->locale == 0)
	{
		/* We haven't computed this yet in this session, so do it */
#ifdef HAVE_LOCALE_T
		HeapTuple	tp;
		Form_pg_collation collform;
		const char *collcollate;
		const char *collctype;
		locale_t	result;

		tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for collation %u", collid);
		collform = (Form_pg_collation) GETSTRUCT(tp);

		collcollate = NameStr(collform->collcollate);
		collctype = NameStr(collform->collctype);

		if (strcmp(collcollate, collctype) == 0)
		{
			/* Normal case where they're the same */
#ifndef WIN32
			result = newlocale(LC_COLLATE_MASK | LC_CTYPE_MASK, collcollate,
							   NULL);
#else
			result = _create_locale(LC_ALL, collcollate);
#endif
			if (!result)
				report_newlocale_failure(collcollate);
		}
		else
		{
#ifndef WIN32
			/* We need two newlocale() steps */
			locale_t	loc1;

			loc1 = newlocale(LC_COLLATE_MASK, collcollate, NULL);
			if (!loc1)
				report_newlocale_failure(collcollate);
			result = newlocale(LC_CTYPE_MASK, collctype, loc1);
			if (!result)
				report_newlocale_failure(collctype);
#else

			/*
			 * XXX The _create_locale() API doesn't appear to support this.
			 * Could perhaps be worked around by changing pg_locale_t to
			 * contain two separate fields.
			 */
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("collations with different collate and ctype values are not supported on this platform")));
#endif
		}

		cache_entry->locale = result;

		ReleaseSysCache(tp);
#else							/* not HAVE_LOCALE_T */

		/*
		 * For platforms that don't support locale_t, we can't do anything
		 * with non-default collations.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		errmsg("nondefault collations are not supported on this platform")));
#endif   /* not HAVE_LOCALE_T */
	}

	return cache_entry->locale;
}


/*
 * These functions convert from/to libc's wchar_t, *not* pg_wchar_t.
 * Therefore we keep them here rather than with the mbutils code.
 */

#ifdef USE_WIDE_UPPER_LOWER

/*
 * wchar2char --- convert wide characters to multibyte format
 *
 * This has the same API as the standard wcstombs_l() function; in particular,
 * tolen is the maximum number of bytes to store at *to, and *from must be
 * zero-terminated.  The output will be zero-terminated iff there is room.
 */
size_t
wchar2char(char *to, const wchar_t *from, size_t tolen, pg_locale_t locale)
{
	size_t		result;

	if (tolen == 0)
		return 0;

#ifdef WIN32

	/*
	 * On Windows, the "Unicode" locales assume UTF16 not UTF8 encoding, and
	 * for some reason mbstowcs and wcstombs won't do this for us, so we use
	 * MultiByteToWideChar().
	 */
	if (GetDatabaseEncoding() == PG_UTF8)
	{
		result = WideCharToMultiByte(CP_UTF8, 0, from, -1, to, tolen,
									 NULL, NULL);
		/* A zero return is failure */
		if (result <= 0)
			result = -1;
		else
		{
			Assert(result <= tolen);
			/* Microsoft counts the zero terminator in the result */
			result--;
		}
	}
	else
#endif   /* WIN32 */
	if (locale == (pg_locale_t) 0)
	{
		/* Use wcstombs directly for the default locale */
		result = wcstombs(to, from, tolen);
	}
	else
	{
#ifdef HAVE_LOCALE_T
#ifdef HAVE_WCSTOMBS_L
		/* Use wcstombs_l for nondefault locales */
		result = wcstombs_l(to, from, tolen, locale);
#else							/* !HAVE_WCSTOMBS_L */
		/* We have to temporarily set the locale as current ... ugh */
		locale_t	save_locale = uselocale(locale);

		result = wcstombs(to, from, tolen);

		uselocale(save_locale);
#endif   /* HAVE_WCSTOMBS_L */
#else							/* !HAVE_LOCALE_T */
		/* Can't have locale != 0 without HAVE_LOCALE_T */
		elog(ERROR, "wcstombs_l is not available");
		result = 0;				/* keep compiler quiet */
#endif   /* HAVE_LOCALE_T */
	}

	return result;
}

/*
 * char2wchar --- convert multibyte characters to wide characters
 *
 * This has almost the API of mbstowcs_l(), except that *from need not be
 * null-terminated; instead, the number of input bytes is specified as
 * fromlen.  Also, we ereport() rather than returning -1 for invalid
 * input encoding.  tolen is the maximum number of wchar_t's to store at *to.
 * The output will be zero-terminated iff there is room.
 */
size_t
char2wchar(wchar_t *to, size_t tolen, const char *from, size_t fromlen,
		   pg_locale_t locale)
{
	size_t		result;

	if (tolen == 0)
		return 0;

#ifdef WIN32
	/* See WIN32 "Unicode" comment above */
	if (GetDatabaseEncoding() == PG_UTF8)
	{
		/* Win32 API does not work for zero-length input */
		if (fromlen == 0)
			result = 0;
		else
		{
			result = MultiByteToWideChar(CP_UTF8, 0, from, fromlen, to, tolen - 1);
			/* A zero return is failure */
			if (result == 0)
				result = -1;
		}

		if (result != -1)
		{
			Assert(result < tolen);
			/* Append trailing null wchar (MultiByteToWideChar() does not) */
			to[result] = 0;
		}
	}
	else
#endif   /* WIN32 */
	{
		/* mbstowcs requires ending '\0' */
		char	   *str = pnstrdup(from, fromlen);

		if (locale == (pg_locale_t) 0)
		{
			/* Use mbstowcs directly for the default locale */
			result = mbstowcs(to, str, tolen);
		}
		else
		{
#ifdef HAVE_LOCALE_T
#ifdef HAVE_MBSTOWCS_L
			/* Use mbstowcs_l for nondefault locales */
			result = mbstowcs_l(to, str, tolen, locale);
#else							/* !HAVE_MBSTOWCS_L */
			/* We have to temporarily set the locale as current ... ugh */
			locale_t	save_locale = uselocale(locale);

			result = mbstowcs(to, str, tolen);

			uselocale(save_locale);
#endif   /* HAVE_MBSTOWCS_L */
#else							/* !HAVE_LOCALE_T */
			/* Can't have locale != 0 without HAVE_LOCALE_T */
			elog(ERROR, "mbstowcs_l is not available");
			result = 0;			/* keep compiler quiet */
#endif   /* HAVE_LOCALE_T */
		}

		pfree(str);
	}

	if (result == -1)
	{
		/*
		 * Invalid multibyte character encountered.  We try to give a useful
		 * error message by letting pg_verifymbstr check the string.  But it's
		 * possible that the string is OK to us, and not OK to mbstowcs ---
		 * this suggests that the LC_CTYPE locale is different from the
		 * database encoding.  Give a generic error message if verifymbstr
		 * can't find anything wrong.
		 */
		pg_verifymbstr(from, fromlen, false);	/* might not return */
		/* but if it does ... */
		ereport(ERROR,
				(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
				 errmsg("invalid multibyte character for locale"),
				 errhint("The server's LC_CTYPE locale is probably incompatible with the database encoding.")));
	}

	return result;
}

#endif   /* USE_WIDE_UPPER_LOWER */
