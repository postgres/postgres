/*-------------------------------------------------------------------------
 *
 * hashfn.c
 *		Hash functions for use in dynahash.c hashtables
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/hash/hashfn.c,v 1.15 2001/10/25 05:49:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/hsearch.h"

/*
 * string_hash: hash function for keys that are null-terminated strings.
 *
 * NOTE: since dynahash.c backs this up with a fixed-length memcmp(),
 * the key must actually be zero-padded to the specified maximum length
 * to work correctly.  However, if it is known that nothing after the
 * first zero byte is interesting, this is the right hash function to use.
 *
 * NOTE: this is the default hash function if none is specified.
 */
long
string_hash(void *key, int keysize)
{
	unsigned char *k = (unsigned char *) key;
	long		h = 0;

	while (*k)
		h = (h * PRIME1) ^ (*k++);

	h %= PRIME2;

	return h;
}

/*
 * tag_hash: hash function for fixed-size tag values
 *
 * NB: we assume that the supplied key is aligned at least on an 'int'
 * boundary, if its size is >= sizeof(int).
 */
long
tag_hash(void *key, int keysize)
{
	int		   *k = (int *) key;
	long		h = 0;

	/*
	 * Use four byte chunks in a "jump table" to go a little faster.
	 *
	 * Currently the maximum keysize is 16 (mar 17 1992).  I have put in
	 * cases for up to 32.	Bigger than this will resort to a for loop
	 * (see the default case).
	 */
	switch (keysize)
	{
		case 8 * sizeof(int):
			h = (h * PRIME1) ^(*k++);
			/* fall through */

		case 7 * sizeof(int):
			h = (h * PRIME1) ^(*k++);
			/* fall through */

		case 6 * sizeof(int):
			h = (h * PRIME1) ^(*k++);
			/* fall through */

		case 5 * sizeof(int):
			h = (h * PRIME1) ^(*k++);
			/* fall through */

		case 4 * sizeof(int):
			h = (h * PRIME1) ^(*k++);
			/* fall through */

		case 3 * sizeof(int):
			h = (h * PRIME1) ^(*k++);
			/* fall through */

		case 2 * sizeof(int):
			h = (h * PRIME1) ^(*k++);
			/* fall through */

		case sizeof(int):
			h = (h * PRIME1) ^(*k++);
			break;

		default:
			/* Do an int at a time */
			for (; keysize >= (int) sizeof(int); keysize -= sizeof(int))
				h = (h * PRIME1) ^ (*k++);

			/* Cope with any partial-int leftover bytes */
			if (keysize > 0)
			{
				unsigned char *keybyte = (unsigned char *) k;

				do
					h = (h * PRIME1) ^ (*keybyte++);
				while (--keysize > 0);
			}
			break;
	}

	h %= PRIME2;

	return h;
}
