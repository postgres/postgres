/* contrib/ltree/crc32.c */

/*
 * Implements CRC-32, as used in ltree.
 *
 * Note that the CRC is used in the on-disk format of GiST indexes, so we
 * must stay backwards-compatible!
 */

#include "postgres.h"

#include <sys/types.h>
#include <stdio.h>
#include <sys/types.h>

#ifdef LOWER_NODE
#include <ctype.h>
#define TOLOWER(x)	tolower((unsigned char) (x))
#else
#define TOLOWER(x)	(x)
#endif

#include "utils/pg_crc.h"
#include "crc32.h"

unsigned int
ltree_crc32_sz(char *buf, int size)
{
	pg_crc32	crc;
	char	   *p = buf;

	INIT_TRADITIONAL_CRC32(crc);
	while (size > 0)
	{
		char		c = (char) TOLOWER(*p);

		COMP_TRADITIONAL_CRC32(crc, &c, 1);
		size--;
		p++;
	}
	FIN_TRADITIONAL_CRC32(crc);
	return (unsigned int) crc;
}
