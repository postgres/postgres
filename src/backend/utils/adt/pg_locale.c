/*-----------------------------------------------------------------------
 *
 * PostgreSQL locale utilities
 *
 * Portions Copyright (c) 2002-2024, PostgreSQL Global Development Group
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
#include "catalog/pg_database.h"
#include "common/hashfn.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/formatting.h"
#include "utils/guc_hooks.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/syscache.h"

#ifdef USE_ICU
#include <unicode/ucnv.h>
#include <unicode/ustring.h>
#endif

#ifdef __GLIBC__
#include <gnu/libc-version.h>
#endif

#ifdef WIN32
#include <shlwapi.h>
#endif

/* Error triggered for locale-sensitive subroutines */
#define		PGLOCALE_SUPPORT_ERROR(provider) \
	elog(ERROR, "unsupported collprovider for %s: %c", __func__, provider)

/*
 * This should be large enough that most strings will fit, but small enough
 * that we feel comfortable putting it on the stack
 */
#define		TEXTBUFLEN			1024

#define		MAX_L10N_DATA		80


/* GUC settings */
char	   *locale_messages;
char	   *locale_monetary;
char	   *locale_numeric;
char	   *locale_time;

int			icu_validation_level = WARNING;

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

/* is the databases's LC_CTYPE the C locale? */
bool		database_ctype_is_c = false;

static struct pg_locale_struct default_locale;

/* indicates whether locale information cache is valid */
static bool CurrentLocaleConvValid = false;
static bool CurrentLCTimeValid = false;

/* Cache for collation-related knowledge */

typedef struct
{
	Oid			collid;			/* hash key: pg_collation OID */
	pg_locale_t locale;			/* locale_t struct, or 0 if not valid */

	/* needed for simplehash */
	uint32		hash;
	char		status;
} collation_cache_entry;

#define SH_PREFIX		collation_cache
#define SH_ELEMENT_TYPE	collation_cache_entry
#define SH_KEY_TYPE		Oid
#define SH_KEY			collid
#define SH_HASH_KEY(tb, key)   	murmurhash32((uint32) key)
#define SH_EQUAL(tb, a, b)		(a == b)
#define SH_GET_HASH(tb, a)		a->hash
#define SH_SCOPE		static inline
#define SH_STORE_HASH
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

static MemoryContext CollationCacheContext = NULL;
static collation_cache_hash *CollationCache = NULL;

#if defined(WIN32) && defined(LC_MESSAGES)
static char *IsoLocaleName(const char *);
#endif

#ifdef USE_ICU
/*
 * Converter object for converting between ICU's UChar strings and C strings
 * in database encoding.  Since the database encoding doesn't change, we only
 * need one of these per session.
 */
static UConverter *icu_converter = NULL;

static UCollator *pg_ucol_open(const char *loc_str);
static void init_icu_converter(void);
static size_t uchar_length(UConverter *converter,
						   const char *str, int32_t len);
static int32_t uchar_convert(UConverter *converter,
							 UChar *dest, int32_t destlen,
							 const char *src, int32_t srclen);
static void icu_set_collation_attributes(UCollator *collator, const char *loc,
										 UErrorCode *status);
#endif

/*
 * POSIX doesn't define _l-variants of these functions, but several systems
 * have them.  We provide our own replacements here.
 */
#ifndef HAVE_MBSTOWCS_L
static size_t
mbstowcs_l(wchar_t *dest, const char *src, size_t n, locale_t loc)
{
#ifdef WIN32
	return _mbstowcs_l(dest, src, n, loc);
#else
	size_t		result;
	locale_t	save_locale = uselocale(loc);

	result = mbstowcs(dest, src, n);
	uselocale(save_locale);
	return result;
#endif
}
#endif
#ifndef HAVE_WCSTOMBS_L
static size_t
wcstombs_l(char *dest, const wchar_t *src, size_t n, locale_t loc)
{
#ifdef WIN32
	return _wcstombs_l(dest, src, n, loc);
#else
	size_t		result;
	locale_t	save_locale = uselocale(loc);

	result = wcstombs(dest, src, n);
	uselocale(save_locale);
	return result;
#endif
}
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
		static char save_lc_ctype[LOCALE_NAME_BUFLEN];

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
			break;
		case LC_CTYPE:
			envvar = "LC_CTYPE";
			break;
#ifdef LC_MESSAGES
		case LC_MESSAGES:
			envvar = "LC_MESSAGES";
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
			break;
		case LC_NUMERIC:
			envvar = "LC_NUMERIC";
			break;
		case LC_TIME:
			envvar = "LC_TIME";
			break;
		default:
			elog(FATAL, "unrecognized LC category: %d", category);
			return NULL;		/* keep compiler quiet */
	}

	if (setenv(envvar, result, 1) != 0)
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
	free(s->decimal_point);
	free(s->thousands_sep);
	free(s->grouping);
	free(s->int_curr_symbol);
	free(s->currency_symbol);
	free(s->mon_decimal_point);
	free(s->mon_thousands_sep);
	free(s->mon_grouping);
	free(s->positive_sign);
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
	struct tm	timeinfobuf;
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
	timeinfo = gmtime_r(&timenow, &timeinfobuf);

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

#if defined(_MSC_VER)

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
		 * If the enumerated locale does not have a hyphen ("en") OR the
		 * locale_name input does not have an underscore ("English"), we only
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
	 * If the lc_messages is already a Unix-style string, we have a direct
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
		 * Since the message catalogs sit on a case-insensitive filesystem, we
		 * need not standardize letter case here.  So long as we do not ship
		 * message catalogs for which it would matter, we also need not
		 * translate the script/variant portion, e.g.  uz-Cyrl-UZ to
		 * uz_UZ@cyrillic.  Simply replace the hyphen with an underscore.
		 */
		hyphen = strchr(iso_lc_messages, '-');
		if (hyphen)
			*hyphen = '_';
		return iso_lc_messages;
	}

	return NULL;
}

static char *
IsoLocaleName(const char *winlocname)
{
	static char iso_lc_messages[LOCALE_NAME_MAX_LENGTH];

	if (pg_strcasecmp("c", winlocname) == 0 ||
		pg_strcasecmp("posix", winlocname) == 0)
	{
		strcpy(iso_lc_messages, "C");
		return iso_lc_messages;
	}
	else
		return get_iso_localename(winlocname);
}

#else							/* !defined(_MSC_VER) */

static char *
IsoLocaleName(const char *winlocname)
{
	return NULL;				/* Not supported on MinGW */
}

#endif							/* defined(_MSC_VER) */

#endif							/* WIN32 && LC_MESSAGES */


/*
 * Cache mechanism for collation information.
 *
 * Note that we currently lack any way to flush the cache.  Since we don't
 * support ALTER COLLATION, this is OK.  The worst case is that someone
 * drops a collation, and a useless cache entry hangs around in existing
 * backends.
 */
