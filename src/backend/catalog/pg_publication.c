/*-------------------------------------------------------------------------
 *
 * pg_publication.c
 *		publication C API manipulation
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		pg_publication.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "funcapi.h"
#include "miscadmin.h"

#include "access/genam.h"
#include "access/hash.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/xact.h"

#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_type.h"
#include "catalog/pg_publication.h"
#include "catalog/pg_publication_rel.h"

#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * Check if relation can be in given publication and throws appropriate
 * error if not.
 */
static void
check_publication_add_relation(Relation targetrel)
{
	/* Must be table */
	if (RelationGetForm(targetrel)->relkind != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" is not a table",
						RelationGetRelationName(targetrel)),
				 errdetail("Only tables can be added to publications.")));

	/* Can't be system table */
	if (IsCatalogRelation(targetrel))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" is a system table",
						RelationGetRelationName(targetrel)),
				 errdetail("System tables cannot be added to publications.")));

	/* UNLOGGED and TEMP relations cannot be part of publication. */
	if (!RelationNeedsWAL(targetrel))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("table \"%s\" cannot be replicated",
						RelationGetRelationName(targetrel)),
				 errdetail("Temporary and unlogged relations cannot be replicated.")));
}

/*
 * Returns if relation represented by oid and Form_pg_class entry
 * is publishable.
 *
 * Does same checks as the above, but does not need relation to be opened
 * and also does not throw errors.
 */
static bool
is_publishable_class(Oid relid, Form_pg_class reltuple)
{
	return reltuple->relkind == RELKIND_RELATION &&
		!IsCatalogClass(relid, reltuple) &&
		reltuple->relpersistence == RELPERSISTENCE_PERMANENT &&
		/*
		 * Also exclude any tables created as part of initdb. This mainly
		 * affects the preinstalled information_schema.
		 * Note that IsCatalogClass() only checks for these inside pg_catalog
		 * and toast schemas.
		 */
		relid >= FirstNormalObjectId;
}

/*
 * Insert new publication / relation mapping.
 */
ObjectAddress
publication_add_relation(Oid pubid, Relation targetrel,
						 bool if_not_exists)
{
	Relation	rel;
	HeapTuple	tup;
	Datum		values[Natts_pg_publication_rel];
	bool		nulls[Natts_pg_publication_rel];
	Oid			relid = RelationGetRelid(targetrel);
	Oid			prrelid;
	Publication *pub = GetPublication(pubid);
	ObjectAddress	myself,
					referenced;

	rel = heap_open(PublicationRelRelationId, RowExclusiveLock);

	/*
	 * Check for duplicates. Note that this does not really prevent
	 * duplicates, it's here just to provide nicer error message in common
	 * case. The real protection is the unique key on the catalog.
	 */
	if (SearchSysCacheExists2(PUBLICATIONRELMAP, ObjectIdGetDatum(relid),
							  ObjectIdGetDatum(pubid)))
	{
		heap_close(rel, RowExclusiveLock);

		if (if_not_exists)
			return InvalidObjectAddress;

		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("relation \"%s\" is already member of publication \"%s\"",
						RelationGetRelationName(targetrel), pub->name)));
	}

	check_publication_add_relation(targetrel);

	/* Form a tuple. */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	values[Anum_pg_publication_rel_prpubid - 1] =
		ObjectIdGetDatum(pubid);
	values[Anum_pg_publication_rel_prrelid - 1] =
		ObjectIdGetDatum(relid);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

	/* Insert tuple into catalog. */
	prrelid = CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	ObjectAddressSet(myself, PublicationRelRelationId, prrelid);

	/* Add dependency on the publication */
	ObjectAddressSet(referenced, PublicationRelationId, pubid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

	/* Add dependency on the relation */
	ObjectAddressSet(referenced, RelationRelationId, relid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

	/* Close the table. */
	heap_close(rel, RowExclusiveLock);

	/* Invalidate relcache so that publication info is rebuilt. */
	CacheInvalidateRelcache(targetrel);

	return myself;
}


/*
 * Gets list of publication oids for a relation oid.
 */
List *
GetRelationPublications(Oid relid)
{
	List		   *result = NIL;
	CatCList	   *pubrellist;
	int				i;

	/* Find all publications associated with the relation. */
	pubrellist = SearchSysCacheList1(PUBLICATIONRELMAP,
									 ObjectIdGetDatum(relid));
	for (i = 0; i < pubrellist->n_members; i++)
	{
		HeapTuple	tup = &pubrellist->members[i]->tuple;
		Oid			pubid = ((Form_pg_publication_rel) GETSTRUCT(tup))->prpubid;

		result = lappend_oid(result, pubid);
	}

	ReleaseSysCacheList(pubrellist);

	return result;
}

/*
 * Gets list of relation oids for a publication.
 *
 * This should only be used for normal publications, the FOR ALL TABLES
 * should use GetAllTablesPublicationRelations().
 */
List *
GetPublicationRelations(Oid pubid)
{
	List		   *result;
	Relation		pubrelsrel;
	ScanKeyData		scankey;
	SysScanDesc		scan;
	HeapTuple		tup;

	/* Find all publications associated with the relation. */
	pubrelsrel = heap_open(PublicationRelRelationId, AccessShareLock);

	ScanKeyInit(&scankey,
				Anum_pg_publication_rel_prpubid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(pubid));

	scan = systable_beginscan(pubrelsrel, PublicationRelPrrelidPrpubidIndexId,
							  true, NULL, 1, &scankey);

	result = NIL;
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_publication_rel		pubrel;

		pubrel = (Form_pg_publication_rel) GETSTRUCT(tup);

		result = lappend_oid(result, pubrel->prrelid);
	}

	systable_endscan(scan);
	heap_close(pubrelsrel, AccessShareLock);

	return result;
}

