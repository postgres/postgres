/*-----------------------------------------------------------------------
 *
 * PostgreSQL locale utilities
 *
 * Portions Copyright (c) 2002-2020, PostgreSQL Global Development Group
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
 * Instead, we only set these locale categories briefly when needed,
 * cache the required information obtained from localeconv() or
 * strftime(), and then set the locale categories back to "C".
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
 * The POSIX locale standard is available here:
 *
 *	http://www.opengroup.org/onlinepubs/009695399/basedefs/xbd_chap07.html
 *----------
 */


#include "postgres.h"

#include <time.h>

#include "access/htup_details.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_control.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/formatting.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/syscache.h"

#ifdef USE_ICU
#include <unicode/ucnv.h>
#endif

#ifdef __GLIBC__
#include <gnu/libc-version.h>
#endif

#ifdef WIN32
/*
 * This Windows file defines StrNCpy. We don't need it here, so we undefine
 * it to keep the compiler quiet, and undefine it again after the file is
 * included, so we don't accidentally use theirs.
 */
#undef StrNCpy
#include <shlwapi.h>
#ifdef StrNCpy
#undef StrNCpy
#endif
#endif

#define		MAX_L10N_DATA		80


/* GUC settings */
char	   *locale_messages;
char	   *locale_monetary;
char	   *locale_numeric;
char	   *locale_time;

/*
 * lc_time localization cache.
 *
 * We use only the first 7 or 12 entries of these arrays.  The last array
 * element is left as NULL for the convenience of outside code that wants
 * to sequentially scan these arrays.
 */
char	   *localized_abbrev_days[7 + 1];
char	   *localized_full_days[7 + 1];
char	   *localized_abbrev_months[12 + 1];
char	   *localized_full_months[12 + 1];

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
static char *IsoLocaleName(const char *);	/* MSVC specific */
#endif

#ifdef USE_ICU
static void icu_set_collation_attributes(UCollator *collator, const char *loc);
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
#endif							/* WIN32 */

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
		static char save_lc_ctype[LC_ENV_BUFSIZE];

		/* copy setlocale() return value before callee invokes it again */
		strlcpy(save_lc_ctype, result, sizeof(save_lc_ctype));
		result = save_lc_ctype;

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
			elog(DEBUG3, "IsoLocaleName() executed; locale: \"%s\"", result);
#endif							/* WIN32 */
			break;
#endif							/* LC_MESSAGES */
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
 * itself.)  It's important that this not throw elog(ERROR).
 */
static void
free_struct_lconv(struct lconv *s)
{
	if (s->decimal_point)
		free(s->decimal_point);
	if (s->thousands_sep)
		free(s->thousands_sep);
	if (s->grouping)
		free(s->grouping);
	if (s->int_curr_symbol)
		free(s->int_curr_symbol);
	if (s->currency_symbol)
		free(s->currency_symbol);
	if (s->mon_decimal_point)
		free(s->mon_decimal_point);
	if (s->mon_thousands_sep)
		free(s->mon_thousands_sep);
	if (s->mon_grouping)
		free(s->mon_grouping);
	if (s->positive_sign)
		free(s->positive_sign);
	if (s->negative_sign)
		free(s->negative_sign);
}

/*
 * Check that all fields of a struct lconv (or at least, the ones we care
 * about) are non-NULL.  The field list must match free_struct_lconv().
 */
static bool
struct_lconv_is_valid(struct lconv *s)
{
	if (s->decimal_point == NULL)
		return false;
	if (s->thousands_sep == NULL)
		return false;
	if (s->grouping == NULL)
		return false;
	if (s->int_curr_symbol == NULL)
		return false;
	if (s->currency_symbol == NULL)
		return false;
	if (s->mon_decimal_point == NULL)
		return false;
	if (s->mon_thousands_sep == NULL)
		return false;
	if (s->mon_grouping == NULL)
		return false;
	if (s->positive_sign == NULL)
		return false;
	if (s->negative_sign == NULL)
		return false;
	return true;
}


/*
 * Convert the strdup'd string at *str from the specified encoding to the
 * database encoding.
 */