static collation_cache_entry *
lookup_collation_cache(Oid collation)
{
	collation_cache_entry *cache_entry;
	bool		found;

	Assert(OidIsValid(collation));
	Assert(collation != DEFAULT_COLLATION_OID);

	if (CollationCache == NULL)
	{
		CollationCacheContext = AllocSetContextCreate(TopMemoryContext,
													  "collation cache",
													  ALLOCSET_DEFAULT_SIZES);
		CollationCache = collation_cache_create(CollationCacheContext,
												16, NULL);
	}

	cache_entry = collation_cache_insert(CollationCache, collation, &found);
	if (!found)
	{
		/*
		 * Make sure cache entry is marked invalid, in case we fail before
		 * setting things.
		 */
		cache_entry->locale = 0;
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
	 * If we're asked about the built-in C/POSIX collations, we know that.
	 */
	if (collation == C_COLLATION_OID ||
		collation == POSIX_COLLATION_OID)
		return true;

	/*
	 * Otherwise, we have to consult pg_collation, but we cache that.
	 */
	return pg_newlocale_from_collation(collation)->collate_is_c;
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
	 * If we're asked about the built-in C/POSIX collations, we know that.
	 */
	if (collation == C_COLLATION_OID ||
		collation == POSIX_COLLATION_OID)
		return true;

	/*
	 * Otherwise, we have to consult pg_collation, but we cache that.
	 */
	return pg_newlocale_from_collation(collation)->ctype_is_c;
}

/* simple subroutine for reporting errors from newlocale() */
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

/*
 * Initialize the locale_t field.
 *
 * The "C" and "POSIX" locales are not actually handled by libc, so set the
 * locale_t to zero in that case.
 */
static void
make_libc_collator(const char *collate, const char *ctype,
				   pg_locale_t result)
{
	locale_t	loc = 0;

	if (strcmp(collate, ctype) == 0)
	{
		if (strcmp(ctype, "C") != 0 && strcmp(ctype, "POSIX") != 0)
		{
			/* Normal case where they're the same */
			errno = 0;
#ifndef WIN32
			loc = newlocale(LC_COLLATE_MASK | LC_CTYPE_MASK, collate,
							NULL);
#else
			loc = _create_locale(LC_ALL, collate);
#endif
			if (!loc)
				report_newlocale_failure(collate);
		}
	}
	else
	{
#ifndef WIN32
		/* We need two newlocale() steps */
		locale_t	loc1 = 0;

		if (strcmp(collate, "C") != 0 && strcmp(collate, "POSIX") != 0)
		{
			errno = 0;
			loc1 = newlocale(LC_COLLATE_MASK, collate, NULL);
			if (!loc1)
				report_newlocale_failure(collate);
		}

		if (strcmp(ctype, "C") != 0 && strcmp(ctype, "POSIX") != 0)
		{
			errno = 0;
			loc = newlocale(LC_CTYPE_MASK, ctype, loc1);
			if (!loc)
				report_newlocale_failure(ctype);
		}
		else
			loc = loc1;
#else

		/*
		 * XXX The _create_locale() API doesn't appear to support this. Could
		 * perhaps be worked around by changing pg_locale_t to contain two
		 * separate fields.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("collations with different collate and ctype values are not supported on this platform")));
#endif
	}

	result->info.lt = loc;
}

void
make_icu_collator(const char *iculocstr,
				  const char *icurules,
				  struct pg_locale_struct *resultp)
{
#ifdef USE_ICU
	UCollator  *collator;

	collator = pg_ucol_open(iculocstr);

	/*
	 * If rules are specified, we extract the rules of the standard collation,
	 * add our own rules, and make a new collator with the combined rules.
	 */
	if (icurules)
	{
		const UChar *default_rules;
		UChar	   *agg_rules;
		UChar	   *my_rules;
		UErrorCode	status;
		int32_t		length;

		default_rules = ucol_getRules(collator, &length);
		icu_to_uchar(&my_rules, icurules, strlen(icurules));

		agg_rules = palloc_array(UChar, u_strlen(default_rules) + u_strlen(my_rules) + 1);
		u_strcpy(agg_rules, default_rules);
		u_strcat(agg_rules, my_rules);

		ucol_close(collator);

		status = U_ZERO_ERROR;
		collator = ucol_openRules(agg_rules, u_strlen(agg_rules),
								  UCOL_DEFAULT, UCOL_DEFAULT_STRENGTH, NULL, &status);
		if (U_FAILURE(status))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not open collator for locale \"%s\" with rules \"%s\": %s",
							iculocstr, icurules, u_errorName(status))));
	}

	/* We will leak this string if the caller errors later :-( */
	resultp->info.icu.locale = MemoryContextStrdup(TopMemoryContext, iculocstr);
	resultp->info.icu.ucol = collator;
#else							/* not USE_ICU */
	/* could get here if a collation was created by a build with ICU */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("ICU is not supported in this build")));
#endif							/* not USE_ICU */
}


bool
pg_locale_deterministic(pg_locale_t locale)
{
	return locale->deterministic;
}

/*
 * Initialize default_locale with database locale settings.
 */
void
init_database_collation(void)
{
	HeapTuple	tup;
	Form_pg_database dbform;
	Datum		datum;
	bool		isnull;

	/* Fetch our pg_database row normally, via syscache */
	tup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for database %u", MyDatabaseId);
	dbform = (Form_pg_database) GETSTRUCT(tup);

	if (dbform->datlocprovider == COLLPROVIDER_BUILTIN)
	{
		char	   *datlocale;

		datum = SysCacheGetAttrNotNull(DATABASEOID, tup, Anum_pg_database_datlocale);
		datlocale = TextDatumGetCString(datum);

		builtin_validate_locale(dbform->encoding, datlocale);

		default_locale.collate_is_c = true;
		default_locale.ctype_is_c = (strcmp(datlocale, "C") == 0);

		default_locale.info.builtin.locale = MemoryContextStrdup(
																 TopMemoryContext, datlocale);
	}
	else if (dbform->datlocprovider == COLLPROVIDER_ICU)
	{
		char	   *datlocale;
		char	   *icurules;

		datum = SysCacheGetAttrNotNull(DATABASEOID, tup, Anum_pg_database_datlocale);
		datlocale = TextDatumGetCString(datum);

		default_locale.collate_is_c = false;
		default_locale.ctype_is_c = false;

		datum = SysCacheGetAttr(DATABASEOID, tup, Anum_pg_database_daticurules, &isnull);
		if (!isnull)
			icurules = TextDatumGetCString(datum);
		else
			icurules = NULL;

		make_icu_collator(datlocale, icurules, &default_locale);
	}
	else
	{
		const char *datcollate;
		const char *datctype;

		Assert(dbform->datlocprovider == COLLPROVIDER_LIBC);

		datum = SysCacheGetAttrNotNull(DATABASEOID, tup, Anum_pg_database_datcollate);
		datcollate = TextDatumGetCString(datum);
		datum = SysCacheGetAttrNotNull(DATABASEOID, tup, Anum_pg_database_datctype);
		datctype = TextDatumGetCString(datum);

		default_locale.collate_is_c = (strcmp(datcollate, "C") == 0) ||
			(strcmp(datcollate, "POSIX") == 0);
		default_locale.ctype_is_c = (strcmp(datctype, "C") == 0) ||
			(strcmp(datctype, "POSIX") == 0);

		make_libc_collator(datcollate, datctype, &default_locale);
	}

	default_locale.provider = dbform->datlocprovider;

	/*
	 * Default locale is currently always deterministic.  Nondeterministic
	 * locales currently don't support pattern matching, which would break a
	 * lot of things if applied globally.
	 */
	default_locale.deterministic = true;

	ReleaseSysCache(tup);
}

