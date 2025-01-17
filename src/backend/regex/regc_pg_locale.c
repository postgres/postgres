/*-------------------------------------------------------------------------
 *
 * regc_pg_locale.c
 *	  ctype functions adapted to work on pg_wchar (a/k/a chr),
 *	  and functions to cache the results of wholesale ctype probing.
 *
 * This file is #included by regcomp.c; it's not meant to compile standalone.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/regex/regc_pg_locale.c
 *
 *-------------------------------------------------------------------------
 */

#include "catalog/pg_collation.h"
#include "common/unicode_case.h"
#include "common/unicode_category.h"
#include "utils/pg_locale.h"

/*
 * To provide as much functionality as possible on a variety of platforms,
 * without going so far as to implement everything from scratch, we use
 * several implementation strategies depending on the situation:
 *
 * 1. In C/POSIX collations, we use hard-wired code.  We can't depend on
 * the <ctype.h> functions since those will obey LC_CTYPE.  Note that these
 * collations don't give a fig about multibyte characters.
 *
 * 2. In the "default" collation (which is supposed to obey LC_CTYPE):
 *
 * 2a. When working in UTF8 encoding, we use the <wctype.h> functions.
 * This assumes that every platform uses Unicode codepoints directly
 * as the wchar_t representation of Unicode.  On some platforms
 * wchar_t is only 16 bits wide, so we have to punt for codepoints > 0xFFFF.
 *
 * 2b. In all other encodings, we use the <ctype.h> functions for pg_wchar
 * values up to 255, and punt for values above that.  This is 100% correct
 * only in single-byte encodings such as LATINn.  However, non-Unicode
 * multibyte encodings are mostly Far Eastern character sets for which the
 * properties being tested here aren't very relevant for higher code values
 * anyway.  The difficulty with using the <wctype.h> functions with
 * non-Unicode multibyte encodings is that we can have no certainty that
 * the platform's wchar_t representation matches what we do in pg_wchar
 * conversions.
 *
 * 3. Here, we use the locale_t-extended forms of the <wctype.h> and <ctype.h>
 * functions, under exactly the same cases as #2.
 *
 * There is one notable difference between cases 2 and 3: in the "default"
 * collation we force ASCII letters to follow ASCII upcase/downcase rules,
 * while in a non-default collation we just let the library functions do what
 * they will.  The case where this matters is treatment of I/i in Turkish,
 * and the behavior is meant to match the upper()/lower() SQL functions.
 *
 * We store the active collation setting in static variables.  In principle
 * it could be passed down to here via the regex library's "struct vars" data
 * structure; but that would require somewhat invasive changes in the regex
 * library, and right now there's no real benefit to be gained from that.
 *
 * NB: the coding here assumes pg_wchar is an unsigned type.
 */

typedef enum
{
	PG_REGEX_STRATEGY_C,		/* C locale (encoding independent) */
	PG_REGEX_STRATEGY_BUILTIN,	/* built-in Unicode semantics */
	PG_REGEX_STRATEGY_LIBC_WIDE,	/* Use locale_t <wctype.h> functions */
	PG_REGEX_STRATEGY_LIBC_1BYTE,	/* Use locale_t <ctype.h> functions */
	PG_REGEX_STRATEGY_ICU,		/* Use ICU uchar.h functions */
} PG_Locale_Strategy;

static PG_Locale_Strategy pg_regex_strategy;
static pg_locale_t pg_regex_locale;

/*
 * Hard-wired character properties for C locale
 */
#define PG_ISDIGIT	0x01
#define PG_ISALPHA	0x02
#define PG_ISALNUM	(PG_ISDIGIT | PG_ISALPHA)
#define PG_ISUPPER	0x04
#define PG_ISLOWER	0x08
#define PG_ISGRAPH	0x10
#define PG_ISPRINT	0x20
#define PG_ISPUNCT	0x40
#define PG_ISSPACE	0x80

