/*-------------------------------------------------------------------------
 *
 * syscache.h--
 *	  System catalog cache definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: syscache.h,v 1.8 1997/10/28 15:11:45 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SYSCACHE_H
#define SYSCACHE_H

#include <access/attnum.h>
#include <access/htup.h>

 /* #define CACHEDEBUG *//* turns DEBUG elogs on */


/*
 *		Declarations for util/syscache.c.
 *
 *		SysCache identifiers.
 *
 *		The order of these must match the order
 *		they are entered into the structure cacheinfo[] in syscache.c
 *		The best thing to do is to add yours at the END, because some
 *		code assumes that certain caches are at certain places in this
 *		array.
 */

#define AMOPOPID		0
#define AMOPSTRATEGY	1
#define ATTNAME			2
#define ATTNUM			3
#define INDEXRELID		4
#define LANNAME			5
#define OPRNAME			6
#define OPROID			7
#define PRONAME			8
#define PROOID			9
#define RELNAME			10
#define RELOID			11
#define TYPNAME			12
#define TYPOID			13
#define AMNAME			14
#define CLANAME			15
#define INDRELIDKEY		16
#define INHRELID		17
#define RULOID			18
#define AGGNAME			19
#define LISTENREL		20
#define USENAME			21
#define USESYSID		22
#define GRONAME			23
#define GROSYSID		24
#define REWRITENAME		25
#define PROSRC			26
#define CLADEFTYPE		27
#define LANOID			28

/* ----------------
 *		struct cachedesc:		information needed for a call to InitSysCache()
 * ----------------
 */
struct cachedesc
{
	char	   *name;			/* this is Name * so that we can
								 * initialize it */
	int			nkeys;
	int			key[4];
	int			size;			/* sizeof(appropriate struct) */
	char	   *indname;		/* index relation for this cache, if
								 * exists */
	HeapTuple	(*iScanFunc) ();/* function to handle index scans */
};

extern void zerocaches(void);
extern void InitCatalogCache(void);
extern HeapTuple
SearchSysCacheTuple(int cacheId, Datum key1, Datum key2,
					Datum key3, Datum key4);
extern int32
SearchSysCacheStruct(int cacheId, char *returnStruct,
					 Datum key1, Datum key2, Datum key3, Datum key4);
extern void *
SearchSysCacheGetAttribute(int cacheId,
						   AttrNumber attributeNumber,
						   Datum key1,
						   Datum key2,
						   Datum key3,
						   Datum key4);
extern void *TypeDefaultRetrieve(Oid typId);

#endif							/* SYSCACHE_H */