/*
 * Create a pg_locale_t from a collation OID.  Results are cached for the
 * lifetime of the backend.  Thus, do not free the result with freelocale().
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

	if (collid == DEFAULT_COLLATION_OID)
		return &default_locale;

	cache_entry = lookup_collation_cache(collid);

	if (cache_entry->locale == 0)
	{
		/* We haven't computed this yet in this session, so do it */
		HeapTuple	tp;
		Form_pg_collation collform;
		struct pg_locale_struct result;
		pg_locale_t resultp;
		Datum		datum;
		bool		isnull;

		tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for collation %u", collid);
		collform = (Form_pg_collation) GETSTRUCT(tp);

		/* We'll fill in the result struct locally before allocating memory */
		memset(&result, 0, sizeof(result));
		result.provider = collform->collprovider;
		result.deterministic = collform->collisdeterministic;

		if (collform->collprovider == COLLPROVIDER_BUILTIN)
		{
			const char *locstr;

			datum = SysCacheGetAttrNotNull(COLLOID, tp, Anum_pg_collation_colllocale);
			locstr = TextDatumGetCString(datum);

			result.collate_is_c = true;
			result.ctype_is_c = (strcmp(locstr, "C") == 0);

			builtin_validate_locale(GetDatabaseEncoding(), locstr);

			result.info.builtin.locale = MemoryContextStrdup(TopMemoryContext,
															 locstr);
		}
		else if (collform->collprovider == COLLPROVIDER_LIBC)
		{
			const char *collcollate;
			const char *collctype;

			datum = SysCacheGetAttrNotNull(COLLOID, tp, Anum_pg_collation_collcollate);
			collcollate = TextDatumGetCString(datum);
			datum = SysCacheGetAttrNotNull(COLLOID, tp, Anum_pg_collation_collctype);
			collctype = TextDatumGetCString(datum);

			result.collate_is_c = (strcmp(collcollate, "C") == 0) ||
				(strcmp(collcollate, "POSIX") == 0);
			result.ctype_is_c = (strcmp(collctype, "C") == 0) ||
				(strcmp(collctype, "POSIX") == 0);

			make_libc_collator(collcollate, collctype, &result);
		}
		else if (collform->collprovider == COLLPROVIDER_ICU)
		{
			const char *iculocstr;
			const char *icurules;

			datum = SysCacheGetAttrNotNull(COLLOID, tp, Anum_pg_collation_colllocale);
			iculocstr = TextDatumGetCString(datum);

			result.collate_is_c = false;
			result.ctype_is_c = false;

			datum = SysCacheGetAttr(COLLOID, tp, Anum_pg_collation_collicurules, &isnull);
			if (!isnull)
				icurules = TextDatumGetCString(datum);
			else
				icurules = NULL;

			make_icu_collator(iculocstr, icurules, &result);
		}

		datum = SysCacheGetAttr(COLLOID, tp, Anum_pg_collation_collversion,
								&isnull);
		if (!isnull)
		{
			char	   *actual_versionstr;
			char	   *collversionstr;

			collversionstr = TextDatumGetCString(datum);

			if (collform->collprovider == COLLPROVIDER_LIBC)
				datum = SysCacheGetAttrNotNull(COLLOID, tp, Anum_pg_collation_collcollate);
			else
				datum = SysCacheGetAttrNotNull(COLLOID, tp, Anum_pg_collation_colllocale);

			actual_versionstr = get_collation_actual_version(collform->collprovider,
															 TextDatumGetCString(datum));
			if (!actual_versionstr)
			{
				/*
				 * This could happen when specifying a version in CREATE
				 * COLLATION but the provider does not support versioning, or
				 * manually creating a mess in the catalogs.
				 */
				ereport(ERROR,
						(errmsg("collation \"%s\" has no actual version, but a version was recorded",
								NameStr(collform->collname))));
			}

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

	/*
	 * The only two supported locales (C and C.UTF-8) are both based on memcmp
	 * and are not expected to change, but track the version anyway.
	 *
	 * Note that the character semantics may change for some locales, but the
	 * collation version only tracks changes to sort order.
	 */
	if (collprovider == COLLPROVIDER_BUILTIN)
	{
		if (strcmp(collcollate, "C") == 0)
			return "1";
		else if (strcmp(collcollate, "C.UTF-8") == 0)
			return "1";
		else
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("invalid locale name \"%s\" for builtin provider",
							collcollate)));
	}

#ifdef USE_ICU
	if (collprovider == COLLPROVIDER_ICU)
	{
		UCollator  *collator;
		UVersionInfo versioninfo;
		char		buf[U_MAX_VERSION_STRING_LENGTH];

		collator = pg_ucol_open(collcollate);

		ucol_getVersion(collator, versioninfo);
		ucol_close(collator);

		u_versionToString(versioninfo, buf);
		collversion = pstrdup(buf);
	}
	else
#endif
		if (collprovider == COLLPROVIDER_LIBC &&
			pg_strcasecmp("C", collcollate) != 0 &&
			pg_strncasecmp("C.", collcollate, 2) != 0 &&
			pg_strcasecmp("POSIX", collcollate) != 0)
	{
#if defined(__GLIBC__)
		/* Use the glibc version because we don't have anything better. */
		collversion = pstrdup(gnu_get_libc_version());
#elif defined(LC_VERSION_MASK)
		locale_t	loc;

		/* Look up FreeBSD collation version. */
		loc = newlocale(LC_COLLATE_MASK, collcollate, NULL);
		if (loc)
		{
			collversion =
				pstrdup(querylocale(LC_COLLATE_MASK | LC_VERSION_MASK, loc));
			freelocale(loc);
		}
		else
			ereport(ERROR,
					(errmsg("could not load locale \"%s\"", collcollate)));
#elif defined(WIN32)
		/*
		 * If we are targeting Windows Vista and above, we can ask for a name
		 * given a collation name (earlier versions required a location code
		 * that we don't have).
		 */
		NLSVERSIONINFOEX version = {sizeof(NLSVERSIONINFOEX)};
		WCHAR		wide_collcollate[LOCALE_NAME_MAX_LENGTH];

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
		collversion = psprintf("%lu.%lu,%lu.%lu",
							   (version.dwNLSVersion >> 8) & 0xFFFF,
							   version.dwNLSVersion & 0xFF,
							   (version.dwDefinedVersion >> 8) & 0xFFFF,
							   version.dwDefinedVersion & 0xFF);
#endif
	}

	return collversion;
}

