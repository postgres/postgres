/*-------------------------------------------------------------------------
 *
 * win32setlocale.c
 *		Wrapper to work around bugs in Windows setlocale() implementation
 *
 * Copyright (c) 2011-2013, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/win32setlocale.c
 *
 *
 * Windows has a problem with locale names that have a dot in the country
 * name. For example:
 *
 * "Chinese (Traditional)_Hong Kong S.A.R..950"
 *
 * For some reason, setlocale() doesn't accept that. Fortunately, Windows'
 * setlocale() accepts various alternative names for such countries, so we
 * provide a wrapper setlocale() function that maps the troublemaking locale
 * names to accepted aliases.
 *-------------------------------------------------------------------------
 */

#include "c.h"

#undef setlocale

struct locale_map
{
	const char *locale_name_part;		/* string in locale name to replace */
	const char *replacement;	/* string to replace it with */
};

static const struct locale_map locale_map_list[] = {
	/*
	 * "HKG" is listed here:
	 * http://msdn.microsoft.com/en-us/library/cdax410z%28v=vs.71%29.aspx
	 * (Country/Region Strings).
	 *
	 * "ARE" is the ISO-3166 three-letter code for U.A.E. It is not on the
	 * above list, but seems to work anyway.
	 */
	{"Hong Kong S.A.R.", "HKG"},
	{"U.A.E.", "ARE"},

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
	{"Chinese (Traditional)_Macau S.A.R..950", "ZHM"},
	{"Chinese_Macau S.A.R..950", "ZHM"},
	{"Chinese (Traditional)_Macao S.A.R..950", "ZHM"},
	{"Chinese_Macao S.A.R..950", "ZHM"}
};

char *
pgwin32_setlocale(int category, const char *locale)
{
	char	   *result;
	char	   *alias;
	int			i;

	if (locale == NULL)
		return setlocale(category, locale);

	/* Check if the locale name matches any of the problematic ones. */
	alias = NULL;
	for (i = 0; i < lengthof(locale_map_list); i++)
	{
		const char *needle = locale_map_list[i].locale_name_part;
		const char *replacement = locale_map_list[i].replacement;
		char	   *match;

		match = strstr(locale, needle);
		if (match != NULL)
		{
			/* Found a match. Replace the matched string. */
			int			matchpos = match - locale;
			int			replacementlen = strlen(replacement);
			char	   *rest = match + strlen(needle);
			int			restlen = strlen(rest);

			alias = malloc(matchpos + replacementlen + restlen + 1);
			if (!alias)
				return NULL;

			memcpy(&alias[0], &locale[0], matchpos);
			memcpy(&alias[matchpos], replacement, replacementlen);
			memcpy(&alias[matchpos + replacementlen], rest, restlen + 1);		/* includes null
																				 * terminator */

			break;
		}
	}

	/* Call the real setlocale() function */
	if (alias)
	{
		result = setlocale(category, alias);
		free(alias);
	}
	else
		result = setlocale(category, locale);

	return result;
}
