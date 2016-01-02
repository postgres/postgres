/*-------------------------------------------------------------------------
 *
 * ts_cache.c
 *	  Tsearch related object caches.
 *
 * Tsearch performance is very sensitive to performance of parsers,
 * dictionaries and mapping, so lookups should be cached as much
 * as possible.
 *
 * Once a backend has created a cache entry for a particular TS object OID,
 * the cache entry will exist for the life of the backend; hence it is
 * safe to hold onto a pointer to the cache entry while doing things that
 * might result in recognizing a cache invalidation.  Beware however that
 * subsidiary information might be deleted and reallocated somewhere else
 * if a cache inval and reval happens!	This does not look like it will be
 * a big problem as long as parser and dictionary methods do not attempt
 * any database access.
 *
 *
 * Copyright (c) 2006-2016, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/ts_cache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_config_map.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "commands/defrem.h"
#include "tsearch/ts_cache.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/tqual.h"


/*
 * MAXTOKENTYPE/MAXDICTSPERTT are arbitrary limits on the workspace size
 * used in lookup_ts_config_cache().  We could avoid hardwiring a limit
 * by making the workspace dynamically enlargeable, but it seems unlikely
 * to be worth the trouble.
 */
#define MAXTOKENTYPE	256
#define MAXDICTSPERTT	100


static HTAB *TSParserCacheHash = NULL;
static TSParserCacheEntry *lastUsedParser = NULL;

static HTAB *TSDictionaryCacheHash = NULL;
static TSDictionaryCacheEntry *lastUsedDictionary = NULL;

static HTAB *TSConfigCacheHash = NULL;
static TSConfigCacheEntry *lastUsedConfig = NULL;

/*
 * GUC default_text_search_config, and a cache of the current config's OID
 */
char	   *TSCurrentConfig = NULL;

static Oid	TSCurrentConfigCache = InvalidOid;


/*
 * We use this syscache callback to detect when a visible change to a TS
 * catalog entry has been made, by either our own backend or another one.
 *
 * In principle we could just flush the specific cache entry that changed,
 * but given that TS configuration changes are probably infrequent, it
 * doesn't seem worth the trouble to determine that; we just flush all the
 * entries of the related hash table.
 *
 * We can use the same function for all TS caches by passing the hash
 * table address as the "arg".
 */
static void
InvalidateTSCacheCallBack(Datum arg, int cacheid, uint32 hashvalue)
{
	HTAB	   *hash = (HTAB *) DatumGetPointer(arg);
	HASH_SEQ_STATUS status;
	TSAnyCacheEntry *entry;

	hash_seq_init(&status, hash);
	while ((entry = (TSAnyCacheEntry *) hash_seq_search(&status)) != NULL)
		entry->isvalid = false;

	/* Also invalidate the current-config cache if it's pg_ts_config */
	if (hash == TSConfigCacheHash)
		TSCurrentConfigCache = InvalidOid;
}

/*
 * Fetch parser cache entry
 */