static const unsigned char pg_char_properties[128] = {
	 /* NUL */ 0,
	 /* ^A */ 0,
	 /* ^B */ 0,
	 /* ^C */ 0,
	 /* ^D */ 0,
	 /* ^E */ 0,
	 /* ^F */ 0,
	 /* ^G */ 0,
	 /* ^H */ 0,
	 /* ^I */ PG_ISSPACE,
	 /* ^J */ PG_ISSPACE,
	 /* ^K */ PG_ISSPACE,
	 /* ^L */ PG_ISSPACE,
	 /* ^M */ PG_ISSPACE,
	 /* ^N */ 0,
	 /* ^O */ 0,
	 /* ^P */ 0,
	 /* ^Q */ 0,
	 /* ^R */ 0,
	 /* ^S */ 0,
	 /* ^T */ 0,
	 /* ^U */ 0,
	 /* ^V */ 0,
	 /* ^W */ 0,
	 /* ^X */ 0,
	 /* ^Y */ 0,
	 /* ^Z */ 0,
	 /* ^[ */ 0,
	 /* ^\ */ 0,
	 /* ^] */ 0,
	 /* ^^ */ 0,
	 /* ^_ */ 0,
	 /* */ PG_ISPRINT | PG_ISSPACE,
	 /* !  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* "  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* #  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* $  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* %  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* &  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* '  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* (  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* )  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* *  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* +  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* ,  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* -  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* .  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* /  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* 0  */ PG_ISDIGIT | PG_ISGRAPH | PG_ISPRINT,
	 /* 1  */ PG_ISDIGIT | PG_ISGRAPH | PG_ISPRINT,
	 /* 2  */ PG_ISDIGIT | PG_ISGRAPH | PG_ISPRINT,
	 /* 3  */ PG_ISDIGIT | PG_ISGRAPH | PG_ISPRINT,
	 /* 4  */ PG_ISDIGIT | PG_ISGRAPH | PG_ISPRINT,
	 /* 5  */ PG_ISDIGIT | PG_ISGRAPH | PG_ISPRINT,
	 /* 6  */ PG_ISDIGIT | PG_ISGRAPH | PG_ISPRINT,
	 /* 7  */ PG_ISDIGIT | PG_ISGRAPH | PG_ISPRINT,
	 /* 8  */ PG_ISDIGIT | PG_ISGRAPH | PG_ISPRINT,
	 /* 9  */ PG_ISDIGIT | PG_ISGRAPH | PG_ISPRINT,
	 /* :  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* ;  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* <  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* =  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* >  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* ?  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* @  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* A  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* B  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* C  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* D  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* E  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* F  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* G  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* H  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* I  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* J  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* K  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* L  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* M  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* N  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* O  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* P  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* Q  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* R  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* S  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* T  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* U  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* V  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* W  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* X  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* Y  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* Z  */ PG_ISALPHA | PG_ISUPPER | PG_ISGRAPH | PG_ISPRINT,
	 /* [  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* \  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* ]  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* ^  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* _  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* `  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* a  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* b  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* c  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* d  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* e  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* f  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* g  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* h  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* i  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* j  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* k  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* l  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* m  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* n  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* o  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* p  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* q  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* r  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* s  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* t  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* u  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* v  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* w  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* x  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* y  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* z  */ PG_ISALPHA | PG_ISLOWER | PG_ISGRAPH | PG_ISPRINT,
	 /* {  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* |  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* }  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* ~  */ PG_ISGRAPH | PG_ISPRINT | PG_ISPUNCT,
	 /* DEL */ 0
};


/*
 * pg_set_regex_collation: set collation for these functions to obey
 *
 * This is called when beginning compilation or execution of a regexp.
 * Since there's no need for reentrancy of regexp operations, it's okay
 * to store the results in static variables.
 */
void
pg_set_regex_collation(Oid collation)
{
	pg_locale_t locale = 0;
	PG_Locale_Strategy strategy;

	if (!OidIsValid(collation))
	{
		/*
		 * This typically means that the parser could not resolve a conflict
		 * of implicit collations, so report it that way.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for regular expression"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));
	}

	if (collation == C_COLLATION_OID)
	{
		/*
		 * Some callers expect regexes to work for C_COLLATION_OID before
		 * catalog access is available, so we can't call
		 * pg_newlocale_from_collation().
		 */
		strategy = PG_REGEX_STRATEGY_C;
		locale = 0;
	}
	else
	{
		locale = pg_newlocale_from_collation(collation);

		if (!locale->deterministic)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("nondeterministic collations are not supported for regular expressions")));

		if (locale->ctype_is_c)
		{
			/*
			 * C/POSIX collations use this path regardless of database
			 * encoding
			 */
			strategy = PG_REGEX_STRATEGY_C;
			locale = 0;
		}
		else if (locale->provider == COLLPROVIDER_BUILTIN)
		{
			Assert(GetDatabaseEncoding() == PG_UTF8);
			strategy = PG_REGEX_STRATEGY_BUILTIN;
		}
