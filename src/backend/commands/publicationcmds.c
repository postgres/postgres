/*-------------------------------------------------------------------------
 *
 * publicationcmds.c
 *		publication manipulation
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		publicationcmds.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "funcapi.h"
#include "miscadmin.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"

#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "catalog/pg_publication.h"
#include "catalog/pg_publication_rel.h"

#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/publicationcmds.h"

#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/varlena.h"

/* Same as MAXNUMMESSAGES in sinvaladt.c */
#define MAX_RELCACHE_INVAL_MSGS 4096

static List *OpenTableList(List *tables);
static void CloseTableList(List *rels);
static void PublicationAddTables(Oid pubid, List *rels, bool if_not_exists,
								 AlterPublicationStmt *stmt);
static void PublicationDropTables(Oid pubid, List *rels, bool missing_ok);

static void
parse_publication_options(List *options,
						  bool *publish_given,
						  bool *publish_insert,
						  bool *publish_update,
						  bool *publish_delete,
						  bool *publish_truncate)
{
	ListCell   *lc;

	*publish_given = false;

	/* Defaults are true */
	*publish_insert = true;
	*publish_update = true;
	*publish_delete = true;
	*publish_truncate = true;

	/* Parse options */
	foreach(lc, options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strcmp(defel->defname, "publish") == 0)
		{
			char	   *publish;
			List	   *publish_list;
			ListCell   *lc;

			if (*publish_given)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));

			/*
			 * If publish option was given only the explicitly listed actions
			 * should be published.
			 */
			*publish_insert = false;
			*publish_update = false;
			*publish_delete = false;
			*publish_truncate = false;

			*publish_given = true;
			publish = defGetString(defel);

			if (!SplitIdentifierString(publish, ',', &publish_list))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid list syntax for \"publish\" option")));

			/* Process the option list. */
			foreach(lc, publish_list)
			{
				char	   *publish_opt = (char *) lfirst(lc);

				if (strcmp(publish_opt, "insert") == 0)
					*publish_insert = true;
				else if (strcmp(publish_opt, "update") == 0)
					*publish_update = true;
				else if (strcmp(publish_opt, "delete") == 0)
					*publish_delete = true;
				else if (strcmp(publish_opt, "truncate") == 0)
					*publish_truncate = true;
				else
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("unrecognized \"publish\" value: \"%s\"", publish_opt)));
			}
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized publication parameter: \"%s\"", defel->defname)));
	}
}

/*
 * Create new publication.
 */
ObjectAddress
CreatePublication(CreatePublicationStmt *stmt)
{
	Relation	rel;
	ObjectAddress myself;
	Oid			puboid;
	bool		nulls[Natts_pg_publication];
	Datum		values[Natts_pg_publication];
	HeapTuple	tup;
	bool		publish_given;
	bool		publish_insert;
	bool		publish_update;
	bool		publish_delete;
	bool		publish_truncate;
	AclResult	aclresult;

	/* must have CREATE privilege on database */
	aclresult = pg_database_aclcheck(MyDatabaseId, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_DATABASE,
					   get_database_name(MyDatabaseId));

	/* FOR ALL TABLES requires superuser */
	if (stmt->for_all_tables && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to create FOR ALL TABLES publication"))));

	rel = table_open(PublicationRelationId, RowExclusiveLock);

	/* Check if name is used */
	puboid = GetSysCacheOid1(PUBLICATIONNAME, Anum_pg_publication_oid,
							 CStringGetDatum(stmt->pubname));
	if (OidIsValid(puboid))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("publication \"%s\" already exists",
						stmt->pubname)));
	}

	/* Form a tuple. */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	values[Anum_pg_publication_pubname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->pubname));
	values[Anum_pg_publication_pubowner - 1] = ObjectIdGetDatum(GetUserId());

	parse_publication_options(stmt->options,
							  &publish_given, &publish_insert,
							  &publish_update, &publish_delete,
							  &publish_truncate);

	puboid = GetNewOidWithIndex(rel, PublicationObjectIndexId,
								Anum_pg_publication_oid);
	values[Anum_pg_publication_oid - 1] = ObjectIdGetDatum(puboid);
	values[Anum_pg_publication_puballtables - 1] =
		BoolGetDatum(stmt->for_all_tables);
	values[Anum_pg_publication_pubinsert - 1] =
		BoolGetDatum(publish_insert);
	values[Anum_pg_publication_pubupdate - 1] =
		BoolGetDatum(publish_update);
	values[Anum_pg_publication_pubdelete - 1] =
		BoolGetDatum(publish_delete);
	values[Anum_pg_publication_pubtruncate - 1] =
		BoolGetDatum(publish_truncate);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

	/* Insert tuple into catalog. */
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	recordDependencyOnOwner(PublicationRelationId, puboid, GetUserId());

	ObjectAddressSet(myself, PublicationRelationId, puboid);

	/* Make the changes visible. */
	CommandCounterIncrement();

	if (stmt->tables)
	{
		List	   *rels;

		Assert(list_length(stmt->tables) > 0);

		rels = OpenTableList(stmt->tables);
		PublicationAddTables(puboid, rels, true, NULL);
		CloseTableList(rels);
	}
	else if (stmt->for_all_tables)
	{
		/* Invalidate relcache so that publication info is rebuilt. */
		CacheInvalidateRelcacheAll();
	}

	table_close(rel, RowExclusiveLock);

	InvokeObjectPostCreateHook(PublicationRelationId, puboid, 0);

	return myself;
}

