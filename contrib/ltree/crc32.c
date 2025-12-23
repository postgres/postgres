/* contrib/ltree/crc32.c */

/*
 * Implements CRC-32, as used in ltree.
 *
 * Note that the CRC is used in the on-disk format of GiST indexes, so we
 * must stay backwards-compatible!
 */

#include "postgres.h"
#include "ltree.h"

#include "crc32.h"
#include "utils/pg_crc.h"
#ifdef LOWER_NODE
#include "utils/pg_locale.h"
#endif

#ifdef LOWER_NODE

unsigned int
ltree_crc32_sz(const char *buf, int size)
{
	pg_crc32	crc;
	const char *p = buf;
	static pg_locale_t locale = NULL;

	if (!locale)
		locale = pg_database_locale();

	INIT_TRADITIONAL_CRC32(crc);
	while (size > 0)
	{
		char		foldstr[UNICODE_CASEMAP_BUFSZ];
		int			srclen = pg_mblen(p);
		size_t		foldlen;

		/* fold one codepoint at a time */
		foldlen = pg_strfold(foldstr, UNICODE_CASEMAP_BUFSZ, p, srclen,
							 locale);

		COMP_TRADITIONAL_CRC32(crc, foldstr, foldlen);

		size -= srclen;
		p += srclen;
	}
	FIN_TRADITIONAL_CRC32(crc);
	return (unsigned int) crc;
}

#else

unsigned int
ltree_crc32_sz(const char *buf, int size)
{
	pg_crc32	crc;
	const char *p = buf;

	INIT_TRADITIONAL_CRC32(crc);
	while (size > 0)
	{
		COMP_TRADITIONAL_CRC32(crc, p, 1);
		size--;
		p++;
	}
	FIN_TRADITIONAL_CRC32(crc);
	return (unsigned int) crc;
}

#endif							/* !LOWER_NODE */