#ifdef USE_ICU
		else if (locale->provider == COLLPROVIDER_ICU)
		{
			strategy = PG_REGEX_STRATEGY_ICU;
		}
#endif
		else
		{
			Assert(locale->provider == COLLPROVIDER_LIBC);
			if (GetDatabaseEncoding() == PG_UTF8)
				strategy = PG_REGEX_STRATEGY_LIBC_WIDE;
			else
				strategy = PG_REGEX_STRATEGY_LIBC_1BYTE;
		}
	}

	pg_regex_strategy = strategy;
	pg_regex_locale = locale;
}

static int
pg_wc_isdigit(pg_wchar c)
{
	switch (pg_regex_strategy)
	{
		case PG_REGEX_STRATEGY_C:
			return (c <= (pg_wchar) 127 &&
					(pg_char_properties[c] & PG_ISDIGIT));
		case PG_REGEX_STRATEGY_BUILTIN:
			return pg_u_isdigit(c, !pg_regex_locale->info.builtin.casemap_full);
		case PG_REGEX_STRATEGY_LIBC_WIDE:
			if (sizeof(wchar_t) >= 4 || c <= (pg_wchar) 0xFFFF)
				return iswdigit_l((wint_t) c, pg_regex_locale->info.lt);
			/* FALL THRU */
		case PG_REGEX_STRATEGY_LIBC_1BYTE:
			return (c <= (pg_wchar) UCHAR_MAX &&
					isdigit_l((unsigned char) c, pg_regex_locale->info.lt));
			break;
		case PG_REGEX_STRATEGY_ICU:
#ifdef USE_ICU
			return u_isdigit(c);
#endif
			break;
	}
	return 0;					/* can't get here, but keep compiler quiet */
}

static int
pg_wc_isalpha(pg_wchar c)
{
	switch (pg_regex_strategy)
	{
		case PG_REGEX_STRATEGY_C:
			return (c <= (pg_wchar) 127 &&
					(pg_char_properties[c] & PG_ISALPHA));
		case PG_REGEX_STRATEGY_BUILTIN:
			return pg_u_isalpha(c);
		case PG_REGEX_STRATEGY_LIBC_WIDE:
			if (sizeof(wchar_t) >= 4 || c <= (pg_wchar) 0xFFFF)
				return iswalpha_l((wint_t) c, pg_regex_locale->info.lt);
			/* FALL THRU */
		case PG_REGEX_STRATEGY_LIBC_1BYTE:
			return (c <= (pg_wchar) UCHAR_MAX &&
					isalpha_l((unsigned char) c, pg_regex_locale->info.lt));
			break;
		case PG_REGEX_STRATEGY_ICU:
#ifdef USE_ICU
			return u_isalpha(c);
#endif
			break;
	}
	return 0;					/* can't get here, but keep compiler quiet */
}

static int
pg_wc_isalnum(pg_wchar c)
{
	switch (pg_regex_strategy)
	{
		case PG_REGEX_STRATEGY_C:
			return (c <= (pg_wchar) 127 &&
					(pg_char_properties[c] & PG_ISALNUM));
		case PG_REGEX_STRATEGY_BUILTIN:
			return pg_u_isalnum(c, !pg_regex_locale->info.builtin.casemap_full);
		case PG_REGEX_STRATEGY_LIBC_WIDE:
			if (sizeof(wchar_t) >= 4 || c <= (pg_wchar) 0xFFFF)
				return iswalnum_l((wint_t) c, pg_regex_locale->info.lt);
			/* FALL THRU */
		case PG_REGEX_STRATEGY_LIBC_1BYTE:
			return (c <= (pg_wchar) UCHAR_MAX &&
					isalnum_l((unsigned char) c, pg_regex_locale->info.lt));
			break;
		case PG_REGEX_STRATEGY_ICU:
#ifdef USE_ICU
			return u_isalnum(c);
#endif
			break;
	}
	return 0;					/* can't get here, but keep compiler quiet */
}