/*
 * pg_strncoll_libc_win32_utf8
 *
 * Win32 does not have UTF-8. Convert UTF8 arguments to wide characters and
 * invoke wcscoll_l().
 */
#ifdef WIN32
static int
pg_strncoll_libc_win32_utf8(const char *arg1, size_t len1, const char *arg2,
							size_t len2, pg_locale_t locale)
{
	char		sbuf[TEXTBUFLEN];
	char	   *buf = sbuf;
	char	   *a1p,
			   *a2p;
	int			a1len = len1 * 2 + 2;
	int			a2len = len2 * 2 + 2;
	int			r;
	int			result;

	Assert(locale->provider == COLLPROVIDER_LIBC);
	Assert(GetDatabaseEncoding() == PG_UTF8);
#ifndef WIN32
	Assert(false);
#endif

	if (a1len + a2len > TEXTBUFLEN)
		buf = palloc(a1len + a2len);

	a1p = buf;
	a2p = buf + a1len;

	/* API does not work for zero-length input */
	if (len1 == 0)
		r = 0;
	else
	{
		r = MultiByteToWideChar(CP_UTF8, 0, arg1, len1,
								(LPWSTR) a1p, a1len / 2);
		if (!r)
			ereport(ERROR,
					(errmsg("could not convert string to UTF-16: error code %lu",
							GetLastError())));
	}
	((LPWSTR) a1p)[r] = 0;

	if (len2 == 0)
		r = 0;
	else
	{
		r = MultiByteToWideChar(CP_UTF8, 0, arg2, len2,
								(LPWSTR) a2p, a2len / 2);
		if (!r)
			ereport(ERROR,
					(errmsg("could not convert string to UTF-16: error code %lu",
							GetLastError())));
	}
	((LPWSTR) a2p)[r] = 0;

	errno = 0;
	result = wcscoll_l((LPWSTR) a1p, (LPWSTR) a2p, locale->info.lt);
	if (result == 2147483647)	/* _NLSCMPERROR; missing from mingw headers */
		ereport(ERROR,
				(errmsg("could not compare Unicode strings: %m")));

	if (buf != sbuf)
		pfree(buf);

	return result;
}
#endif							/* WIN32 */

/*
 * pg_strcoll_libc
 *
 * Call strcoll_l() or wcscoll_l() as appropriate for the given locale,
 * platform, and database encoding. If the locale is NULL, use the database
 * collation.
 *
 * Arguments must be encoded in the database encoding and nul-terminated.
 */
static int
pg_strcoll_libc(const char *arg1, const char *arg2, pg_locale_t locale)
{
	int			result;

	Assert(locale->provider == COLLPROVIDER_LIBC);
#ifdef WIN32
	if (GetDatabaseEncoding() == PG_UTF8)
	{
		size_t		len1 = strlen(arg1);
		size_t		len2 = strlen(arg2);

		result = pg_strncoll_libc_win32_utf8(arg1, len1, arg2, len2, locale);
	}
	else
#endif							/* WIN32 */
		result = strcoll_l(arg1, arg2, locale->info.lt);

	return result;
}

/*
 * pg_strncoll_libc
 *
 * Nul-terminate the arguments and call pg_strcoll_libc().
 */
static int
pg_strncoll_libc(const char *arg1, size_t len1, const char *arg2, size_t len2,
				 pg_locale_t locale)
{
	char		sbuf[TEXTBUFLEN];
	char	   *buf = sbuf;
	size_t		bufsize1 = len1 + 1;
	size_t		bufsize2 = len2 + 1;
	char	   *arg1n;
	char	   *arg2n;
	int			result;

	Assert(locale->provider == COLLPROVIDER_LIBC);

#ifdef WIN32
	/* check for this case before doing the work for nul-termination */
	if (GetDatabaseEncoding() == PG_UTF8)
		return pg_strncoll_libc_win32_utf8(arg1, len1, arg2, len2, locale);
#endif							/* WIN32 */

	if (bufsize1 + bufsize2 > TEXTBUFLEN)
		buf = palloc(bufsize1 + bufsize2);

	arg1n = buf;
	arg2n = buf + bufsize1;

	/* nul-terminate arguments */
	memcpy(arg1n, arg1, len1);
	arg1n[len1] = '\0';
	memcpy(arg2n, arg2, len2);
	arg2n[len2] = '\0';

	result = pg_strcoll_libc(arg1n, arg2n, locale);

	if (buf != sbuf)
		pfree(buf);

	return result;
}

#ifdef USE_ICU

/*
 * pg_strncoll_icu_no_utf8
 *
 * Convert the arguments from the database encoding to UChar strings, then
 * call ucol_strcoll(). An argument length of -1 means that the string is
 * NUL-terminated.
 *
 * When the database encoding is UTF-8, and ICU supports ucol_strcollUTF8(),
 * caller should call that instead.
 */
static int
pg_strncoll_icu_no_utf8(const char *arg1, int32_t len1,
						const char *arg2, int32_t len2, pg_locale_t locale)
{
	char		sbuf[TEXTBUFLEN];
	char	   *buf = sbuf;
	int32_t		ulen1;
	int32_t		ulen2;
	size_t		bufsize1;
	size_t		bufsize2;
	UChar	   *uchar1,
			   *uchar2;
	int			result;

	Assert(locale->provider == COLLPROVIDER_ICU);
#ifdef HAVE_UCOL_STRCOLLUTF8
	Assert(GetDatabaseEncoding() != PG_UTF8);
#endif

	init_icu_converter();

	ulen1 = uchar_length(icu_converter, arg1, len1);
	ulen2 = uchar_length(icu_converter, arg2, len2);

	bufsize1 = (ulen1 + 1) * sizeof(UChar);
	bufsize2 = (ulen2 + 1) * sizeof(UChar);

	if (bufsize1 + bufsize2 > TEXTBUFLEN)
		buf = palloc(bufsize1 + bufsize2);

	uchar1 = (UChar *) buf;
	uchar2 = (UChar *) (buf + bufsize1);

	ulen1 = uchar_convert(icu_converter, uchar1, ulen1 + 1, arg1, len1);
	ulen2 = uchar_convert(icu_converter, uchar2, ulen2 + 1, arg2, len2);

	result = ucol_strcoll(locale->info.icu.ucol,
						  uchar1, ulen1,
						  uchar2, ulen2);

	if (buf != sbuf)
		pfree(buf);

	return result;
}

