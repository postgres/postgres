/*-------------------------------------------------------------------------
 *
 * syscache.h
 *	  System catalog cache definitions.
 *
 * See also lsyscache.h, which provides convenience routines for
 * common cache-lookup operations.
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/syscache.h,v 1.79 2010/02/14 18:42:18 rhaas Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SYSCACHE_H
#define SYSCACHE_H

#include "utils/catcache.h"

/*
 *		SysCache identifiers.
 *
 *		The order of these identifiers must match the order
 *		of the entries in the array cacheinfo[] in syscache.c.
 *		Keep them in alphabetical order (renumbering only costs a
 *		backend rebuild).
 */

enum SysCacheIdentifier
{
	AGGFNOID = 0,
	AMNAME,
	AMOID,
	AMOPOPID,
	AMOPSTRATEGY,
	AMPROCNUM,
	ATTNAME,
	ATTNUM,
	AUTHMEMMEMROLE,
	AUTHMEMROLEMEM,
	AUTHNAME,
	AUTHOID,
	CASTSOURCETARGET,
	CLAAMNAMENSP,
	CLAOID,
	CONDEFAULT,
	CONNAMENSP,
	CONSTROID,
	CONVOID,
	DATABASEOID,
	DEFACLROLENSPOBJ,
	ENUMOID,
	ENUMTYPOIDNAME,
	FOREIGNDATAWRAPPERNAME,
	FOREIGNDATAWRAPPEROID,
	FOREIGNSERVERNAME,
	FOREIGNSERVEROID,
	INDEXRELID,
	LANGNAME,
	LANGOID,
	NAMESPACENAME,
	NAMESPACEOID,
	OPERNAMENSP,
	OPEROID,
	OPFAMILYAMNAMENSP,
	OPFAMILYOID,
	PROCNAMEARGSNSP,
	PROCOID,
	RELNAMENSP,
	RELOID,
	RULERELNAME,
	STATRELATTINH,
	TABLESPACEOID,
	TSCONFIGMAP,
	TSCONFIGNAMENSP,
	TSCONFIGOID,
	TSDICTNAMENSP,
	TSDICTOID,
	TSPARSERNAMENSP,
	TSPARSEROID,
	TSTEMPLATENAMENSP,
	TSTEMPLATEOID,
	TYPENAMENSP,
	TYPEOID,
	USERMAPPINGOID,
	USERMAPPINGUSERSERVER
};

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

extern bool RelationSupportsSysCache(Oid relid);

/*
 * The use of the macros below rather than direct calls to the corresponding
 * functions is encouraged, as it insulates the caller from changes in the
 * maximum number of keys.
 */
#define SearchSysCache1(cacheId, key1) \
	SearchSysCache(cacheId, key1, 0, 0, 0)
#define SearchSysCache2(cacheId, key1, key2) \
	SearchSysCache(cacheId, key1, key2, 0, 0)
#define SearchSysCache3(cacheId, key1, key2, key3) \
	SearchSysCache(cacheId, key1, key2, key3, 0)
#define SearchSysCache4(cacheId, key1, key2, key3, key4) \
	SearchSysCache(cacheId, key1, key2, key3, key4)

#define SearchSysCacheCopy1(cacheId, key1) \
	SearchSysCacheCopy(cacheId, key1, 0, 0, 0)
#define SearchSysCacheCopy2(cacheId, key1, key2) \
	SearchSysCacheCopy(cacheId, key1, key2, 0, 0)
#define SearchSysCacheCopy3(cacheId, key1, key2, key3) \
	SearchSysCacheCopy(cacheId, key1, key2, key3, 0)
#define SearchSysCacheCopy4(cacheId, key1, key2, key3, key4) \
	SearchSysCacheCopy(cacheId, key1, key2, key3, key4)

#define SearchSysCacheExists1(cacheId, key1) \
	SearchSysCacheExists(cacheId, key1, 0, 0, 0)
#define SearchSysCacheExists2(cacheId, key1, key2) \
	SearchSysCacheExists(cacheId, key1, key2, 0, 0)
#define SearchSysCacheExists3(cacheId, key1, key2, key3) \
	SearchSysCacheExists(cacheId, key1, key2, key3, 0)
#define SearchSysCacheExists4(cacheId, key1, key2, key3, key4) \
	SearchSysCacheExists(cacheId, key1, key2, key3, key4)

#define GetSysCacheOid1(cacheId, key1) \
	GetSysCacheOid(cacheId, key1, 0, 0, 0)
#define GetSysCacheOid2(cacheId, key1, key2) \
	GetSysCacheOid(cacheId, key1, key2, 0, 0)
#define GetSysCacheOid3(cacheId, key1, key2, key3) \
	GetSysCacheOid(cacheId, key1, key2, key3, 0)
#define GetSysCacheOid4(cacheId, key1, key2, key3, key4) \
	GetSysCacheOid(cacheId, key1, key2, key3, key4)

#define SearchSysCacheList1(cacheId, key1) \
	SearchSysCacheList(cacheId, 1, key1, 0, 0, 0)
#define SearchSysCacheList2(cacheId, key1, key2) \
	SearchSysCacheList(cacheId, 2, key1, key2, 0, 0)
#define SearchSysCacheList3(cacheId, key1, key2, key3) \
	SearchSysCacheList(cacheId, 3, key1, key2, key3, 0)
#define SearchSysCacheList4(cacheId, key1, key2, key3, key4) \
	SearchSysCacheList(cacheId, 4, key1, key2, key3, key4)

#define ReleaseSysCacheList(x)	ReleaseCatCacheList(x)

#endif   /* SYSCACHE_H */
