/*-------------------------------------------------------------------------
 *
 * hasht.c
 *	  hash table related functions that are not directly supported
 *	  by the hashing packages under utils/hash.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/lib/Attic/hasht.c,v 1.15 2001/01/24 19:42:55 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lib/hasht.h"
#include "utils/memutils.h"

/* -----------------------------------
 *		HashTableWalk
 *
 * call given function on every element in hashtable
 *
 * one extra argument (arg) may be supplied
 *
 * NOTE: it is allowed for the given function to delete the hashtable entry
 * it is passed.  However, deleting any other element while the scan is
 * in progress is UNDEFINED (see hash_seq functions).  Also, if elements are
 * added to the table while the scan is in progress, it is unspecified
 * whether they will be visited by the scan or not.
 * -----------------------------------
 */
void
HashTableWalk(HTAB *hashtable, HashtFunc function, Datum arg)
{
	HASH_SEQ_STATUS status;
	long	   *hashent;
	void	   *data;
	int			keysize;

	hash_seq_init(&status, hashtable);
	keysize = hashtable->hctl->keysize;

	while ((hashent = hash_seq_search(&status)) != (long *) TRUE)
	{
		if (hashent == NULL)
			elog(FATAL, "error in HashTableWalk");

		/*
		 * XXX the corresponding hash table insertion does NOT LONGALIGN
		 * -- make sure the keysize is ok
		 */
		data = (void *) LONGALIGN((char *) hashent + keysize);
		(*function) (data, arg);
	}
}
