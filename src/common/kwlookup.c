/*-------------------------------------------------------------------------
 *
 * kwlookup.c
 *	  Key word lookup for PostgreSQL
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/common/kwlookup.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include "common/kwlookup.h"


/*
 * ScanKeywordLookup - see if a given word is a keyword
 *
 * The list of keywords to be matched against is passed as a ScanKeywordList.
 *
 * Returns the keyword number (0..N-1) of the keyword, or -1 if no match.
 * Callers typically use the keyword number to index into information
 * arrays, but that is no concern of this code.
 *
 * The match is done case-insensitively.  Note that we deliberately use a
 * dumbed-down case conversion that will only translate 'A'-'Z' into 'a'-'z',
 * even if we are in a locale where tolower() would produce more or different
 * translations.  This is to conform to the SQL99 spec, which says that
 * keywords are to be matched in this way even though non-keyword identifiers
 * receive a different case-normalization mapping.
 */
int
ScanKeywordLookup(const char *text,
				  const ScanKeywordList *keywords)
{
	int			len,
				i;
	char		word[NAMEDATALEN];
	const char *kw_string;
	const uint16 *kw_offsets;
	const uint16 *low;
	const uint16 *high;

	len = strlen(text);

	if (len > keywords->max_kw_len)
		return -1;				/* too long to be any keyword */

	/* We assume all keywords are shorter than NAMEDATALEN. */
	Assert(len < NAMEDATALEN);

	/*
	 * Apply an ASCII-only downcasing.  We must not use tolower() since it may
	 * produce the wrong translation in some locales (eg, Turkish).
	 */
	for (i = 0; i < len; i++)
	{
		char		ch = text[i];

		if (ch >= 'A' && ch <= 'Z')
			ch += 'a' - 'A';
		word[i] = ch;
	}
	word[len] = '\0';

	/*
	 * Now do a binary search using plain strcmp() comparison.
	 */
	kw_string = keywords->kw_string;
	kw_offsets = keywords->kw_offsets;
	low = kw_offsets;
	high = kw_offsets + (keywords->num_keywords - 1);
	while (low <= high)
	{
		const uint16 *middle;
		int			difference;

		middle = low + (high - low) / 2;
		difference = strcmp(kw_string + *middle, word);
		if (difference == 0)
			return middle - kw_offsets;
		else if (difference < 0)
			low = middle + 1;
		else
			high = middle - 1;
	}

	return -1;
}