TSParserCacheEntry *
lookup_ts_parser_cache(Oid prsId)
{
	TSParserCacheEntry *entry;

	if (TSParserCacheHash == NULL)
	{
		/* First time through: initialize the hash table */
		HASHCTL		ctl;

		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(TSParserCacheEntry);
		TSParserCacheHash = hash_create("Tsearch parser cache", 4,
										&ctl, HASH_ELEM | HASH_BLOBS);
		/* Flush cache on pg_ts_parser changes */
		CacheRegisterSyscacheCallback(TSPARSEROID, InvalidateTSCacheCallBack,
									  PointerGetDatum(TSParserCacheHash));

		/* Also make sure CacheMemoryContext exists */
		if (!CacheMemoryContext)
			CreateCacheMemoryContext();
	}

	/* Check single-entry cache */
	if (lastUsedParser && lastUsedParser->prsId == prsId &&
		lastUsedParser->isvalid)
		return lastUsedParser;

	/* Try to look up an existing entry */
	entry = (TSParserCacheEntry *) hash_search(TSParserCacheHash,
											   (void *) &prsId,
											   HASH_FIND, NULL);
	if (entry == NULL || !entry->isvalid)
	{
		/*
		 * If we didn't find one, we want to make one. But first look up the
		 * object to be sure the OID is real.
		 */
		HeapTuple	tp;
		Form_pg_ts_parser prs;

		tp = SearchSysCache1(TSPARSEROID, ObjectIdGetDatum(prsId));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for text search parser %u",
				 prsId);
		prs = (Form_pg_ts_parser) GETSTRUCT(tp);

		/*
		 * Sanity checks
		 */
		if (!OidIsValid(prs->prsstart))
			elog(ERROR, "text search parser %u has no prsstart method", prsId);
		if (!OidIsValid(prs->prstoken))
			elog(ERROR, "text search parser %u has no prstoken method", prsId);
		if (!OidIsValid(prs->prsend))
			elog(ERROR, "text search parser %u has no prsend method", prsId);

		if (entry == NULL)
		{
			bool		found;

			/* Now make the cache entry */
			entry = (TSParserCacheEntry *)
				hash_search(TSParserCacheHash,
							(void *) &prsId,
							HASH_ENTER, &found);
			Assert(!found);		/* it wasn't there a moment ago */
		}

		MemSet(entry, 0, sizeof(TSParserCacheEntry));
		entry->prsId = prsId;
		entry->startOid = prs->prsstart;
		entry->tokenOid = prs->prstoken;
		entry->endOid = prs->prsend;
		entry->headlineOid = prs->prsheadline;
		entry->lextypeOid = prs->prslextype;

		ReleaseSysCache(tp);

		fmgr_info_cxt(entry->startOid, &entry->prsstart, CacheMemoryContext);
		fmgr_info_cxt(entry->tokenOid, &entry->prstoken, CacheMemoryContext);
		fmgr_info_cxt(entry->endOid, &entry->prsend, CacheMemoryContext);
		if (OidIsValid(entry->headlineOid))
			fmgr_info_cxt(entry->headlineOid, &entry->prsheadline,
						  CacheMemoryContext);

		entry->isvalid = true;
	}

	lastUsedParser = entry;

	return entry;
}

/*
 * Fetch dictionary cache entry
 */