static void
db_encoding_convert(int encoding, char **str)
{
	char	   *pstr;
	char	   *mstr;

	/* convert the string to the database encoding */
	pstr = pg_any_to_server(*str, strlen(*str), encoding);
	if (pstr == *str)
		return;					/* no conversion happened */

	/* need it malloc'd not palloc'd */
	mstr = strdup(pstr);
	if (mstr == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	/* replace old string */
	free(*str);
	*str = mstr;

	pfree(pstr);
}


/*
 * Return the POSIX lconv struct (contains number/money formatting
 * information) with locale information for all categories.
 */
struct lconv *
PGLC_localeconv(void)
{
	static struct lconv CurrentLocaleConv;
	static bool CurrentLocaleConvAllocated = false;
	struct lconv *extlconv;
	struct lconv worklconv;
	char	   *save_lc_monetary;
	char	   *save_lc_numeric;
#ifdef WIN32
	char	   *save_lc_ctype;
#endif

	/* Did we do it already? */
	if (CurrentLocaleConvValid)
		return &CurrentLocaleConv;

	/* Free any already-allocated storage */
	if (CurrentLocaleConvAllocated)
	{
		free_struct_lconv(&CurrentLocaleConv);
		CurrentLocaleConvAllocated = false;
	}

	/*
	 * This is tricky because we really don't want to risk throwing error
	 * while the locale is set to other than our usual settings.  Therefore,
	 * the process is: collect the usual settings, set locale to special
	 * setting, copy relevant data into worklconv using strdup(), restore
	 * normal settings, convert data to desired encoding, and finally stash
	 * the collected data in CurrentLocaleConv.  This makes it safe if we
	 * throw an error during encoding conversion or run out of memory anywhere
	 * in the process.  All data pointed to by struct lconv members is
	 * allocated with strdup, to avoid premature elog(ERROR) and to allow
	 * using a single cleanup routine.
	 */
	memset(&worklconv, 0, sizeof(worklconv));

	/* Save prevailing values of monetary and numeric locales */
	save_lc_monetary = setlocale(LC_MONETARY, NULL);
	if (!save_lc_monetary)
		elog(ERROR, "setlocale(NULL) failed");
	save_lc_monetary = pstrdup(save_lc_monetary);

	save_lc_numeric = setlocale(LC_NUMERIC, NULL);
	if (!save_lc_numeric)
		elog(ERROR, "setlocale(NULL) failed");
	save_lc_numeric = pstrdup(save_lc_numeric);

#ifdef WIN32

	/*
	 * The POSIX standard explicitly says that it is undefined what happens if
	 * LC_MONETARY or LC_NUMERIC imply an encoding (codeset) different from
	 * that implied by LC_CTYPE.  In practice, all Unix-ish platforms seem to
	 * believe that localeconv() should return strings that are encoded in the
	 * codeset implied by the LC_MONETARY or LC_NUMERIC locale name.  Hence,
	 * once we have successfully collected the localeconv() results, we will
	 * convert them from that codeset to the desired server encoding.
	 *
	 * Windows, of course, resolutely does things its own way; on that
	 * platform LC_CTYPE has to match LC_MONETARY/LC_NUMERIC to get sane
	 * results.  Hence, we must temporarily set that category as well.
	 */

	/* Save prevailing value of ctype locale */
	save_lc_ctype = setlocale(LC_CTYPE, NULL);
	if (!save_lc_ctype)
		elog(ERROR, "setlocale(NULL) failed");
	save_lc_ctype = pstrdup(save_lc_ctype);

	/* Here begins the critical section where we must not throw error */

	/* use numeric to set the ctype */
	setlocale(LC_CTYPE, locale_numeric);
#endif

	/* Get formatting information for numeric */
	setlocale(LC_NUMERIC, locale_numeric);
	extlconv = localeconv();

	/* Must copy data now in case setlocale() overwrites it */
	worklconv.decimal_point = strdup(extlconv->decimal_point);
	worklconv.thousands_sep = strdup(extlconv->thousands_sep);
	worklconv.grouping = strdup(extlconv->grouping);

#ifdef WIN32
	/* use monetary to set the ctype */
	setlocale(LC_CTYPE, locale_monetary);
#endif

	/* Get formatting information for monetary */
	setlocale(LC_MONETARY, locale_monetary);
	extlconv = localeconv();

	/* Must copy data now in case setlocale() overwrites it */
	worklconv.int_curr_symbol = strdup(extlconv->int_curr_symbol);
	worklconv.currency_symbol = strdup(extlconv->currency_symbol);
	worklconv.mon_decimal_point = strdup(extlconv->mon_decimal_point);
	worklconv.mon_thousands_sep = strdup(extlconv->mon_thousands_sep);
	worklconv.mon_grouping = strdup(extlconv->mon_grouping);
	worklconv.positive_sign = strdup(extlconv->positive_sign);
	worklconv.negative_sign = strdup(extlconv->negative_sign);
	/* Copy scalar fields as well */
	worklconv.int_frac_digits = extlconv->int_frac_digits;
	worklconv.frac_digits = extlconv->frac_digits;
	worklconv.p_cs_precedes = extlconv->p_cs_precedes;
	worklconv.p_sep_by_space = extlconv->p_sep_by_space;
	worklconv.n_cs_precedes = extlconv->n_cs_precedes;
	worklconv.n_sep_by_space = extlconv->n_sep_by_space;
	worklconv.p_sign_posn = extlconv->p_sign_posn;
	worklconv.n_sign_posn = extlconv->n_sign_posn;

	/*
	 * Restore the prevailing locale settings; failure to do so is fatal.
	 * Possibly we could limp along with nondefault LC_MONETARY or LC_NUMERIC,
	 * but proceeding with the wrong value of LC_CTYPE would certainly be bad
	 * news; and considering that the prevailing LC_MONETARY and LC_NUMERIC
	 * are almost certainly "C", there's really no reason that restoring those
	 * should fail.
	 */
#ifdef WIN32
	if (!setlocale(LC_CTYPE, save_lc_ctype))
		elog(FATAL, "failed to restore LC_CTYPE to \"%s\"", save_lc_ctype);
#endif
	if (!setlocale(LC_MONETARY, save_lc_monetary))
		elog(FATAL, "failed to restore LC_MONETARY to \"%s\"", save_lc_monetary);
	if (!setlocale(LC_NUMERIC, save_lc_numeric))
		elog(FATAL, "failed to restore LC_NUMERIC to \"%s\"", save_lc_numeric);

	/*
	 * At this point we've done our best to clean up, and can call functions
	 * that might possibly throw errors with a clean conscience.  But let's
	 * make sure we don't leak any already-strdup'd fields in worklconv.
	 */
	PG_TRY();
	{
		int			encoding;

		/* Release the pstrdup'd locale names */
		pfree(save_lc_monetary);
		pfree(save_lc_numeric);
#ifdef WIN32
		pfree(save_lc_ctype);
#endif

		/* If any of the preceding strdup calls failed, complain now. */
		if (!struct_lconv_is_valid(&worklconv))
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));

		/*
		 * Now we must perform encoding conversion from whatever's associated
		 * with the locales into the database encoding.  If we can't identify
		 * the encoding implied by LC_NUMERIC or LC_MONETARY (ie we get -1),
		 * use PG_SQL_ASCII, which will result in just validating that the
		 * strings are OK in the database encoding.
		 */
		encoding = pg_get_encoding_from_locale(locale_numeric, true);
		if (encoding < 0)
			encoding = PG_SQL_ASCII;

		db_encoding_convert(encoding, &worklconv.decimal_point);
		db_encoding_convert(encoding, &worklconv.thousands_sep);
		/* grouping is not text and does not require conversion */

		encoding = pg_get_encoding_from_locale(locale_monetary, true);
		if (encoding < 0)
			encoding = PG_SQL_ASCII;

		db_encoding_convert(encoding, &worklconv.int_curr_symbol);
		db_encoding_convert(encoding, &worklconv.currency_symbol);
		db_encoding_convert(encoding, &worklconv.mon_decimal_point);
		db_encoding_convert(encoding, &worklconv.mon_thousands_sep);
		/* mon_grouping is not text and does not require conversion */
		db_encoding_convert(encoding, &worklconv.positive_sign);
		db_encoding_convert(encoding, &worklconv.negative_sign);
	}
	PG_CATCH();
	{
		free_struct_lconv(&worklconv);
		PG_RE_THROW();
	}
	PG_END_TRY();

	/*
	 * Everything is good, so save the results.
	 */
	CurrentLocaleConv = worklconv;
	CurrentLocaleConvAllocated = true;
	CurrentLocaleConvValid = true;
	return &CurrentLocaleConv;
}