static int
pg_wc_isword(pg_wchar c)
{
	/* We define word characters as alnum class plus underscore */
	if (c == CHR('_'))
		return 1;
	return pg_wc_isalnum(c);
}

static int
pg_wc_isupper(pg_wchar c)
{
	switch (pg_regex_strategy)
	{
		case PG_REGEX_STRATEGY_C:
			return (c <= (pg_wchar) 127 &&
					(pg_char_properties[c] & PG_ISUPPER));
		case PG_REGEX_STRATEGY_BUILTIN:
			return pg_u_isupper(c);
		case PG_REGEX_STRATEGY_LIBC_WIDE:
			if (sizeof(wchar_t) >= 4 || c <= (pg_wchar) 0xFFFF)
				return iswupper_l((wint_t) c, pg_regex_locale->info.lt);
			/* FALL THRU */
		case PG_REGEX_STRATEGY_LIBC_1BYTE:
			return (c <= (pg_wchar) UCHAR_MAX &&
					isupper_l((unsigned char) c, pg_regex_locale->info.lt));
			break;
		case PG_REGEX_STRATEGY_ICU:
#ifdef USE_ICU
			return u_isupper(c);
#endif
			break;
	}
	return 0;					/* can't get here, but keep compiler quiet */
}

static int
pg_wc_islower(pg_wchar c)
{
	switch (pg_regex_strategy)
	{
		case PG_REGEX_STRATEGY_C:
			return (c <= (pg_wchar) 127 &&
					(pg_char_properties[c] & PG_ISLOWER));
		case PG_REGEX_STRATEGY_BUILTIN:
			return pg_u_islower(c);
		case PG_REGEX_STRATEGY_LIBC_WIDE:
			if (sizeof(wchar_t) >= 4 || c <= (pg_wchar) 0xFFFF)
				return iswlower_l((wint_t) c, pg_regex_locale->info.lt);
			/* FALL THRU */
		case PG_REGEX_STRATEGY_LIBC_1BYTE:
			return (c <= (pg_wchar) UCHAR_MAX &&
					islower_l((unsigned char) c, pg_regex_locale->info.lt));
			break;
		case PG_REGEX_STRATEGY_ICU:
#ifdef USE_ICU
			return u_islower(c);
#endif
			break;
	}
	return 0;					/* can't get here, but keep compiler quiet */
}

static int
pg_wc_isgraph(pg_wchar c)
{
	switch (pg_regex_strategy)
	{
		case PG_REGEX_STRATEGY_C:
			return (c <= (pg_wchar) 127 &&
					(pg_char_properties[c] & PG_ISGRAPH));
		case PG_REGEX_STRATEGY_BUILTIN:
			return pg_u_isgraph(c);
		case PG_REGEX_STRATEGY_LIBC_WIDE:
			if (sizeof(wchar_t) >= 4 || c <= (pg_wchar) 0xFFFF)
				return iswgraph_l((wint_t) c, pg_regex_locale->info.lt);
			/* FALL THRU */
		case PG_REGEX_STRATEGY_LIBC_1BYTE:
			return (c <= (pg_wchar) UCHAR_MAX &&
					isgraph_l((unsigned char) c, pg_regex_locale->info.lt));
			break;
		case PG_REGEX_STRATEGY_ICU:
#ifdef USE_ICU
			return u_isgraph(c);
#endif
			break;
	}
	return 0;					/* can't get here, but keep compiler quiet */
}

static int
pg_wc_isprint(pg_wchar c)
{
	switch (pg_regex_strategy)
	{
		case PG_REGEX_STRATEGY_C:
			return (c <= (pg_wchar) 127 &&
					(pg_char_properties[c] & PG_ISPRINT));
		case PG_REGEX_STRATEGY_BUILTIN:
			return pg_u_isprint(c);
		case PG_REGEX_STRATEGY_LIBC_WIDE:
			if (sizeof(wchar_t) >= 4 || c <= (pg_wchar) 0xFFFF)
				return iswprint_l((wint_t) c, pg_regex_locale->info.lt);
			/* FALL THRU */
		case PG_REGEX_STRATEGY_LIBC_1BYTE:
			return (c <= (pg_wchar) UCHAR_MAX &&
					isprint_l((unsigned char) c, pg_regex_locale->info.lt));
			break;
		case PG_REGEX_STRATEGY_ICU:
#ifdef USE_ICU
			return u_isprint(c);
#endif
			break;
	}
	return 0;					/* can't get here, but keep compiler quiet */
}

