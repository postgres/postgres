/*-------------------------------------------------------------------------
 *
 * win32setlocale.c
 *		Wrapper to work around bugs in Windows setlocale() implementation
 *
 * Copyright (c) 2011-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/win32setlocale.c
 *
 *
 * The setlocale() function in Windows is broken in two ways. First, it
 * has a problem with locale names that have a dot in the country name. For
 * example:
 *
 * "Chinese (Traditional)_Hong Kong S.A.R..950"
 *
 * For some reason, setlocale() doesn't accept that as argument, even though
 * setlocale(LC_ALL, NULL) returns exactly that. Fortunately, it accepts
 * various alternative names for such countries, so to work around the broken
 * setlocale() function, we map the troublemaking locale names to accepted
 * aliases, before calling setlocale().
 *
 * The second problem is that the locale name for "Norwegian (Bokm&aring;l)"
 * contains a non-ASCII character. That's problematic, because it's not clear
 * what encoding the locale name itself is supposed to be in, when you
 * haven't yet set a locale. Also, it causes problems when the cluster
 * contains databases with different encodings, as the locale name is stored
 * in the pg_database system catalog. To work around that, when setlocale()
 * returns that locale name, map it to a pure-ASCII alias for the same
 * locale.
 *-------------------------------------------------------------------------
 */

#include "c.h"

#undef setlocale

struct locale_map
{
	/*
	 * String in locale name to replace. Can be a single string (end is NULL),
	 * or separate start and end strings. If two strings are given, the locale
	 * name must contain both of them, and everything between them is
	 * replaced. This is used for a poor-man's regexp search, allowing
	 * replacement of "start.*end".
	 */
	const char *locale_name_start;
	const char *locale_name_end;

	const char *replacement;	/* string to replace the match with */
};

/*
 * Mappings applied before calling setlocale(), to the argument.
 */
static const struct locale_map locale_map_argument[] = {
	/*
	 * "HKG" is listed here:
	 * http://msdn.microsoft.com/en-us/library/cdax410z%28v=vs.71%29.aspx
	 * (Country/Region Strings).
	 *
	 * "ARE" is the ISO-3166 three-letter code for U.A.E. It is not on the
	 * above list, but seems to work anyway.
	 */
	{"Hong Kong S.A.R.", NULL, "HKG"},
	{"U.A.E.", NULL, "ARE"},

	/*
	 * The ISO-3166 country code for Macau S.A.R. is MAC, but Windows doesn't
	 * seem to recognize that. And Macau isn't listed in the table of accepted
	 * abbreviations linked above. Fortunately, "ZHM" seems to be accepted as
	 * an alias for "Chinese (Traditional)_Macau S.A.R..950". I'm not sure
	 * where "ZHM" comes from, must be some legacy naming scheme. But hey, it
	 * works.
	 *
	 * Note that unlike HKG and ARE, ZHM is an alias for the *whole* locale
	 * name, not just the country part.
	 *
	 * Some versions of Windows spell it "Macau", others "Macao".
	 */
	{"Chinese (Traditional)_Macau S.A.R..950", NULL, "ZHM"},
	{"Chinese_Macau S.A.R..950", NULL, "ZHM"},
	{"Chinese (Traditional)_Macao S.A.R..950", NULL, "ZHM"},
	{"Chinese_Macao S.A.R..950", NULL, "ZHM"},
	{NULL, NULL, NULL}
};

/*
 * Mappings applied after calling setlocale(), to its return value.
 */
static const struct locale_map locale_map_result[] = {
	/*
	 * "Norwegian (Bokm&aring;l)" locale name contains the a-ring character.
	 * Map it to a pure-ASCII alias.
	 *
	 * It's not clear what encoding setlocale() uses when it returns the
	 * locale name, so to play it safe, we search for "Norwegian (Bok*l)".
	 *
	 * Just to make life even more complicated, some versions of Windows spell
	 * the locale name without parentheses.  Translate that too.
	 */
	{"Norwegian (Bokm", "l)_Norway", "Norwegian_Norway"},
	{"Norwegian Bokm", "l_Norway", "Norwegian_Norway"},
	{NULL, NULL, NULL}
};

#define MAX_LOCALE_NAME_LEN		100

static const char *
map_locale(const struct locale_map *map, const char *locale)
{
	static char aliasbuf[MAX_LOCALE_NAME_LEN];
	int			i;

	/* Check if the locale name matches any of the problematic ones. */
	for (i = 0; map[i].locale_name_start != NULL; i++)
	{
		const char *needle_start = map[i].locale_name_start;
		const char *needle_end = map[i].locale_name_end;
		const char *replacement = map[i].replacement;
		char	   *match;
		char	   *match_start = NULL;
		char	   *match_end = NULL;

		match = strstr(locale, needle_start);
		if (match)
		{
			/*
			 * Found a match for the first part. If this was a two-part
			 * replacement, find the second part.
			 */
			match_start = match;
			if (needle_end)
			{
				match = strstr(match_start + strlen(needle_start), needle_end);
				if (match)
					match_end = match + strlen(needle_end);
				else
					match_start = NULL;
			}
			else
				match_end = match_start + strlen(needle_start);
		}

		if (match_start)
		{
			/* Found a match. Replace the matched string. */
			int			matchpos = match_start - locale;
			int			replacementlen = strlen(replacement);
			char	   *rest = match_end;
			int			restlen = strlen(rest);

			/* check that the result fits in the static buffer */
			if (matchpos + replacementlen + restlen + 1 > MAX_LOCALE_NAME_LEN)
				return NULL;

			memcpy(&aliasbuf[0], &locale[0], matchpos);
			memcpy(&aliasbuf[matchpos], replacement, replacementlen);
			/* includes null terminator */
			memcpy(&aliasbuf[matchpos + replacementlen], rest, restlen + 1);

			return aliasbuf;
		}
	}

	/* no match, just return the original string */
	return locale;
}

char *
pgwin32_setlocale(int category, const char *locale)
{
	const char *argument;
	char	   *result;

	if (locale == NULL)
		argument = NULL;
	else
		argument = map_locale(locale_map_argument, locale);

	/* Call the real setlocale() function */
	result = setlocale(category, argument);

	/*
	 * setlocale() is specified to return a "char *" that the caller is
	 * forbidden to modify, so casting away the "const" is innocuous.
	 */
	if (result)
		result = unconstify(char *, map_locale(locale_map_result, result));

	return result;
}