#ifdef WIN32
/*
 * On Windows, strftime() returns its output in encoding CP_ACP (the default
 * operating system codepage for the computer), which is likely different
 * from SERVER_ENCODING.  This is especially important in Japanese versions
 * of Windows which will use SJIS encoding, which we don't support as a
 * server encoding.
 *
 * So, instead of using strftime(), use wcsftime() to return the value in
 * wide characters (internally UTF16) and then convert to UTF8, which we
 * know how to handle directly.
 *
 * Note that this only affects the calls to strftime() in this file, which are
 * used to get the locale-aware strings. Other parts of the backend use
 * pg_strftime(), which isn't locale-aware and does not need to be replaced.
 */
static size_t
strftime_win32(char *dst, size_t dstlen,
			   const char *format, const struct tm *tm)
{
	size_t		len;
	wchar_t		wformat[8];		/* formats used below need 3 chars */
	wchar_t		wbuf[MAX_L10N_DATA];

	/*
	 * Get a wchar_t version of the format string.  We only actually use
	 * plain-ASCII formats in this file, so we can say that they're UTF8.
	 */
	len = MultiByteToWideChar(CP_UTF8, 0, format, -1,
							  wformat, lengthof(wformat));
	if (len == 0)
		elog(ERROR, "could not convert format string from UTF-8: error code %lu",
			 GetLastError());

	len = wcsftime(wbuf, MAX_L10N_DATA, wformat, tm);
	if (len == 0)
	{
		/*
		 * wcsftime failed, possibly because the result would not fit in
		 * MAX_L10N_DATA.  Return 0 with the contents of dst unspecified.
		 */
		return 0;
	}

	len = WideCharToMultiByte(CP_UTF8, 0, wbuf, len, dst, dstlen - 1,
							  NULL, NULL);
	if (len == 0)
		elog(ERROR, "could not convert string to UTF-8: error code %lu",
			 GetLastError());

	dst[len] = '\0';

	return len;
}

/* redefine strftime() */
#define strftime(a,b,c,d) strftime_win32(a,b,c,d)
#endif							/* WIN32 */

/*
 * Subroutine for cache_locale_time().
 * Convert the given string from encoding "encoding" to the database
 * encoding, and store the result at *dst, replacing any previous value.
 */
static void
cache_single_string(char **dst, const char *src, int encoding)
{
	char	   *ptr;
	char	   *olddst;

	/* Convert the string to the database encoding, or validate it's OK */
	ptr = pg_any_to_server(src, strlen(src), encoding);

	/* Store the string in long-lived storage, replacing any previous value */
	olddst = *dst;
	*dst = MemoryContextStrdup(TopMemoryContext, ptr);
	if (olddst)
		pfree(olddst);

	/* Might as well clean up any palloc'd conversion result, too */
	if (ptr != src)
		pfree(ptr);
}

/*
 * Update the lc_time localization cache variables if needed.
 */
