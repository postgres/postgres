/*-------------------------------------------------------------------------
 *
 * syscache.h
 *	  System catalog cache definitions.
 *
 * See also lsyscache.h, which provides convenience routines for
 * common cache-lookup operations.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/syscache.h,v 1.65 2006/07/13 18:01:02 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SYSCACHE_H
#define SYSCACHE_H

#include "utils/catcache.h"

/*
 *		Declarations for util/syscache.c.
 *
 *		SysCache identifiers.
 *
 *		The order of these must match the order
 *		they are entered into the structure cacheinfo[] in syscache.c.
 *		Keep them in alphabetical order.
 */

#define AGGFNOID		0
#define AMNAME			1
#define AMOID			2
#define AMOPOPID		3
#define AMOPSTRATEGY	4
#define AMPROCNUM		5
#define ATTNAME			6
#define ATTNUM			7
#define AUTHMEMMEMROLE	8
#define AUTHMEMROLEMEM	9
#define AUTHNAME		10
#define AUTHOID			11
#define CASTSOURCETARGET 12
#define CLAAMNAMENSP	13
#define CLAOID			14
#define CONDEFAULT		15
#define CONNAMENSP		16
#define CONOID			17
#define DATABASEOID		18
#define INDEXRELID		19
#define INHRELID		20
#define LANGNAME		21
#define LANGOID			22
#define NAMESPACENAME	23
#define NAMESPACEOID	24
#define OPERNAMENSP		25
#define OPEROID			26
#define PROCNAMEARGSNSP 27
#define PROCOID			28
#define RELNAMENSP		29
#define RELOID			30
#define RULERELNAME		31
#define STATRELATT		32
#define TYPENAMENSP		33
#define TYPEOID			34

extern void InitCatalogCache(void);
extern void InitCatalogCachePhase2(void);

extern HeapTuple SearchSysCache(int cacheId,
			   Datum key1, Datum key2, Datum key3, Datum key4);
extern void ReleaseSysCache(HeapTuple tuple);

/* convenience routines */
extern HeapTuple SearchSysCacheCopy(int cacheId,
				   Datum key1, Datum key2, Datum key3, Datum key4);
extern bool SearchSysCacheExists(int cacheId,
					 Datum key1, Datum key2, Datum key3, Datum key4);
extern Oid GetSysCacheOid(int cacheId,
			   Datum key1, Datum key2, Datum key3, Datum key4);

extern HeapTuple SearchSysCacheAttName(Oid relid, const char *attname);
extern HeapTuple SearchSysCacheCopyAttName(Oid relid, const char *attname);
extern bool SearchSysCacheExistsAttName(Oid relid, const char *attname);

extern Datum SysCacheGetAttr(int cacheId, HeapTuple tup,
				AttrNumber attributeNumber, bool *isNull);

/* list-search interface.  Users of this must import catcache.h too */
extern struct catclist *SearchSysCacheList(int cacheId, int nkeys,
				   Datum key1, Datum key2, Datum key3, Datum key4);

#define ReleaseSysCacheList(x)	ReleaseCatCacheList(x)

#endif   /* SYSCACHE_H */