/*
 * Change options of a publication.
 */
static void
AlterPublicationOptions(AlterPublicationStmt *stmt, Relation rel,
						HeapTuple tup)
{
	bool		nulls[Natts_pg_publication];
	bool		replaces[Natts_pg_publication];
	Datum		values[Natts_pg_publication];
	bool		publish_given;
	bool		publish_insert;
	bool		publish_update;
	bool		publish_delete;
	bool		publish_truncate;
	ObjectAddress obj;
	Form_pg_publication pubform;

	parse_publication_options(stmt->options,
							  &publish_given, &publish_insert,
							  &publish_update, &publish_delete,
							  &publish_truncate);

	/* Everything ok, form a new tuple. */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	if (publish_given)
	{
		values[Anum_pg_publication_pubinsert - 1] = BoolGetDatum(publish_insert);
		replaces[Anum_pg_publication_pubinsert - 1] = true;

		values[Anum_pg_publication_pubupdate - 1] = BoolGetDatum(publish_update);
		replaces[Anum_pg_publication_pubupdate - 1] = true;

		values[Anum_pg_publication_pubdelete - 1] = BoolGetDatum(publish_delete);
		replaces[Anum_pg_publication_pubdelete - 1] = true;

		values[Anum_pg_publication_pubtruncate - 1] = BoolGetDatum(publish_truncate);
		replaces[Anum_pg_publication_pubtruncate - 1] = true;
	}

	tup = heap_modify_tuple(tup, RelationGetDescr(rel), values, nulls,
							replaces);

	/* Update the catalog. */
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	CommandCounterIncrement();

	pubform = (Form_pg_publication) GETSTRUCT(tup);

	/* Invalidate the relcache. */
	if (pubform->puballtables)
	{
		CacheInvalidateRelcacheAll();
	}
	else
	{
		List	   *relids = GetPublicationRelations(pubform->oid);

		/*
		 * We don't want to send too many individual messages, at some point
		 * it's cheaper to just reset whole relcache.
		 */
		if (list_length(relids) < MAX_RELCACHE_INVAL_MSGS)
		{
			ListCell   *lc;

			foreach(lc, relids)
			{
				Oid			relid = lfirst_oid(lc);

				CacheInvalidateRelcacheByRelid(relid);
			}
		}
		else
			CacheInvalidateRelcacheAll();
	}

	ObjectAddressSet(obj, PublicationRelationId, pubform->oid);
	EventTriggerCollectSimpleCommand(obj, InvalidObjectAddress,
									 (Node *) stmt);

	InvokeObjectPostAlterHook(PublicationRelationId, pubform->oid, 0);
}

/*
 * Add or remove table to/from publication.
 */
static void
AlterPublicationTables(AlterPublicationStmt *stmt, Relation rel,
					   HeapTuple tup)
{
	List	   *rels = NIL;
	Form_pg_publication pubform = (Form_pg_publication) GETSTRUCT(tup);
	Oid			pubid = pubform->oid;

	/* Check that user is allowed to manipulate the publication tables. */
	if (pubform->puballtables)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("publication \"%s\" is defined as FOR ALL TABLES",
						NameStr(pubform->pubname)),
				 errdetail("Tables cannot be added to or dropped from FOR ALL TABLES publications.")));

	Assert(list_length(stmt->tables) > 0);

	rels = OpenTableList(stmt->tables);

	if (stmt->tableAction == DEFELEM_ADD)
		PublicationAddTables(pubid, rels, false, stmt);
	else if (stmt->tableAction == DEFELEM_DROP)
		PublicationDropTables(pubid, rels, false);
	else						/* DEFELEM_SET */
	{
		List	   *oldrelids = GetPublicationRelations(pubid);
		List	   *delrels = NIL;
		ListCell   *oldlc;

		/* Calculate which relations to drop. */
		foreach(oldlc, oldrelids)
		{
			Oid			oldrelid = lfirst_oid(oldlc);
			ListCell   *newlc;
			bool		found = false;

			foreach(newlc, rels)
			{
				Relation	newrel = (Relation) lfirst(newlc);

				if (RelationGetRelid(newrel) == oldrelid)
				{
					found = true;
					break;
				}
			}

			if (!found)
			{
				Relation	oldrel = table_open(oldrelid,
												ShareUpdateExclusiveLock);

				delrels = lappend(delrels, oldrel);
			}
		}

		/* And drop them. */
		PublicationDropTables(pubid, delrels, true);

		/*
		 * Don't bother calculating the difference for adding, we'll catch and
		 * skip existing ones when doing catalog update.
		 */
		PublicationAddTables(pubid, rels, true, stmt);

		CloseTableList(delrels);
	}

	CloseTableList(rels);
}