static int
pg_wc_ispunct(pg_wchar c)
{
	switch (pg_regex_strategy)
	{
		case PG_REGEX_STRATEGY_C:
			return (c <= (pg_wchar) 127 &&
					(pg_char_properties[c] & PG_ISPUNCT));
		case PG_REGEX_STRATEGY_BUILTIN:
			return pg_u_ispunct(c, !pg_regex_locale->info.builtin.casemap_full);
		case PG_REGEX_STRATEGY_LIBC_WIDE:
			if (sizeof(wchar_t) >= 4 || c <= (pg_wchar) 0xFFFF)
				return iswpunct_l((wint_t) c, pg_regex_locale->info.lt);
			/* FALL THRU */
		case PG_REGEX_STRATEGY_LIBC_1BYTE:
			return (c <= (pg_wchar) UCHAR_MAX &&
					ispunct_l((unsigned char) c, pg_regex_locale->info.lt));
			break;
		case PG_REGEX_STRATEGY_ICU:
#ifdef USE_ICU
			return u_ispunct(c);
#endif
			break;
	}
	return 0;					/* can't get here, but keep compiler quiet */
}

static int
pg_wc_isspace(pg_wchar c)
{
	switch (pg_regex_strategy)
	{
		case PG_REGEX_STRATEGY_C:
			return (c <= (pg_wchar) 127 &&
					(pg_char_properties[c] & PG_ISSPACE));
		case PG_REGEX_STRATEGY_BUILTIN:
			return pg_u_isspace(c);
		case PG_REGEX_STRATEGY_LIBC_WIDE:
			if (sizeof(wchar_t) >= 4 || c <= (pg_wchar) 0xFFFF)
				return iswspace_l((wint_t) c, pg_regex_locale->info.lt);
			/* FALL THRU */
		case PG_REGEX_STRATEGY_LIBC_1BYTE:
			return (c <= (pg_wchar) UCHAR_MAX &&
					isspace_l((unsigned char) c, pg_regex_locale->info.lt));
			break;
		case PG_REGEX_STRATEGY_ICU:
#ifdef USE_ICU
			return u_isspace(c);
#endif
			break;
	}
	return 0;					/* can't get here, but keep compiler quiet */
}

static pg_wchar
pg_wc_toupper(pg_wchar c)
{
	switch (pg_regex_strategy)
	{
		case PG_REGEX_STRATEGY_C:
			if (c <= (pg_wchar) 127)
				return pg_ascii_toupper((unsigned char) c);
			return c;
		case PG_REGEX_STRATEGY_BUILTIN:
			return unicode_uppercase_simple(c);
		case PG_REGEX_STRATEGY_LIBC_WIDE:
			if (sizeof(wchar_t) >= 4 || c <= (pg_wchar) 0xFFFF)
				return towupper_l((wint_t) c, pg_regex_locale->info.lt);
			/* FALL THRU */
		case PG_REGEX_STRATEGY_LIBC_1BYTE:
			if (c <= (pg_wchar) UCHAR_MAX)
				return toupper_l((unsigned char) c, pg_regex_locale->info.lt);
			return c;
		case PG_REGEX_STRATEGY_ICU:
#ifdef USE_ICU
			return u_toupper(c);
#endif
			break;
	}
	return 0;					/* can't get here, but keep compiler quiet */
}

static pg_wchar
pg_wc_tolower(pg_wchar c)
{
	switch (pg_regex_strategy)
	{
		case PG_REGEX_STRATEGY_C:
			if (c <= (pg_wchar) 127)
				return pg_ascii_tolower((unsigned char) c);
			return c;
		case PG_REGEX_STRATEGY_BUILTIN:
			return unicode_lowercase_simple(c);
		case PG_REGEX_STRATEGY_LIBC_WIDE:
			if (sizeof(wchar_t) >= 4 || c <= (pg_wchar) 0xFFFF)
				return towlower_l((wint_t) c, pg_regex_locale->info.lt);
			/* FALL THRU */
		case PG_REGEX_STRATEGY_LIBC_1BYTE:
			if (c <= (pg_wchar) UCHAR_MAX)
				return tolower_l((unsigned char) c, pg_regex_locale->info.lt);
			return c;
		case PG_REGEX_STRATEGY_ICU:
#ifdef USE_ICU
			return u_tolower(c);
#endif
			break;
	}
	return 0;					/* can't get here, but keep compiler quiet */
}