TSDictionaryCacheEntry *
lookup_ts_dictionary_cache(Oid dictId)
{
	TSDictionaryCacheEntry *entry;

	if (TSDictionaryCacheHash == NULL)
	{
		/* First time through: initialize the hash table */
		HASHCTL		ctl;

		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(TSDictionaryCacheEntry);
		TSDictionaryCacheHash = hash_create("Tsearch dictionary cache", 8,
											&ctl, HASH_ELEM | HASH_BLOBS);
		/* Flush cache on pg_ts_dict and pg_ts_template changes */
		CacheRegisterSyscacheCallback(TSDICTOID, InvalidateTSCacheCallBack,
									  PointerGetDatum(TSDictionaryCacheHash));
		CacheRegisterSyscacheCallback(TSTEMPLATEOID, InvalidateTSCacheCallBack,
									  PointerGetDatum(TSDictionaryCacheHash));

		/* Also make sure CacheMemoryContext exists */
		if (!CacheMemoryContext)
			CreateCacheMemoryContext();
	}

	/* Check single-entry cache */
	if (lastUsedDictionary && lastUsedDictionary->dictId == dictId &&
		lastUsedDictionary->isvalid)
		return lastUsedDictionary;

	/* Try to look up an existing entry */
	entry = (TSDictionaryCacheEntry *) hash_search(TSDictionaryCacheHash,
												   (void *) &dictId,
												   HASH_FIND, NULL);
	if (entry == NULL || !entry->isvalid)
	{
		/*
		 * If we didn't find one, we want to make one. But first look up the
		 * object to be sure the OID is real.
		 */
		HeapTuple	tpdict,
					tptmpl;
		Form_pg_ts_dict dict;
		Form_pg_ts_template template;
		MemoryContext saveCtx;

		tpdict = SearchSysCache1(TSDICTOID, ObjectIdGetDatum(dictId));
		if (!HeapTupleIsValid(tpdict))
			elog(ERROR, "cache lookup failed for text search dictionary %u",
				 dictId);
		dict = (Form_pg_ts_dict) GETSTRUCT(tpdict);

		/*
		 * Sanity checks
		 */
		if (!OidIsValid(dict->dicttemplate))
			elog(ERROR, "text search dictionary %u has no template", dictId);

		/*
		 * Retrieve dictionary's template
		 */
		tptmpl = SearchSysCache1(TSTEMPLATEOID,
								 ObjectIdGetDatum(dict->dicttemplate));
		if (!HeapTupleIsValid(tptmpl))
			elog(ERROR, "cache lookup failed for text search template %u",
				 dict->dicttemplate);
		template = (Form_pg_ts_template) GETSTRUCT(tptmpl);

		/*
		 * Sanity checks
		 */
		if (!OidIsValid(template->tmpllexize))
			elog(ERROR, "text search template %u has no lexize method",
				 template->tmpllexize);

		if (entry == NULL)
		{
			bool		found;

			/* Now make the cache entry */
			entry = (TSDictionaryCacheEntry *)
				hash_search(TSDictionaryCacheHash,
							(void *) &dictId,
							HASH_ENTER, &found);
			Assert(!found);		/* it wasn't there a moment ago */

			/* Create private memory context the first time through */
			saveCtx = AllocSetContextCreate(CacheMemoryContext,
											NameStr(dict->dictname),
											ALLOCSET_SMALL_MINSIZE,
											ALLOCSET_SMALL_INITSIZE,
											ALLOCSET_SMALL_MAXSIZE);
		}
		else
		{
			/* Clear the existing entry's private context */
			saveCtx = entry->dictCtx;
			MemoryContextResetAndDeleteChildren(saveCtx);
		}

		MemSet(entry, 0, sizeof(TSDictionaryCacheEntry));
		entry->dictId = dictId;
		entry->dictCtx = saveCtx;

		entry->lexizeOid = template->tmpllexize;

		if (OidIsValid(template->tmplinit))
		{
			List	   *dictoptions;
			Datum		opt;
			bool		isnull;
			MemoryContext oldcontext;

			/*
			 * Init method runs in dictionary's private memory context, and we
			 * make sure the options are stored there too
			 */
			oldcontext = MemoryContextSwitchTo(entry->dictCtx);

			opt = SysCacheGetAttr(TSDICTOID, tpdict,
								  Anum_pg_ts_dict_dictinitoption,
								  &isnull);
			if (isnull)
				dictoptions = NIL;
			else
				dictoptions = deserialize_deflist(opt);

			entry->dictData =
				DatumGetPointer(OidFunctionCall1(template->tmplinit,
											  PointerGetDatum(dictoptions)));

			MemoryContextSwitchTo(oldcontext);
		}

		ReleaseSysCache(tptmpl);
		ReleaseSysCache(tpdict);

		fmgr_info_cxt(entry->lexizeOid, &entry->lexize, entry->dictCtx);

		entry->isvalid = true;
	}

	lastUsedDictionary = entry;

	return entry;
}

/*
 * Initialize config cache and prepare callbacks.  This is split out of
 * lookup_ts_config_cache because we need to activate the callback before
 * caching TSCurrentConfigCache, too.
 */
static void
init_ts_config_cache(void)
{
	HASHCTL		ctl;

	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(TSConfigCacheEntry);
	TSConfigCacheHash = hash_create("Tsearch configuration cache", 16,
									&ctl, HASH_ELEM | HASH_BLOBS);
	/* Flush cache on pg_ts_config and pg_ts_config_map changes */
	CacheRegisterSyscacheCallback(TSCONFIGOID, InvalidateTSCacheCallBack,
								  PointerGetDatum(TSConfigCacheHash));
	CacheRegisterSyscacheCallback(TSCONFIGMAP, InvalidateTSCacheCallBack,
								  PointerGetDatum(TSConfigCacheHash));

	/* Also make sure CacheMemoryContext exists */
	if (!CacheMemoryContext)
		CreateCacheMemoryContext();
}

/*
 * Fetch configuration cache entry
 */
