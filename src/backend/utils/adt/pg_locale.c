/*-----------------------------------------------------------------------
 *
 * PostgreSQL locale utilities
 *
 * Portions Copyright (c) 2002-2010, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/backend/utils/adt/pg_locale.c,v 1.57 2010/07/06 19:18:58 momjian Exp $
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

#include "catalog/pg_control.h"
#include "mb/pg_wchar.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"

#ifdef WIN32
#include <shlwapi.h>
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

#if defined(WIN32) && defined(LC_MESSAGES)
static char *IsoLocaleName(const char *);		/* MSVC specific */
#endif


/*
 * pg_perm_setlocale
 *
 * This is identical to the libc function setlocale(), with the addition
 * that if the operation is successful, the corresponding LC_XXX environment
 * variable is set to match.  By setting the environment variable, we ensure
 * that any subsequent use of setlocale(..., "") will preserve the settings
 * made through this routine.  Of course, LC_ALL must also be unset to fully
 * ensure that, but that has to be done elsewhere after all the individual
 * LC_XXX variables have been set correctly.  (Thank you Perl for making this
 * kluge necessary.)
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
 */
bool
check_locale(int category, const char *value)
{
	char	   *save;
	bool		ret;

	save = setlocale(category, NULL);
	if (!save)
		return false;			/* won't happen, we hope */

	/* save may be pointing at a modifiable scratch variable, see above */
	save = pstrdup(save);

	/* set the locale with setlocale, to see if it accepts it. */
	ret = (setlocale(category, value) != NULL);

	setlocale(category, save);	/* assume this won't fail */
	pfree(save);

	return ret;
}

/* GUC assign hooks */

/*
 * This is common code for several locale categories.  This doesn't
 * actually set the locale permanently, it only tests if the locale is
 * valid.  (See explanation at the top of this file.)
 *
 * Note: we accept value = "" as selecting the postmaster's environment
 * value, whatever it was (so long as the environment setting is legal).
 * This will have been locked down by an earlier call to pg_perm_setlocale.
 */
static const char *
locale_xxx_assign(int category, const char *value, bool doit, GucSource source)
{
	if (!check_locale(category, value))
		value = NULL;			/* set failure return marker */

	/* need to reload cache next time? */
	if (doit && value != NULL)
	{
		CurrentLocaleConvValid = false;
		CurrentLCTimeValid = false;
	}

	return value;
}


const char *
locale_monetary_assign(const char *value, bool doit, GucSource source)
{
	return locale_xxx_assign(LC_MONETARY, value, doit, source);
}

const char *
locale_numeric_assign(const char *value, bool doit, GucSource source)
{
	return locale_xxx_assign(LC_NUMERIC, value, doit, source);
}

const char *
locale_time_assign(const char *value, bool doit, GucSource source)
{
	return locale_xxx_assign(LC_TIME, value, doit, source);
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
const char *
locale_messages_assign(const char *value, bool doit, GucSource source)
{
	if (*value == '\0' && source != PGC_S_DEFAULT)
		return NULL;

	/*
	 * LC_MESSAGES category does not exist everywhere, but accept it anyway
	 *
	 * On Windows, we can't even check the value, so the non-doit case is a
	 * no-op
	 */
#ifdef LC_MESSAGES
	if (doit)
	{
		if (!pg_perm_setlocale(LC_MESSAGES, value))
			if (source != PGC_S_DEFAULT)
				return NULL;
	}
#ifndef WIN32
	else
		value = locale_xxx_assign(LC_MESSAGES, value, false, source);
#endif   /* WIN32 */
#endif   /* LC_MESSAGES */
	return value;
}


/*
 * We'd like to cache whether LC_COLLATE is C (or POSIX), so we can
 * optimize a few code paths in various places.
 */
bool
lc_collate_is_c(void)
{
	/* Cache result so we only have to compute it once */
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
 * We'd like to cache whether LC_CTYPE is C (or POSIX), so we can
 * optimize a few code paths in various places.
 */
bool
lc_ctype_is_c(void)
{
	/* Cache result so we only have to compute it once */
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
	pstr = (char *) pg_do_encoding_conversion(
										  (unsigned char *) str, strlen(str),
											encoding, GetDatabaseEncoding());
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
	encoding = pg_get_encoding_from_locale(locale_numeric);

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
	encoding = pg_get_encoding_from_locale(locale_monetary);

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
		setlocale(LC_MONETARY, save_lc_monetary);
		pfree(save_lc_monetary);
	}

	if (save_lc_numeric)
	{
		setlocale(LC_NUMERIC, save_lc_numeric);
		pfree(save_lc_numeric);
	}

#ifdef WIN32
	/* Try to restore internal ctype settings */
	if (save_lc_ctype)
	{
		setlocale(LC_CTYPE, save_lc_ctype);
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
	int			encoding;

	encoding = GetDatabaseEncoding();

	len = wcsftime(wbuf, MAX_L10N_DATA, format, tm);
	if (len == 0)

		/*
		 * strftime call failed - return 0 with the contents of dst
		 * unspecified
		 */
		return 0;

	len = WideCharToMultiByte(CP_UTF8, 0, wbuf, len, dst, dstlen, NULL, NULL);
	if (len == 0)
		elog(ERROR,
			 "could not convert string to UTF-8:error %lu", GetLastError());

	dst[len] = '\0';
	if (encoding != PG_UTF8)
	{
		char	   *convstr = pg_do_encoding_conversion(dst, len, PG_UTF8, encoding);

		if (dst != convstr)
		{
			strlcpy(dst, convstr, dstlen);
			len = strlen(dst);
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
		setlocale(LC_TIME, save_lc_time);
		pfree(save_lc_time);
	}

#ifdef WIN32
	/* try to restore internal ctype settings */
	if (save_lc_ctype)
	{
		setlocale(LC_CTYPE, save_lc_ctype);
		pfree(save_lc_ctype);
	}
#endif

	CurrentLCTimeValid = true;
}


#if defined(WIN32) && defined(LC_MESSAGES)
/*
 *	Convert Windows locale name to the ISO formatted one
 *	if possible.
 *
 *	This function returns NULL if conversion is impossible,
 *	otherwise returns the pointer to a static area which
 *	contains the iso formatted locale name.
 */
static
char *
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
		return iso_lc_messages;
	}
	return NULL;
#else
	return NULL;				/* Not supported on this version of msvc/mingw */
#endif   /* _MSC_VER >= 1400 */
}

#endif   /* WIN32 && LC_MESSAGES */
