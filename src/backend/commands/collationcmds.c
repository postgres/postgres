/*-------------------------------------------------------------------------
 *
 * collationcmds.c
 *	  collation-related commands support code
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/collationcmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_collation_fn.h"
#include "commands/alter.h"
#include "commands/collationcmds.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * CREATE COLLATION
 */
void
DefineCollation(List *names, List *parameters)
{
	char	   *collName;
	Oid			collNamespace;
	AclResult	aclresult;
	ListCell   *pl;
	DefElem    *fromEl = NULL;
	DefElem    *localeEl = NULL;
	DefElem    *lccollateEl = NULL;
	DefElem    *lcctypeEl = NULL;
	char	   *collcollate = NULL;
	char	   *collctype = NULL;
	Oid			newoid;

	collNamespace = QualifiedNameGetCreationNamespace(names, &collName);

	aclresult = pg_namespace_aclcheck(collNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   get_namespace_name(collNamespace));

	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);
		DefElem   **defelp;

		if (pg_strcasecmp(defel->defname, "from") == 0)
			defelp = &fromEl;
		else if (pg_strcasecmp(defel->defname, "locale") == 0)
			defelp = &localeEl;
		else if (pg_strcasecmp(defel->defname, "lc_collate") == 0)
			defelp = &lccollateEl;
		else if (pg_strcasecmp(defel->defname, "lc_ctype") == 0)
			defelp = &lcctypeEl;
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("collation attribute \"%s\" not recognized",
							defel->defname)));
			break;
		}

		*defelp = defel;
	}

	if ((localeEl && (lccollateEl || lcctypeEl))
		|| (fromEl && list_length(parameters) != 1))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("conflicting or redundant options")));

	if (fromEl)
	{
		Oid			collid;
		HeapTuple	tp;

		collid = get_collation_oid(defGetQualifiedName(fromEl), false);
		tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for collation %u", collid);

		collcollate = pstrdup(NameStr(((Form_pg_collation) GETSTRUCT(tp))->collcollate));
		collctype = pstrdup(NameStr(((Form_pg_collation) GETSTRUCT(tp))->collctype));

		ReleaseSysCache(tp);
	}

	if (localeEl)
	{
		collcollate = defGetString(localeEl);
		collctype = defGetString(localeEl);
	}

	if (lccollateEl)
		collcollate = defGetString(lccollateEl);

	if (lcctypeEl)
		collctype = defGetString(lcctypeEl);

	if (!collcollate)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("parameter \"lc_collate\" must be specified")));

	if (!collctype)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("parameter \"lc_ctype\" must be specified")));

	check_encoding_locale_matches(GetDatabaseEncoding(), collcollate, collctype);

	newoid = CollationCreate(collName,
							 collNamespace,
							 GetUserId(),
							 GetDatabaseEncoding(),
							 collcollate,
							 collctype);

	/* check that the locales can be loaded */
	CommandCounterIncrement();
	(void) pg_newlocale_from_collation(newoid);
}

/*
 * Rename collation
 */
void
RenameCollation(List *name, const char *newname)
{
	Oid			collationOid;
	Oid			namespaceOid;
	HeapTuple	tup;
	Relation	rel;
	AclResult	aclresult;

	rel = heap_open(CollationRelationId, RowExclusiveLock);

	collationOid = get_collation_oid(name, false);

	tup = SearchSysCacheCopy1(COLLOID, ObjectIdGetDatum(collationOid));
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for collation %u", collationOid);

	namespaceOid = ((Form_pg_collation) GETSTRUCT(tup))->collnamespace;

	/* make sure the new name doesn't exist */
	if (SearchSysCacheExists3(COLLNAMEENCNSP,
							  CStringGetDatum(newname),
							  Int32GetDatum(GetDatabaseEncoding()),
							  ObjectIdGetDatum(namespaceOid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("collation \"%s\" for encoding \"%s\" already exists in schema \"%s\"",
						newname,
						GetDatabaseEncodingName(),
						get_namespace_name(namespaceOid))));

	/* mustn't match an any-encoding entry, either */
	if (SearchSysCacheExists3(COLLNAMEENCNSP,
							  CStringGetDatum(newname),
							  Int32GetDatum(-1),
							  ObjectIdGetDatum(namespaceOid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("collation \"%s\" already exists in schema \"%s\"",
						newname,
						get_namespace_name(namespaceOid))));

	/* must be owner */
	if (!pg_collation_ownercheck(collationOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_COLLATION,
					   NameListToString(name));

	/* must have CREATE privilege on namespace */
	aclresult = pg_namespace_aclcheck(namespaceOid, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_NAMESPACE,
					   get_namespace_name(namespaceOid));

	/* rename */
	namestrcpy(&(((Form_pg_collation) GETSTRUCT(tup))->collname), newname);
	simple_heap_update(rel, &tup->t_self, tup);
	CatalogUpdateIndexes(rel, tup);

	heap_freetuple(tup);

	heap_close(rel, RowExclusiveLock);
}

/*
 * Execute ALTER COLLATION SET SCHEMA
 */
void
AlterCollationNamespace(List *name, const char *newschema)
{
	Oid			collOid,
				nspOid;

	collOid = get_collation_oid(name, false);

	nspOid = LookupCreationNamespace(newschema);

	AlterCollationNamespace_oid(collOid, nspOid);
}

/*
 * Change collation schema, by oid
 */
Oid
AlterCollationNamespace_oid(Oid collOid, Oid newNspOid)
{
	Oid			oldNspOid;
	Relation	rel;
	char	   *collation_name;

	rel = heap_open(CollationRelationId, RowExclusiveLock);

	/*
	 * We have to check for name collision ourselves, because
	 * AlterObjectNamespace_internal doesn't know how to deal with the encoding
	 * considerations.
	 */
	collation_name = get_collation_name(collOid);
	if (!collation_name)
		elog(ERROR, "cache lookup failed for collation %u", collOid);

	/* make sure the name doesn't already exist in new schema */
	if (SearchSysCacheExists3(COLLNAMEENCNSP,
							  CStringGetDatum(collation_name),
							  Int32GetDatum(GetDatabaseEncoding()),
							  ObjectIdGetDatum(newNspOid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("collation \"%s\" for encoding \"%s\" already exists in schema \"%s\"",
						collation_name,
						GetDatabaseEncodingName(),
						get_namespace_name(newNspOid))));

	/* mustn't match an any-encoding entry, either */
	if (SearchSysCacheExists3(COLLNAMEENCNSP,
							  CStringGetDatum(collation_name),
							  Int32GetDatum(-1),
							  ObjectIdGetDatum(newNspOid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("collation \"%s\" already exists in schema \"%s\"",
						collation_name,
						get_namespace_name(newNspOid))));

	/* OK, do the work */
	oldNspOid = AlterObjectNamespace_internal(rel, collOid, newNspOid);

	heap_close(rel, RowExclusiveLock);

	return oldNspOid;
}
