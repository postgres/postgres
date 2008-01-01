/*-----------------------------------------------------------------------
 *
 * PostgreSQL locale utilities
 *
 * Portions Copyright (c) 2002-2008, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/backend/utils/adt/pg_locale.c,v 1.40 2008/01/01 19:45:52 momjian Exp $
 *
 *-----------------------------------------------------------------------
 */

/*----------
 * Here is how the locale stuff is handled: LC_COLLATE and LC_CTYPE
 * are fixed by initdb, stored in pg_control, and cannot be changed.
 * Thus, the effects of strcoll(), strxfrm(), isupper(), toupper(),
 * etc. are always in the same fixed locale.
 *
 * LC_MESSAGES is settable at run time and will take effect
 * immediately.
 *
 * The other categories, LC_MONETARY, LC_NUMERIC, and LC_TIME are also
 * settable at run-time.  However, we don't actually set those locale
 * categories permanently.	This would have bizarre effects like no
 * longer accepting standard floating-point literals in some locales.
 * Instead, we only set the locales briefly when needed, cache the
 * required information obtained from localeconv(), and set them back.
 * The cached information is only used by the formatting functions
 * (to_char, etc.) and the money type.	For the user, this should all be
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
 * will change the memory save is pointing at.	To do this sort of thing
 * safely, you *must* pstrdup what setlocale returns the first time.
 *----------
 */


#include "postgres.h"

#include <locale.h>

#include "catalog/pg_control.h"
#include "utils/pg_locale.h"


/* GUC storage area */

char	   *locale_messages;
char	   *locale_monetary;
char	   *locale_numeric;
char	   *locale_time;

/* indicates whether locale information cache is valid */
static bool CurrentLocaleConvValid = false;

/* Environment variable storage area */

#define LC_ENV_BUFSIZE (LOCALE_NAME_BUFLEN + 20)

static char lc_collate_envbuf[LC_ENV_BUFSIZE];
static char lc_ctype_envbuf[LC_ENV_BUFSIZE];

#ifdef LC_MESSAGES
static char lc_messages_envbuf[LC_ENV_BUFSIZE];
#endif
static char lc_monetary_envbuf[LC_ENV_BUFSIZE];
static char lc_numeric_envbuf[LC_ENV_BUFSIZE];
static char lc_time_envbuf[LC_ENV_BUFSIZE];


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
			break;
#endif
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
			break;
	}

	snprintf(envbuf, LC_ENV_BUFSIZE - 1, "%s=%s", envvar, result);

#ifndef WIN32
	if (putenv(envbuf))
		return NULL;
#else

	/*
	 * On Windows, we need to modify both the process environment and the
	 * cached version in msvcrt
	 */
	if (!SetEnvironmentVariable(envvar, result))
		return NULL;
	if (_putenv(envbuf))
		return NULL;
#endif

	return result;
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
	char	   *save;

	save = setlocale(category, NULL);
	if (!save)
		return NULL;			/* won't happen, we hope */

	/* save may be pointing at a modifiable scratch variable, see above */
	save = pstrdup(save);

	if (!setlocale(category, value))
		value = NULL;			/* set failure return marker */

	setlocale(category, save);	/* assume this won't fail */
	pfree(save);

	/* need to reload cache next time? */
	if (doit && value != NULL)
		CurrentLocaleConvValid = false;

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

	/* Did we do it already? */
	if (CurrentLocaleConvValid)
		return &CurrentLocaleConv;

	free_struct_lconv(&CurrentLocaleConv);

	/* Set user's values of monetary and numeric locales */
	save_lc_monetary = setlocale(LC_MONETARY, NULL);
	if (save_lc_monetary)
		save_lc_monetary = pstrdup(save_lc_monetary);
	save_lc_numeric = setlocale(LC_NUMERIC, NULL);
	if (save_lc_numeric)
		save_lc_numeric = pstrdup(save_lc_numeric);

	setlocale(LC_MONETARY, locale_monetary);
	setlocale(LC_NUMERIC, locale_numeric);

	/* Get formatting information */
	extlconv = localeconv();

	/*
	 * Must copy all values since restoring internal settings may overwrite
	 * localeconv()'s results.
	 */
	CurrentLocaleConv = *extlconv;
	CurrentLocaleConv.currency_symbol = strdup(extlconv->currency_symbol);
	CurrentLocaleConv.decimal_point = strdup(extlconv->decimal_point);
	CurrentLocaleConv.grouping = strdup(extlconv->grouping);
	CurrentLocaleConv.thousands_sep = strdup(extlconv->thousands_sep);
	CurrentLocaleConv.int_curr_symbol = strdup(extlconv->int_curr_symbol);
	CurrentLocaleConv.mon_decimal_point = strdup(extlconv->mon_decimal_point);
	CurrentLocaleConv.mon_grouping = strdup(extlconv->mon_grouping);
	CurrentLocaleConv.mon_thousands_sep = strdup(extlconv->mon_thousands_sep);
	CurrentLocaleConv.negative_sign = strdup(extlconv->negative_sign);
	CurrentLocaleConv.positive_sign = strdup(extlconv->positive_sign);
	CurrentLocaleConv.n_sign_posn = extlconv->n_sign_posn;

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

	CurrentLocaleConvValid = true;
	return &CurrentLocaleConv;
}