TSConfigCacheEntry *
lookup_ts_config_cache(Oid cfgId)
{
	TSConfigCacheEntry *entry;

	if (TSConfigCacheHash == NULL)
	{
		/* First time through: initialize the hash table */
		init_ts_config_cache();
	}

	/* Check single-entry cache */
	if (lastUsedConfig && lastUsedConfig->cfgId == cfgId &&
		lastUsedConfig->isvalid)
		return lastUsedConfig;

	/* Try to look up an existing entry */
	entry = (TSConfigCacheEntry *) hash_search(TSConfigCacheHash,
											   (void *) &cfgId,
											   HASH_FIND, NULL);
	if (entry == NULL || !entry->isvalid)
	{
		/*
		 * If we didn't find one, we want to make one. But first look up the
		 * object to be sure the OID is real.
		 */
		HeapTuple	tp;
		Form_pg_ts_config cfg;
		Relation	maprel;
		Relation	mapidx;
		ScanKeyData mapskey;
		SysScanDesc mapscan;
		HeapTuple	maptup;
		ListDictionary maplists[MAXTOKENTYPE + 1];
		Oid			mapdicts[MAXDICTSPERTT];
		int			maxtokentype;
		int			ndicts;
		int			i;

		tp = SearchSysCache1(TSCONFIGOID, ObjectIdGetDatum(cfgId));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for text search configuration %u",
				 cfgId);
		cfg = (Form_pg_ts_config) GETSTRUCT(tp);

		/*
		 * Sanity checks
		 */
		if (!OidIsValid(cfg->cfgparser))
			elog(ERROR, "text search configuration %u has no parser", cfgId);

		if (entry == NULL)
		{
			bool		found;

			/* Now make the cache entry */
			entry = (TSConfigCacheEntry *)
				hash_search(TSConfigCacheHash,
							(void *) &cfgId,
							HASH_ENTER, &found);
			Assert(!found);		/* it wasn't there a moment ago */
		}
		else
		{
			/* Cleanup old contents */
			if (entry->map)
			{
				for (i = 0; i < entry->lenmap; i++)
					if (entry->map[i].dictIds)
						pfree(entry->map[i].dictIds);
				pfree(entry->map);
			}
		}

		MemSet(entry, 0, sizeof(TSConfigCacheEntry));
		entry->cfgId = cfgId;
		entry->prsId = cfg->cfgparser;

		ReleaseSysCache(tp);

		/*
		 * Scan pg_ts_config_map to gather dictionary list for each token type
		 *
		 * Because the index is on (mapcfg, maptokentype, mapseqno), we will
		 * see the entries in maptokentype order, and in mapseqno order for
		 * each token type, even though we didn't explicitly ask for that.
		 */
		MemSet(maplists, 0, sizeof(maplists));
		maxtokentype = 0;
		ndicts = 0;

		ScanKeyInit(&mapskey,
					Anum_pg_ts_config_map_mapcfg,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(cfgId));

		maprel = heap_open(TSConfigMapRelationId, AccessShareLock);
		mapidx = index_open(TSConfigMapIndexId, AccessShareLock);
		mapscan = systable_beginscan_ordered(maprel, mapidx,
											 NULL, 1, &mapskey);

		while ((maptup = systable_getnext_ordered(mapscan, ForwardScanDirection)) != NULL)
		{
			Form_pg_ts_config_map cfgmap = (Form_pg_ts_config_map) GETSTRUCT(maptup);
			int			toktype = cfgmap->maptokentype;

			if (toktype <= 0 || toktype > MAXTOKENTYPE)
				elog(ERROR, "maptokentype value %d is out of range", toktype);
			if (toktype < maxtokentype)
				elog(ERROR, "maptokentype entries are out of order");
			if (toktype > maxtokentype)
			{
				/* starting a new token type, but first save the prior data */
				if (ndicts > 0)
				{
					maplists[maxtokentype].len = ndicts;
					maplists[maxtokentype].dictIds = (Oid *)
						MemoryContextAlloc(CacheMemoryContext,
										   sizeof(Oid) * ndicts);
					memcpy(maplists[maxtokentype].dictIds, mapdicts,
						   sizeof(Oid) * ndicts);
				}
				maxtokentype = toktype;
				mapdicts[0] = cfgmap->mapdict;
				ndicts = 1;
			}
			else
			{
				/* continuing data for current token type */
				if (ndicts >= MAXDICTSPERTT)
					elog(ERROR, "too many pg_ts_config_map entries for one token type");
				mapdicts[ndicts++] = cfgmap->mapdict;
			}
		}

		systable_endscan_ordered(mapscan);
		index_close(mapidx, AccessShareLock);
		heap_close(maprel, AccessShareLock);

		if (ndicts > 0)
		{
			/* save the last token type's dictionaries */
			maplists[maxtokentype].len = ndicts;
			maplists[maxtokentype].dictIds = (Oid *)
				MemoryContextAlloc(CacheMemoryContext,
								   sizeof(Oid) * ndicts);
			memcpy(maplists[maxtokentype].dictIds, mapdicts,
				   sizeof(Oid) * ndicts);
			/* and save the overall map */
			entry->lenmap = maxtokentype + 1;
			entry->map = (ListDictionary *)
				MemoryContextAlloc(CacheMemoryContext,
								   sizeof(ListDictionary) * entry->lenmap);
			memcpy(entry->map, maplists,
				   sizeof(ListDictionary) * entry->lenmap);
		}

		entry->isvalid = true;
	}

	lastUsedConfig = entry;

	return entry;
}