/*
 * pg_strncoll_icu
 *
 * Call ucol_strcollUTF8() or ucol_strcoll() as appropriate for the given
 * database encoding. An argument length of -1 means the string is
 * NUL-terminated.
 *
 * Arguments must be encoded in the database encoding.
 */
static int
pg_strncoll_icu(const char *arg1, int32_t len1, const char *arg2, int32_t len2,
				pg_locale_t locale)
{
	int			result;

	Assert(locale->provider == COLLPROVIDER_ICU);

#ifdef HAVE_UCOL_STRCOLLUTF8
	if (GetDatabaseEncoding() == PG_UTF8)
	{
		UErrorCode	status;

		status = U_ZERO_ERROR;
		result = ucol_strcollUTF8(locale->info.icu.ucol,
								  arg1, len1,
								  arg2, len2,
								  &status);
		if (U_FAILURE(status))
			ereport(ERROR,
					(errmsg("collation failed: %s", u_errorName(status))));
	}
	else
#endif
	{
		result = pg_strncoll_icu_no_utf8(arg1, len1, arg2, len2, locale);
	}

	return result;
}

#endif							/* USE_ICU */

/*
 * pg_strcoll
 *
 * Call ucol_strcollUTF8(), ucol_strcoll(), strcoll_l() or wcscoll_l() as
 * appropriate for the given locale, platform, and database encoding. If the
 * locale is not specified, use the database collation.
 *
 * Arguments must be encoded in the database encoding and nul-terminated.
 *
 * The caller is responsible for breaking ties if the collation is
 * deterministic; this maintains consistency with pg_strxfrm(), which cannot
 * easily account for deterministic collations.
 */
int
pg_strcoll(const char *arg1, const char *arg2, pg_locale_t locale)
{
	int			result;

	if (locale->provider == COLLPROVIDER_LIBC)
		result = pg_strcoll_libc(arg1, arg2, locale);
#ifdef USE_ICU
	else if (locale->provider == COLLPROVIDER_ICU)
		result = pg_strncoll_icu(arg1, -1, arg2, -1, locale);
#endif
	else
		/* shouldn't happen */
		PGLOCALE_SUPPORT_ERROR(locale->provider);

	return result;
}

/*
 * pg_strncoll
 *
 * Call ucol_strcollUTF8(), ucol_strcoll(), strcoll_l() or wcscoll_l() as
 * appropriate for the given locale, platform, and database encoding. If the
 * locale is not specified, use the database collation.
 *
 * Arguments must be encoded in the database encoding.
 *
 * This function may need to nul-terminate the arguments for libc functions;
 * so if the caller already has nul-terminated strings, it should call
 * pg_strcoll() instead.
 *
 * The caller is responsible for breaking ties if the collation is
 * deterministic; this maintains consistency with pg_strnxfrm(), which cannot
 * easily account for deterministic collations.
 */
int
pg_strncoll(const char *arg1, size_t len1, const char *arg2, size_t len2,
			pg_locale_t locale)
{
	int			result;

	if (locale->provider == COLLPROVIDER_LIBC)
		result = pg_strncoll_libc(arg1, len1, arg2, len2, locale);
#ifdef USE_ICU
	else if (locale->provider == COLLPROVIDER_ICU)
		result = pg_strncoll_icu(arg1, len1, arg2, len2, locale);
#endif
	else
		/* shouldn't happen */
		PGLOCALE_SUPPORT_ERROR(locale->provider);

	return result;
}


static size_t
pg_strxfrm_libc(char *dest, const char *src, size_t destsize,
				pg_locale_t locale)
{
	Assert(locale->provider == COLLPROVIDER_LIBC);
	return strxfrm_l(dest, src, destsize, locale->info.lt);
}

static size_t
pg_strnxfrm_libc(char *dest, const char *src, size_t srclen, size_t destsize,
				 pg_locale_t locale)
{
	char		sbuf[TEXTBUFLEN];
	char	   *buf = sbuf;
	size_t		bufsize = srclen + 1;
	size_t		result;

	Assert(locale->provider == COLLPROVIDER_LIBC);

	if (bufsize > TEXTBUFLEN)
		buf = palloc(bufsize);

	/* nul-terminate arguments */
	memcpy(buf, src, srclen);
	buf[srclen] = '\0';

	result = pg_strxfrm_libc(dest, buf, destsize, locale);

	if (buf != sbuf)
		pfree(buf);

	/* if dest is defined, it should be nul-terminated */
	Assert(result >= destsize || dest[result] == '\0');

	return result;
}

#ifdef USE_ICU

/* 'srclen' of -1 means the strings are NUL-terminated */
static size_t
pg_strnxfrm_icu(char *dest, const char *src, int32_t srclen, int32_t destsize,
				pg_locale_t locale)
{
	char		sbuf[TEXTBUFLEN];
	char	   *buf = sbuf;
	UChar	   *uchar;
	int32_t		ulen;
	size_t		uchar_bsize;
	Size		result_bsize;

	Assert(locale->provider == COLLPROVIDER_ICU);

	init_icu_converter();

	ulen = uchar_length(icu_converter, src, srclen);

	uchar_bsize = (ulen + 1) * sizeof(UChar);

	if (uchar_bsize > TEXTBUFLEN)
		buf = palloc(uchar_bsize);

	uchar = (UChar *) buf;

	ulen = uchar_convert(icu_converter, uchar, ulen + 1, src, srclen);

	result_bsize = ucol_getSortKey(locale->info.icu.ucol,
								   uchar, ulen,
								   (uint8_t *) dest, destsize);

	/*
	 * ucol_getSortKey() counts the nul-terminator in the result length, but
	 * this function should not.
	 */
	Assert(result_bsize > 0);
	result_bsize--;

	if (buf != sbuf)
		pfree(buf);

	/* if dest is defined, it should be nul-terminated */
	Assert(result_bsize >= destsize || dest[result_bsize] == '\0');

	return result_bsize;
}

/* 'srclen' of -1 means the strings are NUL-terminated */
static size_t
pg_strnxfrm_prefix_icu_no_utf8(char *dest, const char *src, int32_t srclen,
							   int32_t destsize, pg_locale_t locale)
{
	char		sbuf[TEXTBUFLEN];
	char	   *buf = sbuf;
	UCharIterator iter;
	uint32_t	state[2];
	UErrorCode	status;
	int32_t		ulen = -1;
	UChar	   *uchar = NULL;
	size_t		uchar_bsize;
	Size		result_bsize;

	Assert(locale->provider == COLLPROVIDER_ICU);
	Assert(GetDatabaseEncoding() != PG_UTF8);

	init_icu_converter();

	ulen = uchar_length(icu_converter, src, srclen);

	uchar_bsize = (ulen + 1) * sizeof(UChar);

	if (uchar_bsize > TEXTBUFLEN)
		buf = palloc(uchar_bsize);

	uchar = (UChar *) buf;

	ulen = uchar_convert(icu_converter, uchar, ulen + 1, src, srclen);

	uiter_setString(&iter, uchar, ulen);
	state[0] = state[1] = 0;	/* won't need that again */
	status = U_ZERO_ERROR;
	result_bsize = ucol_nextSortKeyPart(locale->info.icu.ucol,
										&iter,
										state,
										(uint8_t *) dest,
										destsize,
										&status);
	if (U_FAILURE(status))
		ereport(ERROR,
				(errmsg("sort key generation failed: %s",
						u_errorName(status))));

	return result_bsize;
}