void
cache_locale_time(void)
{
	char		buf[(2 * 7 + 2 * 12) * MAX_L10N_DATA];
	char	   *bufptr;
	time_t		timenow;
	struct tm  *timeinfo;
	bool		strftimefail = false;
	int			encoding;
	int			i;
	char	   *save_lc_time;
#ifdef WIN32
	char	   *save_lc_ctype;
#endif

	/* did we do this already? */
	if (CurrentLCTimeValid)
		return;

	elog(DEBUG3, "cache_locale_time() executed; locale: \"%s\"", locale_time);

	/*
	 * As in PGLC_localeconv(), it's critical that we not throw error while
	 * libc's locale settings have nondefault values.  Hence, we just call
	 * strftime() within the critical section, and then convert and save its
	 * results afterwards.
	 */

	/* Save prevailing value of time locale */
	save_lc_time = setlocale(LC_TIME, NULL);
	if (!save_lc_time)
		elog(ERROR, "setlocale(NULL) failed");
	save_lc_time = pstrdup(save_lc_time);

#ifdef WIN32

	/*
	 * On Windows, it appears that wcsftime() internally uses LC_CTYPE, so we
	 * must set it here.  This code looks the same as what PGLC_localeconv()
	 * does, but the underlying reason is different: this does NOT determine
	 * the encoding we'll get back from strftime_win32().
	 */

	/* Save prevailing value of ctype locale */
	save_lc_ctype = setlocale(LC_CTYPE, NULL);
	if (!save_lc_ctype)
		elog(ERROR, "setlocale(NULL) failed");
	save_lc_ctype = pstrdup(save_lc_ctype);

	/* use lc_time to set the ctype */
	setlocale(LC_CTYPE, locale_time);
#endif

	setlocale(LC_TIME, locale_time);

	/* We use times close to current time as data for strftime(). */
	timenow = time(NULL);
	timeinfo = localtime(&timenow);

	/* Store the strftime results in MAX_L10N_DATA-sized portions of buf[] */
	bufptr = buf;

	/*
	 * MAX_L10N_DATA is sufficient buffer space for every known locale, and
	 * POSIX defines no strftime() errors.  (Buffer space exhaustion is not an
	 * error.)  An implementation might report errors (e.g. ENOMEM) by
	 * returning 0 (or, less plausibly, a negative value) and setting errno.
	 * Report errno just in case the implementation did that, but clear it in
	 * advance of the calls so we don't emit a stale, unrelated errno.
	 */
	errno = 0;

	/* localized days */
	for (i = 0; i < 7; i++)
	{
		timeinfo->tm_wday = i;
		if (strftime(bufptr, MAX_L10N_DATA, "%a", timeinfo) <= 0)
			strftimefail = true;
		bufptr += MAX_L10N_DATA;
		if (strftime(bufptr, MAX_L10N_DATA, "%A", timeinfo) <= 0)
			strftimefail = true;
		bufptr += MAX_L10N_DATA;
	}

	/* localized months */
	for (i = 0; i < 12; i++)
	{
		timeinfo->tm_mon = i;
		timeinfo->tm_mday = 1;	/* make sure we don't have invalid date */
		if (strftime(bufptr, MAX_L10N_DATA, "%b", timeinfo) <= 0)
			strftimefail = true;
		bufptr += MAX_L10N_DATA;
		if (strftime(bufptr, MAX_L10N_DATA, "%B", timeinfo) <= 0)
			strftimefail = true;
		bufptr += MAX_L10N_DATA;
	}

	/*
	 * Restore the prevailing locale settings; as in PGLC_localeconv(),
	 * failure to do so is fatal.
	 */
#ifdef WIN32
	if (!setlocale(LC_CTYPE, save_lc_ctype))
		elog(FATAL, "failed to restore LC_CTYPE to \"%s\"", save_lc_ctype);
#endif
	if (!setlocale(LC_TIME, save_lc_time))
		elog(FATAL, "failed to restore LC_TIME to \"%s\"", save_lc_time);

	/*
	 * At this point we've done our best to clean up, and can throw errors, or
	 * call functions that might throw errors, with a clean conscience.
	 */
	if (strftimefail)
		elog(ERROR, "strftime() failed: %m");

	/* Release the pstrdup'd locale names */
	pfree(save_lc_time);
#ifdef WIN32
	pfree(save_lc_ctype);
#endif

#ifndef WIN32

	/*
	 * As in PGLC_localeconv(), we must convert strftime()'s output from the
	 * encoding implied by LC_TIME to the database encoding.  If we can't
	 * identify the LC_TIME encoding, just perform encoding validation.
	 */
	encoding = pg_get_encoding_from_locale(locale_time, true);
	if (encoding < 0)
		encoding = PG_SQL_ASCII;

#else

	/*
	 * On Windows, strftime_win32() always returns UTF8 data, so convert from
	 * that if necessary.
	 */
	encoding = PG_UTF8;

#endif							/* WIN32 */

	bufptr = buf;

	/* localized days */
	for (i = 0; i < 7; i++)
	{
		cache_single_string(&localized_abbrev_days[i], bufptr, encoding);
		bufptr += MAX_L10N_DATA;
		cache_single_string(&localized_full_days[i], bufptr, encoding);
		bufptr += MAX_L10N_DATA;
	}
	localized_abbrev_days[7] = NULL;
	localized_full_days[7] = NULL;

	/* localized months */
	for (i = 0; i < 12; i++)
	{
		cache_single_string(&localized_abbrev_months[i], bufptr, encoding);
		bufptr += MAX_L10N_DATA;
		cache_single_string(&localized_full_months[i], bufptr, encoding);
		bufptr += MAX_L10N_DATA;
	}
	localized_abbrev_months[12] = NULL;
	localized_full_months[12] = NULL;

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
 * Visual Studio 2015 should still be able to do the same as Visual Studio
 * 2012, but the declaration of locale_name is missing in _locale_t, causing
 * this code compilation to fail, hence this falls back instead on to
 * enumerating all system locales by using EnumSystemLocalesEx to find the
 * required locale name.  If the input argument is in Unix-style then we can
 * get ISO Locale name directly by using GetLocaleInfoEx() with LCType as
 * LOCALE_SNAME.
 *
 * MinGW headers declare _create_locale(), but msvcrt.dll lacks that symbol in
 * releases before Windows 8. IsoLocaleName() always fails in a MinGW-built
 * postgres.exe, so only Unix-style values of the lc_messages GUC can elicit
 * localized messages. In particular, every lc_messages setting that initdb
 * can select automatically will yield only C-locale messages. XXX This could
 * be fixed by running the fully-qualified locale name through a lookup table.
 *
 * This function returns a pointer to a static buffer bearing the converted
 * name or NULL if conversion fails.
 *
 * [1] https://docs.microsoft.com/en-us/windows/win32/intl/locale-identifiers
 * [2] https://docs.microsoft.com/en-us/windows/win32/intl/locale-names
 */

#if _MSC_VER >= 1900
/*
 * Callback function for EnumSystemLocalesEx() in get_iso_localename().
 *
 * This function enumerates all system locales, searching for one that matches
 * an input with the format: <Language>[_<Country>], e.g.
 * English[_United States]
 *
 * The input is a three wchar_t array as an LPARAM. The first element is the
 * locale_name we want to match, the second element is an allocated buffer
 * where the Unix-style locale is copied if a match is found, and the third
 * element is the search status, 1 if a match was found, 0 otherwise.
 */
static BOOL CALLBACK
search_locale_enum(LPWSTR pStr, DWORD dwFlags, LPARAM lparam)
{
	wchar_t		test_locale[LOCALE_NAME_MAX_LENGTH];
	wchar_t   **argv;

	(void) (dwFlags);

	argv = (wchar_t **) lparam;
	*argv[2] = (wchar_t) 0;

	memset(test_locale, 0, sizeof(test_locale));

	/* Get the name of the <Language> in English */
	if (GetLocaleInfoEx(pStr, LOCALE_SENGLISHLANGUAGENAME,
						test_locale, LOCALE_NAME_MAX_LENGTH))
	{
		/*
		 * If the enumerated locale does not have a hyphen ("en") OR  the
		 * lc_message input does not have an underscore ("English"), we only
		 * need to compare the <Language> tags.
		 */
		if (wcsrchr(pStr, '-') == NULL || wcsrchr(argv[0], '_') == NULL)
		{
			if (_wcsicmp(argv[0], test_locale) == 0)
			{
				wcscpy(argv[1], pStr);
				*argv[2] = (wchar_t) 1;
				return FALSE;
			}
		}

		/*
		 * We have to compare a full <Language>_<Country> tag, so we append
		 * the underscore and name of the country/region in English, e.g.
		 * "English_United States".
		 */
		else
		{
			size_t		len;

			wcscat(test_locale, L"_");
			len = wcslen(test_locale);
			if (GetLocaleInfoEx(pStr, LOCALE_SENGLISHCOUNTRYNAME,
								test_locale + len,
								LOCALE_NAME_MAX_LENGTH - len))
			{
				if (_wcsicmp(argv[0], test_locale) == 0)
				{
					wcscpy(argv[1], pStr);
					*argv[2] = (wchar_t) 1;
					return FALSE;
				}
			}
		}
	}

	return TRUE;
}

/*
 * This function converts a Windows locale name to an ISO formatted version
 * for Visual Studio 2015 or greater.
 *
 * Returns NULL, if no valid conversion was found.
 */
static char *
get_iso_localename(const char *winlocname)
{
	wchar_t		wc_locale_name[LOCALE_NAME_MAX_LENGTH];
	wchar_t		buffer[LOCALE_NAME_MAX_LENGTH];
	static char iso_lc_messages[LOCALE_NAME_MAX_LENGTH];
	char	   *period;
	int			len;
	int			ret_val;

	/*
	 * Valid locales have the following syntax:
	 * <Language>[_<Country>[.<CodePage>]]
	 *
	 * GetLocaleInfoEx can only take locale name without code-page and for the
	 * purpose of this API the code-page doesn't matter.
	 */
	period = strchr(winlocname, '.');
	if (period != NULL)
		len = period - winlocname;
	else
		len = pg_mbstrlen(winlocname);

	memset(wc_locale_name, 0, sizeof(wc_locale_name));
	memset(buffer, 0, sizeof(buffer));
	MultiByteToWideChar(CP_ACP, 0, winlocname, len, wc_locale_name,
						LOCALE_NAME_MAX_LENGTH);

	/*
	 * If the lc_messages is already an Unix-style string, we have a direct
	 * match with LOCALE_SNAME, e.g. en-US, en_US.
	 */
	ret_val = GetLocaleInfoEx(wc_locale_name, LOCALE_SNAME, (LPWSTR) &buffer,
							  LOCALE_NAME_MAX_LENGTH);
	if (!ret_val)
	{
		/*
		 * Search for a locale in the system that matches language and country
		 * name.
		 */
		wchar_t    *argv[3];

		argv[0] = wc_locale_name;
		argv[1] = buffer;
		argv[2] = (wchar_t *) &ret_val;
		EnumSystemLocalesEx(search_locale_enum, LOCALE_WINDOWS, (LPARAM) argv,
							NULL);
	}

	if (ret_val)
	{
		size_t		rc;
		char	   *hyphen;

		/* Locale names use only ASCII, any conversion locale suffices. */
		rc = wchar2char(iso_lc_messages, buffer, sizeof(iso_lc_messages), NULL);
		if (rc == -1 || rc == sizeof(iso_lc_messages))
			return NULL;

		/*
		 * Simply replace the hyphen with an underscore.  See comments in
		 * IsoLocaleName.
		 */
		hyphen = strchr(iso_lc_messages, '-');
		if (hyphen)
			*hyphen = '_';
		return iso_lc_messages;
	}

	return NULL;
}
#endif							/* _MSC_VER >= 1900 */

static char *
IsoLocaleName(const char *winlocname)
{
#if defined(_MSC_VER)
	static char iso_lc_messages[LOCALE_NAME_MAX_LENGTH];

	if (pg_strcasecmp("c", winlocname) == 0 ||
		pg_strcasecmp("posix", winlocname) == 0)
	{
		strcpy(iso_lc_messages, "C");
		return iso_lc_messages;
	}
	else
	{
#if (_MSC_VER >= 1900)			/* Visual Studio 2015 or later */
		return get_iso_localename(winlocname);
#else
		_locale_t	loct;

		loct = _create_locale(LC_CTYPE, winlocname);
		if (loct != NULL)
		{
			size_t		rc;
			char	   *hyphen;

			/* Locale names use only ASCII, any conversion locale suffices. */
			rc = wchar2char(iso_lc_messages, loct->locinfo->locale_name[LC_CTYPE],
							sizeof(iso_lc_messages), NULL);
			_free_locale(loct);
			if (rc == -1 || rc == sizeof(iso_lc_messages))
				return NULL;

			/*
			 * Since the message catalogs sit on a case-insensitive
			 * filesystem, we need not standardize letter case here.  So long
			 * as we do not ship message catalogs for which it would matter,
			 * we also need not translate the script/variant portion, e.g.
			 * uz-Cyrl-UZ to uz_UZ@cyrillic.  Simply replace the hyphen with
			 * an underscore.
			 *
			 * Note that the locale name can be less-specific than the value
			 * we would derive under earlier Visual Studio releases.  For
			 * example, French_France.1252 yields just "fr".  This does not
			 * affect any of the country-specific message catalogs available
			 * as of this writing (pt_BR, zh_CN, zh_TW).
			 */
			hyphen = strchr(iso_lc_messages, '-');
			if (hyphen)
				*hyphen = '_';
			return iso_lc_messages;
		}
#endif							/* Visual Studio 2015 or later */
	}
#endif							/* defined(_MSC_VER) */
	return NULL;				/* Not supported on this version of msvc/mingw */
}
#endif							/* WIN32 && LC_MESSAGES */


/*
 * Detect aging strxfrm() implementations that, in a subset of locales, write
 * past the specified buffer length.  Affected users must update OS packages
 * before using PostgreSQL 9.5 or later.
 *
 * Assume that the bug can come and go from one postmaster startup to another
 * due to physical replication among diverse machines.  Assume that the bug's
 * presence will not change during the life of a particular postmaster.  Given
 * those assumptions, call this no less than once per postmaster startup per
 * LC_COLLATE setting used.  No known-affected system offers strxfrm_l(), so
 * there is no need to consider pg_collation locales.
 */
void
check_strxfrm_bug(void)
{
	char		buf[32];
	const int	canary = 0x7F;
	bool		ok = true;

	/*
	 * Given a two-byte ASCII string and length limit 7, 8 or 9, Solaris 10
	 * 05/08 returns 18 and modifies 10 bytes.  It respects limits above or
	 * below that range.
	 *
	 * The bug is present in Solaris 8 as well; it is absent in Solaris 10
	 * 01/13 and Solaris 11.2.  Affected locales include is_IS.ISO8859-1,
	 * en_US.UTF-8, en_US.ISO8859-1, and ru_RU.KOI8-R.  Unaffected locales
	 * include de_DE.UTF-8, de_DE.ISO8859-1, zh_TW.UTF-8, and C.
	 */
	buf[7] = canary;
	(void) strxfrm(buf, "ab", 7);
	if (buf[7] != canary)
		ok = false;

	/*
	 * illumos bug #1594 was present in the source tree from 2010-10-11 to
	 * 2012-02-01.  Given an ASCII string of any length and length limit 1,
	 * affected systems ignore the length limit and modify a number of bytes
	 * one less than the return value.  The problem inputs for this bug do not
	 * overlap those for the Solaris bug, hence a distinct test.
	 *
	 * Affected systems include smartos-20110926T021612Z.  Affected locales
	 * include en_US.ISO8859-1 and en_US.UTF-8.  Unaffected locales include C.
	 */
	buf[1] = canary;
	(void) strxfrm(buf, "a", 1);
	if (buf[1] != canary)
		ok = false;

	if (!ok)
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg_internal("strxfrm(), in locale \"%s\", writes past the specified array length",
								 setlocale(LC_COLLATE, NULL)),
				 errhint("Apply system library package updates.")));
}


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
		collation_cache = hash_create("Collation cache", 100, &ctl,
									  HASH_ELEM | HASH_BLOBS);
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
	int			save_errno;

	/*
	 * Windows doesn't provide any useful error indication from
	 * _create_locale(), and BSD-derived platforms don't seem to feel they
	 * need to set errno either (even though POSIX is pretty clear that
	 * newlocale should do so).  So, if errno hasn't been set, assume ENOENT
	 * is what to report.
	 */
	if (errno == 0)
		errno = ENOENT;

	/*
	 * ENOENT means "no such locale", not "no such file", so clarify that
	 * errno with an errdetail message.
	 */
	save_errno = errno;			/* auxiliary funcs might change errno */
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("could not create locale \"%s\": %m",
					localename),
			 (save_errno == ENOENT ?
			  errdetail("The operating system could not find any locale data for the locale name \"%s\".",
						localename) : 0)));
}
#endif							/* HAVE_LOCALE_T */


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
		HeapTuple	tp;
		Form_pg_collation collform;
		const char *collcollate;
		const char *collctype pg_attribute_unused();
		struct pg_locale_struct result;
		pg_locale_t resultp;
		Datum		collversion;
		bool		isnull;

		tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for collation %u", collid);
		collform = (Form_pg_collation) GETSTRUCT(tp);

		collcollate = NameStr(collform->collcollate);
		collctype = NameStr(collform->collctype);

		/* We'll fill in the result struct locally before allocating memory */
		memset(&result, 0, sizeof(result));
		result.provider = collform->collprovider;
		result.deterministic = collform->collisdeterministic;

		if (collform->collprovider == COLLPROVIDER_LIBC)
		{
#ifdef HAVE_LOCALE_T
			locale_t	loc;

			if (strcmp(collcollate, collctype) == 0)
			{
				/* Normal case where they're the same */
				errno = 0;
#ifndef WIN32
				loc = newlocale(LC_COLLATE_MASK | LC_CTYPE_MASK, collcollate,
								NULL);
#else
				loc = _create_locale(LC_ALL, collcollate);
#endif
				if (!loc)
					report_newlocale_failure(collcollate);
			}
			else
			{
#ifndef WIN32
				/* We need two newlocale() steps */
				locale_t	loc1;

				errno = 0;
				loc1 = newlocale(LC_COLLATE_MASK, collcollate, NULL);
				if (!loc1)
					report_newlocale_failure(collcollate);
				errno = 0;
				loc = newlocale(LC_CTYPE_MASK, collctype, loc1);
				if (!loc)
					report_newlocale_failure(collctype);
#else

				/*
				 * XXX The _create_locale() API doesn't appear to support
				 * this. Could perhaps be worked around by changing
				 * pg_locale_t to contain two separate fields.
				 */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("collations with different collate and ctype values are not supported on this platform")));
