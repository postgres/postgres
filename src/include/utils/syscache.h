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
 * $Id: syscache.h,v 1.28 2001/01/24 19:43:29 momjian Exp $
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
#define ATTNAME			4
#define ATTNUM			5
#define CLADEFTYPE		6
#define CLANAME			7
#define GRONAME			8
#define GROSYSID		9
#define INDEXRELID		10
#define INHRELID		11
#define LANGNAME		12
#define LANGOID			13
#define LISTENREL		14
#define OPERNAME		15
#define OPEROID			16
#define PROCNAME		17
#define PROCOID			18
#define RELNAME			19
#define RELOID			20
#define RULENAME		21
#define RULEOID			22
#define SHADOWNAME		23
#define SHADOWSYSID		24
#define STATRELID		25
#define TYPENAME		26
#define TYPEOID			27

extern void InitCatalogCache(void);

extern HeapTuple SearchSysCache(int cacheId,
					Datum key1, Datum key2, Datum key3, Datum key4);
extern void ReleaseSysCache(HeapTuple tuple);

/* convenience routines */
extern HeapTuple SearchSysCacheCopy(int cacheId,
					Datum key1, Datum key2, Datum key3, Datum key4);
extern Oid GetSysCacheOid(int cacheId,
					Datum key1, Datum key2, Datum key3, Datum key4);

/* macro for just probing for existence of a tuple via the syscache */
#define SearchSysCacheExists(c,k1,k2,k3,k4)  \
	OidIsValid(GetSysCacheOid(c,k1,k2,k3,k4))

extern Datum SysCacheGetAttr(int cacheId, HeapTuple tup,
				AttrNumber attributeNumber, bool *isNull);

#endif	 /* SYSCACHE_H */
