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
 * $Id: syscache.h,v 1.49 2002/07/11 07:39:28 ishii Exp $
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
#define CONNAMESP		10
#define GRONAME			11
#define GROSYSID		12
#define INDEXRELID		13
#define INHRELID		14
#define LANGNAME		15
#define LANGOID			16
#define NAMESPACENAME	17
#define NAMESPACEOID	18
#define OPERNAMENSP		19
#define OPEROID			20
#define PROCNAMENSP		21
#define PROCOID			22
#define RELNAMENSP		23
#define RELOID			24
#define RULERELNAME		25
#define SHADOWNAME		26
#define SHADOWSYSID		27
#define STATRELATT		28
#define TYPENAMENSP		29
#define TYPEOID			30

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