#endif
			}

			result.info.lt = loc;
#else							/* not HAVE_LOCALE_T */
			/* platform that doesn't support locale_t */
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("collation provider LIBC is not supported on this platform")));
#endif							/* not HAVE_LOCALE_T */
		}
		else if (collform->collprovider == COLLPROVIDER_ICU)
		{
#ifdef USE_ICU
			UCollator  *collator;
			UErrorCode	status;

			if (strcmp(collcollate, collctype) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("collations with different collate and ctype values are not supported by ICU")));

			status = U_ZERO_ERROR;
			collator = ucol_open(collcollate, &status);
			if (U_FAILURE(status))
				ereport(ERROR,
						(errmsg("could not open collator for locale \"%s\": %s",
								collcollate, u_errorName(status))));

			if (U_ICU_VERSION_MAJOR_NUM < 54)
				icu_set_collation_attributes(collator, collcollate);

			/* We will leak this string if we get an error below :-( */
			result.info.icu.locale = MemoryContextStrdup(TopMemoryContext,
														 collcollate);
			result.info.icu.ucol = collator;
#else							/* not USE_ICU */
			/* could get here if a collation was created by a build with ICU */
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("ICU is not supported in this build"), \
					 errhint("You need to rebuild PostgreSQL using --with-icu.")));
