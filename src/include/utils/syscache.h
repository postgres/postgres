/*-------------------------------------------------------------------------
 *
 * syscache.h
 *	  System catalog cache definitions.
 *
 * See also lsyscache.h, which provides convenience routines for
 * common cache-lookup operations.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: syscache.h,v 1.50 2002/07/18 23:11:32 petere Exp $
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
#define CASTSOURCETARGET 8
#define CLAAMNAMENSP	9
#define CLAOID			10
#define CONNAMESP		11
#define GRONAME			12
#define GROSYSID		13
#define INDEXRELID		14
#define INHRELID		15
#define LANGNAME		16
#define LANGOID			17
#define NAMESPACENAME	18
#define NAMESPACEOID	19
#define OPERNAMENSP		20
#define OPEROID			21
#define PROCNAMENSP		22
#define PROCOID			23
#define RELNAMENSP		24
#define RELOID			25
#define RULERELNAME		26
#define SHADOWNAME		27
#define SHADOWSYSID		28
#define STATRELATT		29
#define TYPENAMENSP		30
#define TYPEOID			31

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