/* 'srclen' of -1 means the strings are NUL-terminated */
static size_t
pg_strnxfrm_prefix_icu(char *dest, const char *src, int32_t srclen,
					   int32_t destsize, pg_locale_t locale)
{
	size_t		result;

	Assert(locale->provider == COLLPROVIDER_ICU);

	if (GetDatabaseEncoding() == PG_UTF8)
	{
		UCharIterator iter;
		uint32_t	state[2];
		UErrorCode	status;

		uiter_setUTF8(&iter, src, srclen);
		state[0] = state[1] = 0;	/* won't need that again */
		status = U_ZERO_ERROR;
		result = ucol_nextSortKeyPart(locale->info.icu.ucol,
									  &iter,
									  state,
									  (uint8_t *) dest,
									  destsize,
									  &status);
		if (U_FAILURE(status))
			ereport(ERROR,
					(errmsg("sort key generation failed: %s",
							u_errorName(status))));
	}
	else
		result = pg_strnxfrm_prefix_icu_no_utf8(dest, src, srclen, destsize,
												locale);

	return result;
}

#endif

/*
 * Return true if the collation provider supports pg_strxfrm() and
 * pg_strnxfrm(); otherwise false.
 *
 * Unfortunately, it seems that strxfrm() for non-C collations is broken on
 * many common platforms; testing of multiple versions of glibc reveals that,
 * for many locales, strcoll() and strxfrm() do not return consistent
 * results. While no other libc other than Cygwin has so far been shown to
 * have a problem, we take the conservative course of action for right now and
 * disable this categorically.  (Users who are certain this isn't a problem on
 * their system can define TRUST_STRXFRM.)
 *
 * No similar problem is known for the ICU provider.
 */
bool
pg_strxfrm_enabled(pg_locale_t locale)
{
	if (locale->provider == COLLPROVIDER_LIBC)
#ifdef TRUST_STRXFRM
		return true;
#else
		return false;
#endif
	else if (locale->provider == COLLPROVIDER_ICU)
		return true;
	else
		/* shouldn't happen */
		PGLOCALE_SUPPORT_ERROR(locale->provider);

	return false;				/* keep compiler quiet */
}

/*
 * pg_strxfrm
 *
 * Transforms 'src' to a nul-terminated string stored in 'dest' such that
 * ordinary strcmp() on transformed strings is equivalent to pg_strcoll() on
 * untransformed strings.
 *
 * The provided 'src' must be nul-terminated. If 'destsize' is zero, 'dest'
 * may be NULL.
 *
 * Not all providers support pg_strxfrm() safely. The caller should check
 * pg_strxfrm_enabled() first, otherwise this function may return wrong
 * results or an error.
 *
 * Returns the number of bytes needed (or more) to store the transformed
 * string, excluding the terminating nul byte. If the value returned is
 * 'destsize' or greater, the resulting contents of 'dest' are undefined.
 */
size_t
pg_strxfrm(char *dest, const char *src, size_t destsize, pg_locale_t locale)
{
	size_t		result = 0;		/* keep compiler quiet */

	if (locale->provider == COLLPROVIDER_LIBC)
		result = pg_strxfrm_libc(dest, src, destsize, locale);
#ifdef USE_ICU
	else if (locale->provider == COLLPROVIDER_ICU)
		result = pg_strnxfrm_icu(dest, src, -1, destsize, locale);
#endif
	else
		/* shouldn't happen */
		PGLOCALE_SUPPORT_ERROR(locale->provider);

	return result;
}

/*
 * pg_strnxfrm
 *
 * Transforms 'src' to a nul-terminated string stored in 'dest' such that
 * ordinary strcmp() on transformed strings is equivalent to pg_strcoll() on
 * untransformed strings.
 *
 * 'src' does not need to be nul-terminated. If 'destsize' is zero, 'dest' may
 * be NULL.
 *
 * Not all providers support pg_strnxfrm() safely. The caller should check
 * pg_strxfrm_enabled() first, otherwise this function may return wrong
 * results or an error.
 *
 * Returns the number of bytes needed (or more) to store the transformed
 * string, excluding the terminating nul byte. If the value returned is
 * 'destsize' or greater, the resulting contents of 'dest' are undefined.
 *
 * This function may need to nul-terminate the argument for libc functions;
 * so if the caller already has a nul-terminated string, it should call
 * pg_strxfrm() instead.
 */
size_t
pg_strnxfrm(char *dest, size_t destsize, const char *src, size_t srclen,
			pg_locale_t locale)
{
	size_t		result = 0;		/* keep compiler quiet */

	if (locale->provider == COLLPROVIDER_LIBC)
		result = pg_strnxfrm_libc(dest, src, srclen, destsize, locale);
#ifdef USE_ICU
	else if (locale->provider == COLLPROVIDER_ICU)
		result = pg_strnxfrm_icu(dest, src, srclen, destsize, locale);
#endif
	else
		/* shouldn't happen */
		PGLOCALE_SUPPORT_ERROR(locale->provider);

	return result;
}

/*
 * Return true if the collation provider supports pg_strxfrm_prefix() and
 * pg_strnxfrm_prefix(); otherwise false.
 */
bool
pg_strxfrm_prefix_enabled(pg_locale_t locale)
{
	if (locale->provider == COLLPROVIDER_LIBC)
		return false;
	else if (locale->provider == COLLPROVIDER_ICU)
		return true;
	else
		/* shouldn't happen */
		PGLOCALE_SUPPORT_ERROR(locale->provider);

	return false;				/* keep compiler quiet */
}

/*
 * pg_strxfrm_prefix
 *
 * Transforms 'src' to a byte sequence stored in 'dest' such that ordinary
 * memcmp() on the byte sequence is equivalent to pg_strcoll() on
 * untransformed strings. The result is not nul-terminated.
 *
 * The provided 'src' must be nul-terminated.
 *
 * Not all providers support pg_strxfrm_prefix() safely. The caller should
 * check pg_strxfrm_prefix_enabled() first, otherwise this function may return
 * wrong results or an error.
 *
 * If destsize is not large enough to hold the resulting byte sequence, stores
 * only the first destsize bytes in 'dest'. Returns the number of bytes
 * actually copied to 'dest'.
 */
size_t
pg_strxfrm_prefix(char *dest, const char *src, size_t destsize,
				  pg_locale_t locale)
{
	size_t		result = 0;		/* keep compiler quiet */

#ifdef USE_ICU
	if (locale->provider == COLLPROVIDER_ICU)
		result = pg_strnxfrm_prefix_icu(dest, src, -1, destsize, locale);
	else
#endif
		PGLOCALE_SUPPORT_ERROR(locale->provider);

	return result;
}