/*
 * Alter the existing publication.
 *
 * This is dispatcher function for AlterPublicationOptions and
 * AlterPublicationTables.
 */
void
AlterPublication(AlterPublicationStmt *stmt)
{
	Relation	rel;
	HeapTuple	tup;
	Form_pg_publication pubform;

	rel = table_open(PublicationRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(PUBLICATIONNAME,
							  CStringGetDatum(stmt->pubname));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("publication \"%s\" does not exist",
						stmt->pubname)));

	pubform = (Form_pg_publication) GETSTRUCT(tup);

	/* must be owner */
	if (!pg_publication_ownercheck(pubform->oid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_PUBLICATION,
					   stmt->pubname);

	if (stmt->options)
		AlterPublicationOptions(stmt, rel, tup);
	else
		AlterPublicationTables(stmt, rel, tup);

	/* Cleanup. */
	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);
}

/*
 * Remove the publication by mapping OID.
 */
void
RemovePublicationById(Oid pubid)
{
	Relation	rel;
	HeapTuple	tup;
	Form_pg_publication pubform;

	rel = table_open(PublicationRelationId, RowExclusiveLock);

	tup = SearchSysCache1(PUBLICATIONOID, ObjectIdGetDatum(pubid));

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for publication %u", pubid);

	pubform = (Form_pg_publication) GETSTRUCT(tup);

	/* Invalidate relcache so that publication info is rebuilt. */
	if (pubform->puballtables)
		CacheInvalidateRelcacheAll();

	CatalogTupleDelete(rel, &tup->t_self);

	ReleaseSysCache(tup);

	table_close(rel, RowExclusiveLock);
}

/*
 * Remove relation from publication by mapping OID.
 */
void
RemovePublicationRelById(Oid proid)
{
	Relation	rel;
	HeapTuple	tup;
	Form_pg_publication_rel pubrel;

	rel = table_open(PublicationRelRelationId, RowExclusiveLock);

	tup = SearchSysCache1(PUBLICATIONREL, ObjectIdGetDatum(proid));

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for publication table %u",
			 proid);

	pubrel = (Form_pg_publication_rel) GETSTRUCT(tup);

	/* Invalidate relcache so that publication info is rebuilt. */
	CacheInvalidateRelcacheByRelid(pubrel->prrelid);

	CatalogTupleDelete(rel, &tup->t_self);

	ReleaseSysCache(tup);

	table_close(rel, RowExclusiveLock);
}

/*
 * Open relations specified by a RangeVar list.
 * The returned tables are locked in ShareUpdateExclusiveLock mode.
 */
static List *
OpenTableList(List *tables)
{
	List	   *relids = NIL;
	List	   *rels = NIL;
	ListCell   *lc;

	/*
	 * Open, share-lock, and check all the explicitly-specified relations
	 */
	foreach(lc, tables)
	{
		RangeVar   *rv = castNode(RangeVar, lfirst(lc));
		bool		recurse = rv->inh;
		Relation	rel;
		Oid			myrelid;

		/* Allow query cancel in case this takes a long time */
		CHECK_FOR_INTERRUPTS();

		rel = table_openrv(rv, ShareUpdateExclusiveLock);
		myrelid = RelationGetRelid(rel);

		/*
		 * Filter out duplicates if user specifies "foo, foo".
		 *
		 * Note that this algorithm is known to not be very efficient (O(N^2))
		 * but given that it only works on list of tables given to us by user
		 * it's deemed acceptable.
		 */
		if (list_member_oid(relids, myrelid))
		{
			table_close(rel, ShareUpdateExclusiveLock);
			continue;
		}

		rels = lappend(rels, rel);
		relids = lappend_oid(relids, myrelid);

		/* Add children of this rel, if requested */
		if (recurse)
		{
			List	   *children;
			ListCell   *child;

			children = find_all_inheritors(myrelid, ShareUpdateExclusiveLock,
										   NULL);

			foreach(child, children)
			{
				Oid			childrelid = lfirst_oid(child);

				/* Allow query cancel in case this takes a long time */
				CHECK_FOR_INTERRUPTS();

				/*
				 * Skip duplicates if user specified both parent and child
				 * tables.
				 */
				if (list_member_oid(relids, childrelid))
					continue;

				/* find_all_inheritors already got lock */
				rel = table_open(childrelid, NoLock);
				rels = lappend(rels, rel);
				relids = lappend_oid(relids, childrelid);
			}
		}
	}

	list_free(relids);

	return rels;
}