/*
 * These functions cache the results of probing libc's ctype behavior for
 * all character codes of interest in a given encoding/collation.  The
 * result is provided as a "struct cvec", but notice that the representation
 * is a touch different from a cvec created by regc_cvec.c: we allocate the
 * chrs[] and ranges[] arrays separately from the struct so that we can
 * realloc them larger at need.  This is okay since the cvecs made here
 * should never be freed by freecvec().
 *
 * We use malloc not palloc since we mustn't lose control on out-of-memory;
 * the main regex code expects us to return a failure indication instead.
 */

typedef int (*pg_wc_probefunc) (pg_wchar c);

typedef struct pg_ctype_cache
{
	pg_wc_probefunc probefunc;	/* pg_wc_isalpha or a sibling */
	pg_locale_t locale;			/* locale this entry is for */
	struct cvec cv;				/* cache entry contents */
	struct pg_ctype_cache *next;	/* chain link */
} pg_ctype_cache;

static pg_ctype_cache *pg_ctype_cache_list = NULL;

/*
 * Add a chr or range to pcc->cv; return false if run out of memory
 */
static bool
store_match(pg_ctype_cache *pcc, pg_wchar chr1, int nchrs)
{
	chr		   *newchrs;

	if (nchrs > 1)
	{
		if (pcc->cv.nranges >= pcc->cv.rangespace)
		{
			pcc->cv.rangespace *= 2;
			newchrs = (chr *) realloc(pcc->cv.ranges,
									  pcc->cv.rangespace * sizeof(chr) * 2);
			if (newchrs == NULL)
				return false;
			pcc->cv.ranges = newchrs;
		}
		pcc->cv.ranges[pcc->cv.nranges * 2] = chr1;
		pcc->cv.ranges[pcc->cv.nranges * 2 + 1] = chr1 + nchrs - 1;
		pcc->cv.nranges++;
	}
	else
	{
		assert(nchrs == 1);
		if (pcc->cv.nchrs >= pcc->cv.chrspace)
		{
			pcc->cv.chrspace *= 2;
			newchrs = (chr *) realloc(pcc->cv.chrs,
									  pcc->cv.chrspace * sizeof(chr));
			if (newchrs == NULL)
				return false;
			pcc->cv.chrs = newchrs;
		}
		pcc->cv.chrs[pcc->cv.nchrs++] = chr1;
	}
	return true;
}

/*
 * Given a probe function (e.g., pg_wc_isalpha) get a struct cvec for all
 * chrs satisfying the probe function.  The active collation is the one
 * previously set by pg_set_regex_collation.  Return NULL if out of memory.
 *
 * Note that the result must not be freed or modified by caller.
 */