/*
 * pg_strnxfrm_prefix
 *
 * Transforms 'src' to a byte sequence stored in 'dest' such that ordinary
 * memcmp() on the byte sequence is equivalent to pg_strcoll() on
 * untransformed strings. The result is not nul-terminated.
 *
 * The provided 'src' must be nul-terminated.
 *
 * Not all providers support pg_strnxfrm_prefix() safely. The caller should
 * check pg_strxfrm_prefix_enabled() first, otherwise this function may return
 * wrong results or an error.
 *
 * If destsize is not large enough to hold the resulting byte sequence, stores
 * only the first destsize bytes in 'dest'. Returns the number of bytes
 * actually copied to 'dest'.
 *
 * This function may need to nul-terminate the argument for libc functions;
 * so if the caller already has a nul-terminated string, it should call
 * pg_strxfrm_prefix() instead.
 */
size_t
pg_strnxfrm_prefix(char *dest, size_t destsize, const char *src,
				   size_t srclen, pg_locale_t locale)
{
	size_t		result = 0;		/* keep compiler quiet */

#ifdef USE_ICU
	if (locale->provider == COLLPROVIDER_ICU)
		result = pg_strnxfrm_prefix_icu(dest, src, -1, destsize, locale);
	else
#endif
		PGLOCALE_SUPPORT_ERROR(locale->provider);

	return result;
}

/*
 * Return required encoding ID for the given locale, or -1 if any encoding is
 * valid for the locale.
 */
int
builtin_locale_encoding(const char *locale)
{
	if (strcmp(locale, "C") == 0)
		return -1;
	if (strcmp(locale, "C.UTF-8") == 0)
		return PG_UTF8;

	ereport(ERROR,
			(errcode(ERRCODE_WRONG_OBJECT_TYPE),
			 errmsg("invalid locale name \"%s\" for builtin provider",
					locale)));

	return 0;					/* keep compiler quiet */
}


/*
 * Validate the locale and encoding combination, and return the canonical form
 * of the locale name.
 */
const char *
builtin_validate_locale(int encoding, const char *locale)
{
	const char *canonical_name = NULL;
	int			required_encoding;

	if (strcmp(locale, "C") == 0)
		canonical_name = "C";
	else if (strcmp(locale, "C.UTF-8") == 0 || strcmp(locale, "C.UTF8") == 0)
		canonical_name = "C.UTF-8";

	if (!canonical_name)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("invalid locale name \"%s\" for builtin provider",
						locale)));

	required_encoding = builtin_locale_encoding(canonical_name);
	if (required_encoding >= 0 && encoding != required_encoding)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("encoding \"%s\" does not match locale \"%s\"",
						pg_encoding_to_char(encoding), locale)));

	return canonical_name;
}


#ifdef USE_ICU

/*
 * Wrapper around ucol_open() to handle API differences for older ICU
 * versions.
 */
static UCollator *
pg_ucol_open(const char *loc_str)
{
	UCollator  *collator;
	UErrorCode	status;
	const char *orig_str = loc_str;
	char	   *fixed_str = NULL;

	/*
	 * Must never open default collator, because it depends on the environment
	 * and may change at any time. Should not happen, but check here to catch
	 * bugs that might be hard to catch otherwise.
	 *
	 * NB: the default collator is not the same as the collator for the root
	 * locale. The root locale may be specified as the empty string, "und", or
	 * "root". The default collator is opened by passing NULL to ucol_open().
	 */
	if (loc_str == NULL)
		elog(ERROR, "opening default collator is not supported");

	/*
	 * In ICU versions 54 and earlier, "und" is not a recognized spelling of
	 * the root locale. If the first component of the locale is "und", replace
	 * with "root" before opening.
	 */
	if (U_ICU_VERSION_MAJOR_NUM < 55)
	{
		char		lang[ULOC_LANG_CAPACITY];

		status = U_ZERO_ERROR;
		uloc_getLanguage(loc_str, lang, ULOC_LANG_CAPACITY, &status);
		if (U_FAILURE(status) || status == U_STRING_NOT_TERMINATED_WARNING)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not get language from locale \"%s\": %s",
							loc_str, u_errorName(status))));
		}

		if (strcmp(lang, "und") == 0)
		{
			const char *remainder = loc_str + strlen("und");

			fixed_str = palloc(strlen("root") + strlen(remainder) + 1);
			strcpy(fixed_str, "root");
			strcat(fixed_str, remainder);

			loc_str = fixed_str;
		}
	}

	status = U_ZERO_ERROR;
	collator = ucol_open(loc_str, &status);
	if (U_FAILURE(status))
		ereport(ERROR,
		/* use original string for error report */
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not open collator for locale \"%s\": %s",
						orig_str, u_errorName(status))));

	if (U_ICU_VERSION_MAJOR_NUM < 54)
	{
		status = U_ZERO_ERROR;
		icu_set_collation_attributes(collator, loc_str, &status);

		/*
		 * Pretend the error came from ucol_open(), for consistent error
		 * message across ICU versions.
		 */
		if (U_FAILURE(status) || status == U_STRING_NOT_TERMINATED_WARNING)
		{
			ucol_close(collator);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not open collator for locale \"%s\": %s",
							orig_str, u_errorName(status))));
		}
	}

	if (fixed_str != NULL)
		pfree(fixed_str);

	return collator;
}

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
 * Find length, in UChars, of given string if converted to UChar string.
 */
static size_t
uchar_length(UConverter *converter, const char *str, int32_t len)
{
	UErrorCode	status = U_ZERO_ERROR;
	int32_t		ulen;

	ulen = ucnv_toUChars(converter, NULL, 0, str, len, &status);
	if (U_FAILURE(status) && status != U_BUFFER_OVERFLOW_ERROR)
		ereport(ERROR,
				(errmsg("%s failed: %s", "ucnv_toUChars", u_errorName(status))));
	return ulen;
}

/*
 * Convert the given source string into a UChar string, stored in dest, and
 * return the length (in UChars).
 */
static int32_t
uchar_convert(UConverter *converter, UChar *dest, int32_t destlen,
			  const char *src, int32_t srclen)
{
	UErrorCode	status = U_ZERO_ERROR;
	int32_t		ulen;

	status = U_ZERO_ERROR;
	ulen = ucnv_toUChars(converter, dest, destlen, src, srclen, &status);
	if (U_FAILURE(status))
		ereport(ERROR,
				(errmsg("%s failed: %s", "ucnv_toUChars", u_errorName(status))));
	return ulen;
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
	int32_t		len_uchar;

	init_icu_converter();

	len_uchar = uchar_length(icu_converter, buff, nbytes);

	*buff_uchar = palloc((len_uchar + 1) * sizeof(**buff_uchar));
	len_uchar = uchar_convert(icu_converter,
							  *buff_uchar, len_uchar + 1, buff, nbytes);

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
	if (U_FAILURE(status) ||
		status == U_STRING_NOT_TERMINATED_WARNING)
		ereport(ERROR,
				(errmsg("%s failed: %s", "ucnv_fromUChars",
						u_errorName(status))));

	return len_result;
}

