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
 * $Id: hasht.h,v 1.11 2001/01/02 04:33:24 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HASHT_H
#define HASHT_H

#include "utils/hsearch.h"

typedef void (*HashtFunc) (void *hashitem, Datum arg);

extern void HashTableWalk(HTAB *hashtable, HashtFunc function, Datum arg);

#endif	 /* HASHT_H */
