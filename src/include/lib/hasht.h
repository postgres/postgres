/*-------------------------------------------------------------------------
 *
 * hasht.h
 *	  hash table related functions that are not directly supported
 *	  under utils/hash.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: hasht.h,v 1.10 2000/01/31 04:35:55 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HASHT_H
#define HASHT_H

#include "utils/hsearch.h"

typedef void (*HashtFunc) (void *hashitem, int arg);

extern void HashTableWalk(HTAB *hashtable, HashtFunc function, int arg);

#endif	 /* HASHT_H */