/*
 * Close all relations in the list.
 */
static void
CloseTableList(List *rels)
{
	ListCell   *lc;

	foreach(lc, rels)
	{
		Relation	rel = (Relation) lfirst(lc);

		table_close(rel, NoLock);
	}
}

/*
 * Add listed tables to the publication.
 */
static void
PublicationAddTables(Oid pubid, List *rels, bool if_not_exists,
					 AlterPublicationStmt *stmt)
{
	ListCell   *lc;

	Assert(!stmt || !stmt->for_all_tables);

	foreach(lc, rels)
	{
		Relation	rel = (Relation) lfirst(lc);
		ObjectAddress obj;

		/* Must be owner of the table or superuser. */
		if (!pg_class_ownercheck(RelationGetRelid(rel), GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(rel->rd_rel->relkind),
						   RelationGetRelationName(rel));

		obj = publication_add_relation(pubid, rel, if_not_exists);
		if (stmt)
		{
			EventTriggerCollectSimpleCommand(obj, InvalidObjectAddress,
											 (Node *) stmt);

			InvokeObjectPostCreateHook(PublicationRelRelationId,
									   obj.objectId, 0);
		}
	}
}

/*
 * Remove listed tables from the publication.
 */
static void
PublicationDropTables(Oid pubid, List *rels, bool missing_ok)
{
	ObjectAddress obj;
	ListCell   *lc;
	Oid			prid;

	foreach(lc, rels)
	{
		Relation	rel = (Relation) lfirst(lc);
		Oid			relid = RelationGetRelid(rel);

		prid = GetSysCacheOid2(PUBLICATIONRELMAP, Anum_pg_publication_rel_oid,
							   ObjectIdGetDatum(relid),
							   ObjectIdGetDatum(pubid));
		if (!OidIsValid(prid))
		{
			if (missing_ok)
				continue;

			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("relation \"%s\" is not part of the publication",
							RelationGetRelationName(rel))));
		}

		ObjectAddressSet(obj, PublicationRelRelationId, prid);
		performDeletion(&obj, DROP_CASCADE, 0);
	}
}

/*
 * Internal workhorse for changing a publication owner
 */
static void
AlterPublicationOwner_internal(Relation rel, HeapTuple tup, Oid newOwnerId)
{
	Form_pg_publication form;

	form = (Form_pg_publication) GETSTRUCT(tup);

	if (form->pubowner == newOwnerId)
		return;

	if (!superuser())
	{
		AclResult	aclresult;

		/* Must be owner */
		if (!pg_publication_ownercheck(form->oid, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_PUBLICATION,
						   NameStr(form->pubname));

		/* Must be able to become new owner */
		check_is_member_of_role(GetUserId(), newOwnerId);

		/* New owner must have CREATE privilege on database */
		aclresult = pg_database_aclcheck(MyDatabaseId, newOwnerId, ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_DATABASE,
						   get_database_name(MyDatabaseId));

		if (form->puballtables && !superuser_arg(newOwnerId))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to change owner of publication \"%s\"",
							NameStr(form->pubname)),
					 errhint("The owner of a FOR ALL TABLES publication must be a superuser.")));
	}

	form->pubowner = newOwnerId;
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	/* Update owner dependency reference */
	changeDependencyOnOwner(PublicationRelationId,
							form->oid,
							newOwnerId);

	InvokeObjectPostAlterHook(PublicationRelationId,
							  form->oid, 0);
}

/*
 * Change publication owner -- by name
 */
ObjectAddress
AlterPublicationOwner(const char *name, Oid newOwnerId)
{
	Oid			subid;
	HeapTuple	tup;
	Relation	rel;
	ObjectAddress address;
	Form_pg_publication pubform;

	rel = table_open(PublicationRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(PUBLICATIONNAME, CStringGetDatum(name));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("publication \"%s\" does not exist", name)));

	pubform = (Form_pg_publication) GETSTRUCT(tup);
	subid = pubform->oid;

	AlterPublicationOwner_internal(rel, tup, newOwnerId);

	ObjectAddressSet(address, PublicationRelationId, subid);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);

	return address;
}

/*
 * Change publication owner -- by OID
 */
void
AlterPublicationOwner_oid(Oid subid, Oid newOwnerId)
{
	HeapTuple	tup;
	Relation	rel;

	rel = table_open(PublicationRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(PUBLICATIONOID, ObjectIdGetDatum(subid));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("publication with OID %u does not exist", subid)));

	AlterPublicationOwner_internal(rel, tup, newOwnerId);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);
}