/*
 * Parse collation attributes from the given locale string and apply them to
 * the open collator.
 *
 * First, the locale string is canonicalized to an ICU format locale ID such
 * as "und@colStrength=primary;colCaseLevel=yes". Then, it parses and applies
 * the key-value arguments.
 *
 * Starting with ICU version 54, the attributes are processed automatically by
 * ucol_open(), so this is only necessary for emulating this behavior on older
 * versions.
 */
pg_attribute_unused()
static void
icu_set_collation_attributes(UCollator *collator, const char *loc,
							 UErrorCode *status)
{
	int32_t		len;
	char	   *icu_locale_id;
	char	   *lower_str;
	char	   *str;
	char	   *token;

	/*
	 * The input locale may be a BCP 47 language tag, e.g.
	 * "und-u-kc-ks-level1", which expresses the same attributes in a
	 * different form. It will be converted to the equivalent ICU format
	 * locale ID, e.g. "und@colcaselevel=yes;colstrength=primary", by
	 * uloc_canonicalize().
	 */
	*status = U_ZERO_ERROR;
	len = uloc_canonicalize(loc, NULL, 0, status);
	icu_locale_id = palloc(len + 1);
	*status = U_ZERO_ERROR;
	len = uloc_canonicalize(loc, icu_locale_id, len + 1, status);
	if (U_FAILURE(*status) || *status == U_STRING_NOT_TERMINATED_WARNING)
		return;

	lower_str = asc_tolower(icu_locale_id, strlen(icu_locale_id));

	pfree(icu_locale_id);

	str = strchr(lower_str, '@');
	if (!str)
		return;
	str++;

	while ((token = strsep(&str, ";")))
	{
		char	   *e = strchr(token, '=');

		if (e)
		{
			char	   *name;
			char	   *value;
			UColAttribute uattr;
			UColAttributeValue uvalue;

			*status = U_ZERO_ERROR;

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
			{
				*status = U_ILLEGAL_ARGUMENT_ERROR;
				break;
			}

			ucol_setAttribute(collator, uattr, uvalue, status);
		}
	}

	pfree(lower_str);
}
#endif

/*
 * Return the BCP47 language tag representation of the requested locale.
 *
 * This function should be called before passing the string to ucol_open(),
 * because conversion to a language tag also performs "level 2
 * canonicalization". In addition to producing a consistent format, level 2
 * canonicalization is able to more accurately interpret different input
 * locale string formats, such as POSIX and .NET IDs.
 */
char *
icu_language_tag(const char *loc_str, int elevel)
{
#ifdef USE_ICU
	UErrorCode	status;
	char	   *langtag;
	size_t		buflen = 32;	/* arbitrary starting buffer size */
	const bool	strict = true;

	/*
	 * A BCP47 language tag doesn't have a clearly-defined upper limit (cf.
	 * RFC5646 section 4.4). Additionally, in older ICU versions,
	 * uloc_toLanguageTag() doesn't always return the ultimate length on the
	 * first call, necessitating a loop.
	 */
	langtag = palloc(buflen);
	while (true)
	{
		status = U_ZERO_ERROR;
		uloc_toLanguageTag(loc_str, langtag, buflen, strict, &status);

		/* try again if the buffer is not large enough */
		if ((status == U_BUFFER_OVERFLOW_ERROR ||
			 status == U_STRING_NOT_TERMINATED_WARNING) &&
			buflen < MaxAllocSize)
		{
			buflen = Min(buflen * 2, MaxAllocSize);
			langtag = repalloc(langtag, buflen);
			continue;
		}

		break;
	}

	if (U_FAILURE(status))
	{
		pfree(langtag);

		if (elevel > 0)
			ereport(elevel,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not convert locale name \"%s\" to language tag: %s",
							loc_str, u_errorName(status))));
		return NULL;
	}

	return langtag;
#else							/* not USE_ICU */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("ICU is not supported in this build")));
	return NULL;				/* keep compiler quiet */
#endif							/* not USE_ICU */
}

/*
 * Perform best-effort check that the locale is a valid one.
 */
void
icu_validate_locale(const char *loc_str)
{
#ifdef USE_ICU
	UCollator  *collator;
	UErrorCode	status;
	char		lang[ULOC_LANG_CAPACITY];
	bool		found = false;
	int			elevel = icu_validation_level;

	/* no validation */
	if (elevel < 0)
		return;

	/* downgrade to WARNING during pg_upgrade */
	if (IsBinaryUpgrade && elevel > WARNING)
		elevel = WARNING;

	/* validate that we can extract the language */
	status = U_ZERO_ERROR;
	uloc_getLanguage(loc_str, lang, ULOC_LANG_CAPACITY, &status);
	if (U_FAILURE(status) || status == U_STRING_NOT_TERMINATED_WARNING)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("could not get language from ICU locale \"%s\": %s",
						loc_str, u_errorName(status)),
				 errhint("To disable ICU locale validation, set the parameter \"%s\" to \"%s\".",
						 "icu_validation_level", "disabled")));
		return;
	}

	/* check for special language name */
	if (strcmp(lang, "") == 0 ||
		strcmp(lang, "root") == 0 || strcmp(lang, "und") == 0)
		found = true;

	/* search for matching language within ICU */
	for (int32_t i = 0; !found && i < uloc_countAvailable(); i++)
	{
		const char *otherloc = uloc_getAvailable(i);
		char		otherlang[ULOC_LANG_CAPACITY];

		status = U_ZERO_ERROR;
		uloc_getLanguage(otherloc, otherlang, ULOC_LANG_CAPACITY, &status);
		if (U_FAILURE(status) || status == U_STRING_NOT_TERMINATED_WARNING)
			continue;

		if (strcmp(lang, otherlang) == 0)
			found = true;
	}

	if (!found)
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("ICU locale \"%s\" has unknown language \"%s\"",
						loc_str, lang),
				 errhint("To disable ICU locale validation, set the parameter \"%s\" to \"%s\".",
						 "icu_validation_level", "disabled")));

	/* check that it can be opened */
	collator = pg_ucol_open(loc_str);
	ucol_close(collator);
#else							/* not USE_ICU */
	/* could get here if a collation was created by a build with ICU */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("ICU is not supported in this build")));
#endif							/* not USE_ICU */
}

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
		/* Use wcstombs_l for nondefault locales */
		result = wcstombs_l(to, from, tolen, locale->info.lt);
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
			/* Use mbstowcs_l for nondefault locales */
			result = mbstowcs_l(to, str, tolen, locale->info.lt);
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