#endif							/* not USE_ICU */
		}

		collversion = SysCacheGetAttr(COLLOID, tp, Anum_pg_collation_collversion,
									  &isnull);
		if (!isnull)
		{
			char	   *actual_versionstr;
			char	   *collversionstr;

			actual_versionstr = get_collation_actual_version(collform->collprovider, collcollate);
			if (!actual_versionstr)
			{
				/*
				 * This could happen when specifying a version in CREATE
				 * COLLATION for a libc locale, or manually creating a mess in
				 * the catalogs.
				 */
				ereport(ERROR,
						(errmsg("collation \"%s\" has no actual version, but a version was specified",
								NameStr(collform->collname))));
			}
			collversionstr = TextDatumGetCString(collversion);

			if (strcmp(actual_versionstr, collversionstr) != 0)
				ereport(WARNING,
						(errmsg("collation \"%s\" has version mismatch",
								NameStr(collform->collname)),
						 errdetail("The collation in the database was created using version %s, "
								   "but the operating system provides version %s.",
								   collversionstr, actual_versionstr),
						 errhint("Rebuild all objects affected by this collation and run "
								 "ALTER COLLATION %s REFRESH VERSION, "
								 "or build PostgreSQL with the right library version.",
								 quote_qualified_identifier(get_namespace_name(collform->collnamespace),
															NameStr(collform->collname)))));
		}

		ReleaseSysCache(tp);

		/* We'll keep the pg_locale_t structures in TopMemoryContext */
		resultp = MemoryContextAlloc(TopMemoryContext, sizeof(*resultp));
		*resultp = result;

		cache_entry->locale = resultp;
	}

	return cache_entry->locale;
}