/*
 * Gets list of publication oids for publications marked as FOR ALL TABLES.
 */
List *
GetAllTablesPublications(void)
{
	List		   *result;
	Relation		rel;
	ScanKeyData		scankey;
	SysScanDesc		scan;
	HeapTuple		tup;

	/* Find all publications that are marked as for all tables. */
	rel = heap_open(PublicationRelationId, AccessShareLock);

	ScanKeyInit(&scankey,
				Anum_pg_publication_puballtables,
				BTEqualStrategyNumber, F_BOOLEQ,
				BoolGetDatum(true));

	scan = systable_beginscan(rel, InvalidOid, false,
							  NULL, 1, &scankey);

	result = NIL;
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
		result = lappend_oid(result, HeapTupleGetOid(tup));

	systable_endscan(scan);
	heap_close(rel, AccessShareLock);

	return result;
}

/*
 * Gets list of all relation published by FOR ALL TABLES publication(s).
 */
List *
GetAllTablesPublicationRelations(void)
{
	Relation	classRel;
	ScanKeyData key[1];
	HeapScanDesc scan;
	HeapTuple	tuple;
	List	   *result = NIL;

	classRel = heap_open(RelationRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_class_relkind,
				BTEqualStrategyNumber, F_CHAREQ,
				CharGetDatum(RELKIND_RELATION));

	scan = heap_beginscan_catalog(classRel, 1, key);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Oid				relid = HeapTupleGetOid(tuple);
		Form_pg_class	relForm = (Form_pg_class) GETSTRUCT(tuple);

		if (is_publishable_class(relid, relForm))
			result = lappend_oid(result, relid);
	}

	heap_endscan(scan);
	heap_close(classRel, AccessShareLock);

	return result;
}

/*
 * Get publication using oid
 *
 * The Publication struct and it's data are palloced here.
 */
Publication *
GetPublication(Oid pubid)
{
	HeapTuple		tup;
	Publication	   *pub;
	Form_pg_publication	pubform;

	tup = SearchSysCache1(PUBLICATIONOID, ObjectIdGetDatum(pubid));

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for publication %u", pubid);

	pubform = (Form_pg_publication) GETSTRUCT(tup);

	pub = (Publication *) palloc(sizeof(Publication));
	pub->oid = pubid;
	pub->name = pstrdup(NameStr(pubform->pubname));
	pub->alltables = pubform->puballtables;
	pub->pubactions.pubinsert = pubform->pubinsert;
	pub->pubactions.pubupdate = pubform->pubupdate;
	pub->pubactions.pubdelete = pubform->pubdelete;

	ReleaseSysCache(tup);

	return pub;
}


/*
 * Get Publication using name.
 */
Publication *
GetPublicationByName(const char *pubname, bool missing_ok)
{
	Oid			oid;

	oid = GetSysCacheOid1(PUBLICATIONNAME, CStringGetDatum(pubname));
	if (!OidIsValid(oid))
	{
		if (missing_ok)
			return NULL;

		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("publication \"%s\" does not exist", pubname)));
	}

	return GetPublication(oid);
}

/*
 * get_publication_oid - given a publication name, look up the OID
 *
 * If missing_ok is false, throw an error if name not found.  If true, just
 * return InvalidOid.
 */
Oid
get_publication_oid(const char *pubname, bool missing_ok)
{
	Oid			oid;

	oid = GetSysCacheOid1(PUBLICATIONNAME, CStringGetDatum(pubname));
	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("publication \"%s\" does not exist", pubname)));
	return oid;
}

/*
 * get_publication_name - given a publication Oid, look up the name
 */
char *
get_publication_name(Oid pubid)
{
	HeapTuple		tup;
	char		   *pubname;
	Form_pg_publication	pubform;

	tup = SearchSysCache1(PUBLICATIONOID, ObjectIdGetDatum(pubid));

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for publication %u", pubid);

	pubform = (Form_pg_publication) GETSTRUCT(tup);
	pubname = pstrdup(NameStr(pubform->pubname));

	ReleaseSysCache(tup);

	return pubname;
}

/*
 * Returns Oids of tables in a publication.
 */
Datum
pg_get_publication_tables(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	char		   *pubname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Publication	   *publication;
	List		   *tables;
	ListCell	  **lcp;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/* switch to memory context appropriate for multiple function calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		publication = GetPublicationByName(pubname, false);
		if (publication->alltables)
			tables = GetAllTablesPublicationRelations();
		else
			tables = GetPublicationRelations(publication->oid);
		lcp = (ListCell **) palloc(sizeof(ListCell *));
		*lcp = list_head(tables);
		funcctx->user_fctx = (void *) lcp;

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();
	lcp = (ListCell **) funcctx->user_fctx;

	while (*lcp != NULL)
	{
		Oid		relid = lfirst_oid(*lcp);

		*lcp = lnext(*lcp);
		SRF_RETURN_NEXT(funcctx, ObjectIdGetDatum(relid));
	}

	SRF_RETURN_DONE(funcctx);
}
