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
 * $Id: syscache.h,v 1.35 2001/10/28 06:26:09 momjian Exp $
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
#define AMNAME			1
#define AMOPOPID		2
#define AMOPSTRATEGY	3
#define AMPROCNUM		4
#define ATTNAME			5
#define ATTNUM			6
#define CLAAMNAME		7
#define CLAOID			8
#define GRONAME			9
#define GROSYSID		10
#define INDEXRELID		11
#define INHRELID		12
#define LANGNAME		13
#define LANGOID			14
#define OPERNAME		15
#define OPEROID			16
#define PROCNAME		17
#define PROCOID			18
#define RELNAME			19
#define RELOID			20
#define RULENAME		21
#define SHADOWNAME		22
#define SHADOWSYSID		23
#define STATRELATT		24
#define TYPENAME		25
#define TYPEOID			26

extern void InitCatalogCache(void);

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

#endif	 /* SYSCACHE_H */
