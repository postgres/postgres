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
 * $Id: syscache.h,v 1.38 2002/03/21 23:27:25 tgl Exp $
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

#define AGGNAME			0
#define AGGOID			1
#define AMNAME			2
#define AMOID			3
#define AMOPOPID		4
#define AMOPSTRATEGY	5
#define AMPROCNUM		6
#define ATTNAME			7
#define ATTNUM			8
#define CLAAMNAME		9
#define CLAOID			10
#define GRONAME			11
#define GROSYSID		12
#define INDEXRELID		13
#define INHRELID		14
#define LANGNAME		15
#define LANGOID			16
#define OPERNAME		17
#define OPEROID			18
#define PROCNAME		19
#define PROCOID			20
#define RELNAME			21
#define RELOID			22
#define RULENAME		23
#define SHADOWNAME		24
#define SHADOWSYSID		25
#define STATRELATT		26
#define TYPENAME		27
#define TYPEOID			28


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

#endif   /* SYSCACHE_H */
