/*-------------------------------------------------------------------------
 *
 * syscache.h
 *	  System catalog cache definitions.
 *
 * See also lsyscache.h, which provides convenience routines for
 * common cache-lookup operations.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: syscache.h,v 1.25 2000/04/12 17:16:55 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SYSCACHE_H
#define SYSCACHE_H

#include "access/htup.h"

 /* #define CACHEDEBUG *//* turns DEBUG elogs on */


/*
 *		Declarations for util/syscache.c.
 *
 *		SysCache identifiers.
 *
 *		The order of these must match the order
 *		they are entered into the structure cacheinfo[] in syscache.c
 *		Keep them in alphabeticall order.
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

/* ----------------
 *		struct cachedesc:		information needed for a call to InitSysCache()
 * ----------------
 */
struct cachedesc
{
	char	   *name;			/* this is Name so that we can initialize
								 * it */
	int			nkeys;
	int			key[4];
	int			size;			/* sizeof(appropriate struct) */
	char	   *indname;		/* index relation for this cache, if
								 * exists */
	HeapTuple	(*iScanFunc) ();/* function to handle index scans */
};

extern void zerocaches(void);
extern void InitCatalogCache(void);
extern HeapTuple SearchSysCacheTupleCopy(int cacheId,
						Datum key1, Datum key2, Datum key3, Datum key4);
extern HeapTuple SearchSysCacheTuple(int cacheId,
					Datum key1, Datum key2, Datum key3, Datum key4);
extern Datum SysCacheGetAttr(int cacheId, HeapTuple tup,
				AttrNumber attributeNumber,
				bool *isnull);

#endif	 /* SYSCACHE_H */
