/*-------------------------------------------------------------------------
 *
 * syscache.h
 *	  System catalog cache definitions.
 *
 * See also lsyscache.h, which provides convenience routines for
 * common cache-lookup operations.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: syscache.h,v 1.47 2002/04/18 20:01:11 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SYSCACHE_H
#define SYSCACHE_H

#include "access/htup.h"

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
#define CLAAMNAMENSP	8
#define CLAOID			9
#define GRONAME			10
#define GROSYSID		11
#define INDEXRELID		12
#define INHRELID		13
#define LANGNAME		14
#define LANGOID			15
#define NAMESPACENAME	16
#define NAMESPACEOID	17
#define OPERNAMENSP		18
#define OPEROID			19
#define PROCNAMENSP		20
#define PROCOID			21
#define RELNAMENSP		22
#define RELOID			23
#define RULERELNAME		24
#define SHADOWNAME		25
#define SHADOWSYSID		26
#define STATRELATT		27
#define TYPENAMENSP		28
#define TYPEOID			29


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

extern Datum SysCacheGetAttr(int cacheId, HeapTuple tup,
				AttrNumber attributeNumber, bool *isNull);

/* list-search interface.  Users of this must import catcache.h too */
extern struct catclist *SearchSysCacheList(int cacheId, int nkeys,
			   Datum key1, Datum key2, Datum key3, Datum key4);
#define ReleaseSysCacheList(x)  ReleaseCatCacheList(x)

#endif   /* SYSCACHE_H */
