/*-------------------------------------------------------------------------
 *
 * hasht.c
 *	  hash table related functions that are not directly supported
 *	  by the hashing packages under utils/hash.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/lib/Attic/hasht.c,v 1.12 2000/01/26 05:56:26 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lib/hasht.h"
#include "utils/memutils.h"

/* -----------------------------------
 *		HashTableWalk
 *
 *		call function on every element in hashtable
 *		one extra argument, arg may be supplied
 * -----------------------------------
 */
void
HashTableWalk(HTAB *hashtable, HashtFunc function, int arg)
{
	long	   *hashent;
	long	   *data;
	int			keysize;

	keysize = hashtable->hctl->keysize;
	hash_seq((HTAB *) NULL);
	while ((hashent = hash_seq(hashtable)) != (long *) TRUE)
	{
		if (hashent == NULL)
			elog(FATAL, "error in HashTableWalk.");

		/*
		 * XXX the corresponding hash table insertion does NOT LONGALIGN
		 * -- make sure the keysize is ok
		 */
		data = (long *) LONGALIGN((char *) hashent + keysize);
		(*function) (data, arg);
	}
}