/*---------------------------------------------------
 * GUC variable "default_text_search_config"
 *---------------------------------------------------
 */

Oid
getTSCurrentConfig(bool emitError)
{
	/* if we have a cached value, return it */
	if (OidIsValid(TSCurrentConfigCache))
		return TSCurrentConfigCache;

	/* fail if GUC hasn't been set up yet */
	if (TSCurrentConfig == NULL || *TSCurrentConfig == '\0')
	{
		if (emitError)
			elog(ERROR, "text search configuration isn't set");
		else
			return InvalidOid;
	}

	if (TSConfigCacheHash == NULL)
	{
		/* First time through: initialize the tsconfig inval callback */
		init_ts_config_cache();
	}

	/* Look up the config */
	TSCurrentConfigCache =
		get_ts_config_oid(stringToQualifiedNameList(TSCurrentConfig),
						  !emitError);

	return TSCurrentConfigCache;
}

/* GUC check_hook for default_text_search_config */
bool
check_TSCurrentConfig(char **newval, void **extra, GucSource source)
{
	/*
	 * If we aren't inside a transaction, we cannot do database access so
	 * cannot verify the config name.  Must accept it on faith.
	 */
	if (IsTransactionState())
	{
		Oid			cfgId;
		HeapTuple	tuple;
		Form_pg_ts_config cfg;
		char	   *buf;

		cfgId = get_ts_config_oid(stringToQualifiedNameList(*newval), true);

		/*
		 * When source == PGC_S_TEST, don't throw a hard error for a
		 * nonexistent configuration, only a NOTICE.  See comments in guc.h.
		 */
		if (!OidIsValid(cfgId))
		{
			if (source == PGC_S_TEST)
			{
				ereport(NOTICE,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("text search configuration \"%s\" does not exist", *newval)));
				return true;
			}
			else
				return false;
		}

		/*
		 * Modify the actually stored value to be fully qualified, to ensure
		 * later changes of search_path don't affect it.
		 */
		tuple = SearchSysCache1(TSCONFIGOID, ObjectIdGetDatum(cfgId));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for text search configuration %u",
				 cfgId);
		cfg = (Form_pg_ts_config) GETSTRUCT(tuple);

		buf = quote_qualified_identifier(get_namespace_name(cfg->cfgnamespace),
										 NameStr(cfg->cfgname));

		ReleaseSysCache(tuple);

		/* GUC wants it malloc'd not palloc'd */
		free(*newval);
		*newval = strdup(buf);
		pfree(buf);
		if (!*newval)
			return false;
	}

	return true;
}

/* GUC assign_hook for default_text_search_config */
void
assign_TSCurrentConfig(const char *newval, void *extra)
{
	/* Just reset the cache to force a lookup on first use */
	TSCurrentConfigCache = InvalidOid;
}