static struct cvec *
pg_ctype_get_cache(pg_wc_probefunc probefunc, int cclasscode)
{
	pg_ctype_cache *pcc;
	pg_wchar	max_chr;
	pg_wchar	cur_chr;
	int			nmatches;
	chr		   *newchrs;

	/*
	 * Do we already have the answer cached?
	 */
	for (pcc = pg_ctype_cache_list; pcc != NULL; pcc = pcc->next)
	{
		if (pcc->probefunc == probefunc &&
			pcc->locale == pg_regex_locale)
			return &pcc->cv;
	}

	/*
	 * Nope, so initialize some workspace ...
	 */
	pcc = (pg_ctype_cache *) malloc(sizeof(pg_ctype_cache));
	if (pcc == NULL)
		return NULL;
	pcc->probefunc = probefunc;
	pcc->locale = pg_regex_locale;
	pcc->cv.nchrs = 0;
	pcc->cv.chrspace = 128;
	pcc->cv.chrs = (chr *) malloc(pcc->cv.chrspace * sizeof(chr));
	pcc->cv.nranges = 0;
	pcc->cv.rangespace = 64;
	pcc->cv.ranges = (chr *) malloc(pcc->cv.rangespace * sizeof(chr) * 2);
	if (pcc->cv.chrs == NULL || pcc->cv.ranges == NULL)
		goto out_of_memory;
	pcc->cv.cclasscode = cclasscode;

	/*
	 * Decide how many character codes we ought to look through.  In general
	 * we don't go past MAX_SIMPLE_CHR; chr codes above that are handled at
	 * runtime using the "high colormap" mechanism.  However, in C locale
	 * there's no need to go further than 127, and if we only have a 1-byte
	 * <ctype.h> API there's no need to go further than that can handle.
	 *
	 * If it's not MAX_SIMPLE_CHR that's constraining the search, mark the
	 * output cvec as not having any locale-dependent behavior, since there
	 * will be no need to do any run-time locale checks.  (The #if's here
	 * would always be true for production values of MAX_SIMPLE_CHR, but it's
	 * useful to allow it to be small for testing purposes.)
	 */
	switch (pg_regex_strategy)
	{
		case PG_REGEX_STRATEGY_C:
#if MAX_SIMPLE_CHR >= 127
			max_chr = (pg_wchar) 127;
			pcc->cv.cclasscode = -1;
#else
			max_chr = (pg_wchar) MAX_SIMPLE_CHR;
#endif
			break;
		case PG_REGEX_STRATEGY_BUILTIN:
			max_chr = (pg_wchar) MAX_SIMPLE_CHR;
			break;
		case PG_REGEX_STRATEGY_LIBC_WIDE:
			max_chr = (pg_wchar) MAX_SIMPLE_CHR;
			break;
		case PG_REGEX_STRATEGY_LIBC_1BYTE:
#if MAX_SIMPLE_CHR >= UCHAR_MAX
			max_chr = (pg_wchar) UCHAR_MAX;
			pcc->cv.cclasscode = -1;
#else
			max_chr = (pg_wchar) MAX_SIMPLE_CHR;
#endif
			break;
		case PG_REGEX_STRATEGY_ICU:
			max_chr = (pg_wchar) MAX_SIMPLE_CHR;
			break;
		default:
			Assert(false);
			max_chr = 0;		/* can't get here, but keep compiler quiet */
			break;
	}

	/*
	 * And scan 'em ...
	 */
	nmatches = 0;				/* number of consecutive matches */

	for (cur_chr = 0; cur_chr <= max_chr; cur_chr++)
	{
		if ((*probefunc) (cur_chr))
			nmatches++;
		else if (nmatches > 0)
		{
			if (!store_match(pcc, cur_chr - nmatches, nmatches))
				goto out_of_memory;
			nmatches = 0;
		}
	}

	if (nmatches > 0)
		if (!store_match(pcc, cur_chr - nmatches, nmatches))
			goto out_of_memory;

	/*
	 * We might have allocated more memory than needed, if so free it
	 */
	if (pcc->cv.nchrs == 0)
	{
		free(pcc->cv.chrs);
		pcc->cv.chrs = NULL;
		pcc->cv.chrspace = 0;
	}
	else if (pcc->cv.nchrs < pcc->cv.chrspace)
	{
		newchrs = (chr *) realloc(pcc->cv.chrs,
								  pcc->cv.nchrs * sizeof(chr));
		if (newchrs == NULL)
			goto out_of_memory;
		pcc->cv.chrs = newchrs;
		pcc->cv.chrspace = pcc->cv.nchrs;
	}
	if (pcc->cv.nranges == 0)
	{
		free(pcc->cv.ranges);
		pcc->cv.ranges = NULL;
		pcc->cv.rangespace = 0;
	}
	else if (pcc->cv.nranges < pcc->cv.rangespace)
	{
		newchrs = (chr *) realloc(pcc->cv.ranges,
								  pcc->cv.nranges * sizeof(chr) * 2);
		if (newchrs == NULL)
			goto out_of_memory;
		pcc->cv.ranges = newchrs;
		pcc->cv.rangespace = pcc->cv.nranges;
	}

	/*
	 * Success, link it into cache chain
	 */
	pcc->next = pg_ctype_cache_list;
	pg_ctype_cache_list = pcc;

	return &pcc->cv;

	/*
	 * Failure, clean up
	 */
out_of_memory:
	free(pcc->cv.chrs);
	free(pcc->cv.ranges);
	free(pcc);

	return NULL;
}