/*
 * Get provider-specific collation version string for the given collation from
 * the operating system/library.
 */
char *
get_collation_actual_version(char collprovider, const char *collcollate)
{
	char	   *collversion = NULL;

#ifdef USE_ICU
	if (collprovider == COLLPROVIDER_ICU)
	{
		UCollator  *collator;
		UErrorCode	status;
		UVersionInfo versioninfo;
		char		buf[U_MAX_VERSION_STRING_LENGTH];

		status = U_ZERO_ERROR;
		collator = ucol_open(collcollate, &status);
		if (U_FAILURE(status))
			ereport(ERROR,
					(errmsg("could not open collator for locale \"%s\": %s",
							collcollate, u_errorName(status))));
		ucol_getVersion(collator, versioninfo);
		ucol_close(collator);

		u_versionToString(versioninfo, buf);
		collversion = pstrdup(buf);
	}
	else
#endif
	if (collprovider == COLLPROVIDER_LIBC)
	{
#if defined(__GLIBC__)
		char	   *copy = pstrdup(collcollate);
		char	   *copy_suffix = strstr(copy, ".");
		bool		need_version = true;

		/*
		 * Check for names like C.UTF-8 by chopping off the encoding suffix on
		 * our temporary copy, so we can skip the version.
		 */
		if (copy_suffix)
			*copy_suffix = '\0';
		if (pg_strcasecmp("c", copy) == 0 ||
			pg_strcasecmp("posix", copy) == 0)
			need_version = false;
		pfree(copy);
		if (!need_version)
			return NULL;

		/* Use the glibc version because we don't have anything better. */
		collversion = pstrdup(gnu_get_libc_version());
#elif defined(WIN32) && _WIN32_WINNT >= 0x0600
		/*
		 * If we are targeting Windows Vista and above, we can ask for a name
		 * given a collation name (earlier versions required a location code
		 * that we don't have).
		 */
		NLSVERSIONINFOEX version = {sizeof(NLSVERSIONINFOEX)};
		WCHAR		wide_collcollate[LOCALE_NAME_MAX_LENGTH];

		/* These would be invalid arguments, but have no version. */
		if (pg_strcasecmp("c", collcollate) == 0 ||
			pg_strcasecmp("posix", collcollate) == 0)
			return NULL;

		/* For all other names, ask the OS. */
		MultiByteToWideChar(CP_ACP, 0, collcollate, -1, wide_collcollate,
							LOCALE_NAME_MAX_LENGTH);
		if (!GetNLSVersionEx(COMPARE_STRING, wide_collcollate, &version))
		{
			/*
			 * GetNLSVersionEx() wants a language tag such as "en-US", not a
			 * locale name like "English_United States.1252".  Until those
			 * values can be prevented from entering the system, or 100%
			 * reliably converted to the more useful tag format, tolerate the
			 * resulting error and report that we have no version data.
			 */
			if (GetLastError() == ERROR_INVALID_PARAMETER)
				return NULL;

			ereport(ERROR,
					(errmsg("could not get collation version for locale \"%s\": error code %lu",
							collcollate,
							GetLastError())));
		}
		collversion = psprintf("%d.%d,%d.%d",
							   (version.dwNLSVersion >> 8) & 0xFFFF,
							   version.dwNLSVersion & 0xFF,
							   (version.dwDefinedVersion >> 8) & 0xFFFF,
							   version.dwDefinedVersion & 0xFF);
#endif
	}

	return collversion;
}


#ifdef USE_ICU
/*
 * Converter object for converting between ICU's UChar strings and C strings
 * in database encoding.  Since the database encoding doesn't change, we only
 * need one of these per session.
 */
static UConverter *icu_converter = NULL;

static void
init_icu_converter(void)
{
	const char *icu_encoding_name;
	UErrorCode	status;
	UConverter *conv;

	if (icu_converter)
		return;					/* already done */

	icu_encoding_name = get_encoding_name_for_icu(GetDatabaseEncoding());
	if (!icu_encoding_name)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("encoding \"%s\" not supported by ICU",
						pg_encoding_to_char(GetDatabaseEncoding()))));

	status = U_ZERO_ERROR;
	conv = ucnv_open(icu_encoding_name, &status);
	if (U_FAILURE(status))
		ereport(ERROR,
				(errmsg("could not open ICU converter for encoding \"%s\": %s",
						icu_encoding_name, u_errorName(status))));

	icu_converter = conv;
}

/*
 * Convert a string in the database encoding into a string of UChars.
 *
 * The source string at buff is of length nbytes
 * (it needn't be nul-terminated)
 *
 * *buff_uchar receives a pointer to the palloc'd result string, and
 * the function's result is the number of UChars generated.
 *
 * The result string is nul-terminated, though most callers rely on the
 * result length instead.
 */
int32_t
icu_to_uchar(UChar **buff_uchar, const char *buff, size_t nbytes)
{
	UErrorCode	status;
	int32_t		len_uchar;

	init_icu_converter();

	status = U_ZERO_ERROR;
	len_uchar = ucnv_toUChars(icu_converter, NULL, 0,
							  buff, nbytes, &status);
	if (U_FAILURE(status) && status != U_BUFFER_OVERFLOW_ERROR)
		ereport(ERROR,
				(errmsg("%s failed: %s", "ucnv_toUChars", u_errorName(status))));

	*buff_uchar = palloc((len_uchar + 1) * sizeof(**buff_uchar));

	status = U_ZERO_ERROR;
	len_uchar = ucnv_toUChars(icu_converter, *buff_uchar, len_uchar + 1,
							  buff, nbytes, &status);
	if (U_FAILURE(status))
		ereport(ERROR,
				(errmsg("%s failed: %s", "ucnv_toUChars", u_errorName(status))));

	return len_uchar;
}

/*
 * Convert a string of UChars into the database encoding.
 *
 * The source string at buff_uchar is of length len_uchar
 * (it needn't be nul-terminated)
 *
 * *result receives a pointer to the palloc'd result string, and the
 * function's result is the number of bytes generated (not counting nul).
 *
 * The result string is nul-terminated.
 */
int32_t
icu_from_uchar(char **result, const UChar *buff_uchar, int32_t len_uchar)
{
	UErrorCode	status;
	int32_t		len_result;

	init_icu_converter();

	status = U_ZERO_ERROR;
	len_result = ucnv_fromUChars(icu_converter, NULL, 0,
								 buff_uchar, len_uchar, &status);
	if (U_FAILURE(status) && status != U_BUFFER_OVERFLOW_ERROR)
		ereport(ERROR,
				(errmsg("%s failed: %s", "ucnv_fromUChars",
						u_errorName(status))));

	*result = palloc(len_result + 1);

	status = U_ZERO_ERROR;
	len_result = ucnv_fromUChars(icu_converter, *result, len_result + 1,
								 buff_uchar, len_uchar, &status);
	if (U_FAILURE(status))
		ereport(ERROR,
				(errmsg("%s failed: %s", "ucnv_fromUChars",
						u_errorName(status))));

	return len_result;
}

/*
 * Parse collation attributes and apply them to the open collator.  This takes
 * a string like "und@colStrength=primary;colCaseLevel=yes" and parses and
 * applies the key-value arguments.
 *
 * Starting with ICU version 54, the attributes are processed automatically by
 * ucol_open(), so this is only necessary for emulating this behavior on older
 * versions.
 */
pg_attribute_unused()
static void
icu_set_collation_attributes(UCollator *collator, const char *loc)
{
	char	   *str = asc_tolower(loc, strlen(loc));

	str = strchr(str, '@');
	if (!str)
		return;
	str++;

	for (char *token = strtok(str, ";"); token; token = strtok(NULL, ";"))
	{
		char	   *e = strchr(token, '=');

		if (e)
		{
			char	   *name;
			char	   *value;
			UColAttribute uattr;
			UColAttributeValue uvalue;
			UErrorCode	status;

			status = U_ZERO_ERROR;

			*e = '\0';
			name = token;
			value = e + 1;

			/*
			 * See attribute name and value lists in ICU i18n/coll.cpp
			 */
			if (strcmp(name, "colstrength") == 0)
				uattr = UCOL_STRENGTH;
			else if (strcmp(name, "colbackwards") == 0)
				uattr = UCOL_FRENCH_COLLATION;
			else if (strcmp(name, "colcaselevel") == 0)
				uattr = UCOL_CASE_LEVEL;
			else if (strcmp(name, "colcasefirst") == 0)
				uattr = UCOL_CASE_FIRST;
			else if (strcmp(name, "colalternate") == 0)
				uattr = UCOL_ALTERNATE_HANDLING;
			else if (strcmp(name, "colnormalization") == 0)
				uattr = UCOL_NORMALIZATION_MODE;
			else if (strcmp(name, "colnumeric") == 0)
				uattr = UCOL_NUMERIC_COLLATION;
			else
				/* ignore if unknown */
				continue;

			if (strcmp(value, "primary") == 0)
				uvalue = UCOL_PRIMARY;
			else if (strcmp(value, "secondary") == 0)
				uvalue = UCOL_SECONDARY;
			else if (strcmp(value, "tertiary") == 0)
				uvalue = UCOL_TERTIARY;
			else if (strcmp(value, "quaternary") == 0)
				uvalue = UCOL_QUATERNARY;
			else if (strcmp(value, "identical") == 0)
				uvalue = UCOL_IDENTICAL;
			else if (strcmp(value, "no") == 0)
				uvalue = UCOL_OFF;
			else if (strcmp(value, "yes") == 0)
				uvalue = UCOL_ON;
			else if (strcmp(value, "shifted") == 0)
				uvalue = UCOL_SHIFTED;
			else if (strcmp(value, "non-ignorable") == 0)
				uvalue = UCOL_NON_IGNORABLE;
			else if (strcmp(value, "lower") == 0)
				uvalue = UCOL_LOWER_FIRST;
			else if (strcmp(value, "upper") == 0)
				uvalue = UCOL_UPPER_FIRST;
			else
				status = U_ILLEGAL_ARGUMENT_ERROR;

			if (status == U_ZERO_ERROR)
				ucol_setAttribute(collator, uattr, uvalue, &status);

			/*
			 * Pretend the error came from ucol_open(), for consistent error
			 * message across ICU versions.
			 */
			if (U_FAILURE(status))
				ereport(ERROR,
						(errmsg("could not open collator for locale \"%s\": %s",
								loc, u_errorName(status))));
		}
	}
}

#endif							/* USE_ICU */

/*
 * These functions convert from/to libc's wchar_t, *not* pg_wchar_t.
 * Therefore we keep them here rather than with the mbutils code.
 */

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

	Assert(!locale || locale->provider == COLLPROVIDER_LIBC);

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
#endif							/* WIN32 */
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
		result = wcstombs_l(to, from, tolen, locale->info.lt);
#else							/* !HAVE_WCSTOMBS_L */
		/* We have to temporarily set the locale as current ... ugh */
		locale_t	save_locale = uselocale(locale->info.lt);

		result = wcstombs(to, from, tolen);

		uselocale(save_locale);
#endif							/* HAVE_WCSTOMBS_L */
#else							/* !HAVE_LOCALE_T */
		/* Can't have locale != 0 without HAVE_LOCALE_T */
		elog(ERROR, "wcstombs_l is not available");
		result = 0;				/* keep compiler quiet */
#endif							/* HAVE_LOCALE_T */
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

	Assert(!locale || locale->provider == COLLPROVIDER_LIBC);

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
#endif							/* WIN32 */
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
			result = mbstowcs_l(to, str, tolen, locale->info.lt);
#else							/* !HAVE_MBSTOWCS_L */
			/* We have to temporarily set the locale as current ... ugh */
			locale_t	save_locale = uselocale(locale->info.lt);

			result = mbstowcs(to, str, tolen);

			uselocale(save_locale);
#endif							/* HAVE_MBSTOWCS_L */
#else							/* !HAVE_LOCALE_T */
			/* Can't have locale != 0 without HAVE_LOCALE_T */
			elog(ERROR, "mbstowcs_l is not available");
			result = 0;			/* keep compiler quiet */
#endif							/* HAVE_LOCALE_T */
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
		 * database encoding.  Give a generic error message if pg_verifymbstr
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
