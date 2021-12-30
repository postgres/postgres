/*-------------------------------------------------------------------------
 *
 * publicationcmds.c
 *		publication manipulation
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		publicationcmds.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/objectaddress.h"
#include "catalog/partition.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_publication.h"
#include "catalog/pg_publication_namespace.h"
#include "catalog/pg_publication_rel.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/publicationcmds.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/varlena.h"

static List *OpenRelIdList(List *relids);
static List *OpenTableList(List *tables);
static void CloseTableList(List *rels);
static void LockSchemaList(List *schemalist);
static void PublicationAddTables(Oid pubid, List *rels, bool if_not_exists,
								 AlterPublicationStmt *stmt);
static void PublicationDropTables(Oid pubid, List *rels, bool missing_ok);
static void PublicationAddSchemas(Oid pubid, List *schemas, bool if_not_exists,
								  AlterPublicationStmt *stmt);
static void PublicationDropSchemas(Oid pubid, List *schemas, bool missing_ok);

static void
parse_publication_options(ParseState *pstate,
						  List *options,
						  bool *publish_given,
						  PublicationActions *pubactions,
						  bool *publish_via_partition_root_given,
						  bool *publish_via_partition_root)
{
	ListCell   *lc;

	*publish_given = false;
	*publish_via_partition_root_given = false;

	/* defaults */
	pubactions->pubinsert = true;
	pubactions->pubupdate = true;
	pubactions->pubdelete = true;
	pubactions->pubtruncate = true;
	*publish_via_partition_root = false;

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
				errorConflictingDefElem(defel, pstate);

			/*
			 * If publish option was given only the explicitly listed actions
			 * should be published.
			 */
			pubactions->pubinsert = false;
			pubactions->pubupdate = false;
			pubactions->pubdelete = false;
			pubactions->pubtruncate = false;

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
					pubactions->pubinsert = true;
				else if (strcmp(publish_opt, "update") == 0)
					pubactions->pubupdate = true;
				else if (strcmp(publish_opt, "delete") == 0)
					pubactions->pubdelete = true;
				else if (strcmp(publish_opt, "truncate") == 0)
					pubactions->pubtruncate = true;
				else
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("unrecognized \"publish\" value: \"%s\"", publish_opt)));
			}
		}
		else if (strcmp(defel->defname, "publish_via_partition_root") == 0)
		{
			if (*publish_via_partition_root_given)
				errorConflictingDefElem(defel, pstate);
			*publish_via_partition_root_given = true;
			*publish_via_partition_root = defGetBoolean(defel);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized publication parameter: \"%s\"", defel->defname)));
	}
}

/*
 * Convert the PublicationObjSpecType list into schema oid list and
 * PublicationTable list.
 */
static void
ObjectsInPublicationToOids(List *pubobjspec_list, ParseState *pstate,
						   List **rels, List **schemas)
{
	ListCell   *cell;
	PublicationObjSpec *pubobj;

	if (!pubobjspec_list)
		return;

	foreach(cell, pubobjspec_list)
	{
		Oid			schemaid;
		List	   *search_path;

		pubobj = (PublicationObjSpec *) lfirst(cell);

		switch (pubobj->pubobjtype)
		{
			case PUBLICATIONOBJ_TABLE:
				*rels = lappend(*rels, pubobj->pubtable);
				break;
			case PUBLICATIONOBJ_TABLES_IN_SCHEMA:
				schemaid = get_namespace_oid(pubobj->name, false);

				/* Filter out duplicates if user specifies "sch1, sch1" */
				*schemas = list_append_unique_oid(*schemas, schemaid);
				break;
			case PUBLICATIONOBJ_TABLES_IN_CUR_SCHEMA:
				search_path = fetch_search_path(false);
				if (search_path == NIL) /* nothing valid in search_path? */
					ereport(ERROR,
							errcode(ERRCODE_UNDEFINED_SCHEMA),
							errmsg("no schema has been selected for CURRENT_SCHEMA"));

				schemaid = linitial_oid(search_path);
				list_free(search_path);

				/* Filter out duplicates if user specifies "sch1, sch1" */
				*schemas = list_append_unique_oid(*schemas, schemaid);
				break;
			default:
				/* shouldn't happen */
				elog(ERROR, "invalid publication object type %d", pubobj->pubobjtype);
				break;
		}
	}
}

/*
 * Check if any of the given relation's schema is a member of the given schema
 * list.
 */
static void
CheckObjSchemaNotAlreadyInPublication(List *rels, List *schemaidlist,
									  PublicationObjSpecType checkobjtype)
{
	ListCell   *lc;

	foreach(lc, rels)
	{
		PublicationRelInfo *pub_rel = (PublicationRelInfo *) lfirst(lc);
		Relation	rel = pub_rel->relation;
		Oid			relSchemaId = RelationGetNamespace(rel);

		if (list_member_oid(schemaidlist, relSchemaId))
		{
			if (checkobjtype == PUBLICATIONOBJ_TABLES_IN_SCHEMA)
				ereport(ERROR,
						errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("cannot add schema \"%s\" to publication",
							   get_namespace_name(relSchemaId)),
						errdetail("Table \"%s\" in schema \"%s\" is already part of the publication, adding the same schema is not supported.",
								  RelationGetRelationName(rel),
								  get_namespace_name(relSchemaId)));
			else if (checkobjtype == PUBLICATIONOBJ_TABLE)
				ereport(ERROR,
						errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("cannot add relation \"%s.%s\" to publication",
							   get_namespace_name(relSchemaId),
							   RelationGetRelationName(rel)),
						errdetail("Table's schema \"%s\" is already part of the publication or part of the specified schema list.",
								  get_namespace_name(relSchemaId)));
		}
	}
}

/*
 * Create new publication.
 */
ObjectAddress
CreatePublication(ParseState *pstate, CreatePublicationStmt *stmt)
{
	Relation	rel;
	ObjectAddress myself;
	Oid			puboid;
	bool		nulls[Natts_pg_publication];
	Datum		values[Natts_pg_publication];
	HeapTuple	tup;
	bool		publish_given;
	PublicationActions pubactions;
	bool		publish_via_partition_root_given;
	bool		publish_via_partition_root;
	AclResult	aclresult;
	List	   *relations = NIL;
	List	   *schemaidlist = NIL;

	/* must have CREATE privilege on database */
	aclresult = pg_database_aclcheck(MyDatabaseId, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_DATABASE,
					   get_database_name(MyDatabaseId));

	/* FOR ALL TABLES requires superuser */
	if (stmt->for_all_tables && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create FOR ALL TABLES publication")));

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

	parse_publication_options(pstate,
							  stmt->options,
							  &publish_given, &pubactions,
							  &publish_via_partition_root_given,
							  &publish_via_partition_root);

	puboid = GetNewOidWithIndex(rel, PublicationObjectIndexId,
								Anum_pg_publication_oid);
	values[Anum_pg_publication_oid - 1] = ObjectIdGetDatum(puboid);
	values[Anum_pg_publication_puballtables - 1] =
		BoolGetDatum(stmt->for_all_tables);
	values[Anum_pg_publication_pubinsert - 1] =
		BoolGetDatum(pubactions.pubinsert);
	values[Anum_pg_publication_pubupdate - 1] =
		BoolGetDatum(pubactions.pubupdate);
	values[Anum_pg_publication_pubdelete - 1] =
		BoolGetDatum(pubactions.pubdelete);
	values[Anum_pg_publication_pubtruncate - 1] =
		BoolGetDatum(pubactions.pubtruncate);
	values[Anum_pg_publication_pubviaroot - 1] =
		BoolGetDatum(publish_via_partition_root);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

	/* Insert tuple into catalog. */
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	recordDependencyOnOwner(PublicationRelationId, puboid, GetUserId());

	ObjectAddressSet(myself, PublicationRelationId, puboid);

	/* Make the changes visible. */
	CommandCounterIncrement();

	/* Associate objects with the publication. */
	if (stmt->for_all_tables)
	{
		/* Invalidate relcache so that publication info is rebuilt. */
		CacheInvalidateRelcacheAll();
	}
	else
	{
		ObjectsInPublicationToOids(stmt->pubobjects, pstate, &relations,
								   &schemaidlist);

		/* FOR ALL TABLES IN SCHEMA requires superuser */
		if (list_length(schemaidlist) > 0 && !superuser())
			ereport(ERROR,
					errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					errmsg("must be superuser to create FOR ALL TABLES IN SCHEMA publication"));

		if (list_length(relations) > 0)
		{
			List	   *rels;

			rels = OpenTableList(relations);
			CheckObjSchemaNotAlreadyInPublication(rels, schemaidlist,
												  PUBLICATIONOBJ_TABLE);
			PublicationAddTables(puboid, rels, true, NULL);
			CloseTableList(rels);
		}

		if (list_length(schemaidlist) > 0)
		{
			/*
			 * Schema lock is held until the publication is created to prevent
			 * concurrent schema deletion.
			 */
			LockSchemaList(schemaidlist);
			PublicationAddSchemas(puboid, schemaidlist, true, NULL);
		}
	}

	table_close(rel, RowExclusiveLock);

	InvokeObjectPostCreateHook(PublicationRelationId, puboid, 0);

	if (wal_level != WAL_LEVEL_LOGICAL)
	{
		ereport(WARNING,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("wal_level is insufficient to publish logical changes"),
				 errhint("Set wal_level to logical before creating subscriptions.")));
	}

	return myself;
}

/*
 * Change options of a publication.
 */
static void
AlterPublicationOptions(ParseState *pstate, AlterPublicationStmt *stmt,
						Relation rel, HeapTuple tup)
{
	bool		nulls[Natts_pg_publication];
	bool		replaces[Natts_pg_publication];
	Datum		values[Natts_pg_publication];
	bool		publish_given;
	PublicationActions pubactions;
	bool		publish_via_partition_root_given;
	bool		publish_via_partition_root;
	ObjectAddress obj;
	Form_pg_publication pubform;

	parse_publication_options(pstate,
							  stmt->options,
							  &publish_given, &pubactions,
							  &publish_via_partition_root_given,
							  &publish_via_partition_root);

	/* Everything ok, form a new tuple. */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	if (publish_given)
	{
		values[Anum_pg_publication_pubinsert - 1] = BoolGetDatum(pubactions.pubinsert);
		replaces[Anum_pg_publication_pubinsert - 1] = true;

		values[Anum_pg_publication_pubupdate - 1] = BoolGetDatum(pubactions.pubupdate);
		replaces[Anum_pg_publication_pubupdate - 1] = true;

		values[Anum_pg_publication_pubdelete - 1] = BoolGetDatum(pubactions.pubdelete);
		replaces[Anum_pg_publication_pubdelete - 1] = true;

		values[Anum_pg_publication_pubtruncate - 1] = BoolGetDatum(pubactions.pubtruncate);
		replaces[Anum_pg_publication_pubtruncate - 1] = true;
	}

	if (publish_via_partition_root_given)
	{
		values[Anum_pg_publication_pubviaroot - 1] = BoolGetDatum(publish_via_partition_root);
		replaces[Anum_pg_publication_pubviaroot - 1] = true;
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
		List	   *relids = NIL;
		List	   *schemarelids = NIL;

		/*
		 * For any partitioned tables contained in the publication, we must
		 * invalidate all partitions contained in the respective partition
		 * trees, not just those explicitly mentioned in the publication.
		 */
		relids = GetPublicationRelations(pubform->oid,
										 PUBLICATION_PART_ALL);
		schemarelids = GetAllSchemaPublicationRelations(pubform->oid,
														PUBLICATION_PART_ALL);
		relids = list_concat_unique_oid(relids, schemarelids);

		InvalidatePublicationRels(relids);
	}

	ObjectAddressSet(obj, PublicationRelationId, pubform->oid);
	EventTriggerCollectSimpleCommand(obj, InvalidObjectAddress,
									 (Node *) stmt);

	InvokeObjectPostAlterHook(PublicationRelationId, pubform->oid, 0);
}

/*
 * Invalidate the relations.
 */
void
InvalidatePublicationRels(List *relids)
{
	/*
	 * We don't want to send too many individual messages, at some point it's
	 * cheaper to just reset whole relcache.
	 */
	if (list_length(relids) < MAX_RELCACHE_INVAL_MSGS)
	{
		ListCell   *lc;

		foreach(lc, relids)
			CacheInvalidateRelcacheByRelid(lfirst_oid(lc));
	}
	else
		CacheInvalidateRelcacheAll();
}

/*
 * Add or remove table to/from publication.
 */
static void
AlterPublicationTables(AlterPublicationStmt *stmt, HeapTuple tup,
					   List *tables, List *schemaidlist)
{
	List	   *rels = NIL;
	Form_pg_publication pubform = (Form_pg_publication) GETSTRUCT(tup);
	Oid			pubid = pubform->oid;

	/*
	 * Nothing to do if no objects, except in SET: for that it is quite
	 * possible that user has not specified any tables in which case we need
	 * to remove all the existing tables.
	 */
	if (!tables && stmt->action != DEFELEM_SET)
		return;

	rels = OpenTableList(tables);

	if (stmt->action == DEFELEM_ADD)
	{
		List	   *schemas = NIL;

		/*
		 * Check if the relation is member of the existing schema in the
		 * publication or member of the schema list specified.
		 */
		schemas = list_concat_copy(schemaidlist, GetPublicationSchemas(pubid));
		CheckObjSchemaNotAlreadyInPublication(rels, schemas,
											  PUBLICATIONOBJ_TABLE);
		PublicationAddTables(pubid, rels, false, stmt);
	}
	else if (stmt->action == DEFELEM_DROP)
		PublicationDropTables(pubid, rels, false);
	else						/* DEFELEM_SET */
	{
		List	   *oldrelids = GetPublicationRelations(pubid,
														PUBLICATION_PART_ROOT);
		List	   *delrels = NIL;
		ListCell   *oldlc;

		CheckObjSchemaNotAlreadyInPublication(rels, schemaidlist,
											  PUBLICATIONOBJ_TABLE);

		/* Calculate which relations to drop. */
		foreach(oldlc, oldrelids)
		{
			Oid			oldrelid = lfirst_oid(oldlc);
			ListCell   *newlc;
			bool		found = false;

			foreach(newlc, rels)
			{
				PublicationRelInfo *newpubrel;

				newpubrel = (PublicationRelInfo *) lfirst(newlc);
				if (RelationGetRelid(newpubrel->relation) == oldrelid)
				{
					found = true;
					break;
				}
			}
			/* Not yet in the list, open it and add to the list */
			if (!found)
			{
				Relation	oldrel;
				PublicationRelInfo *pubrel;

				/* Wrap relation into PublicationRelInfo */
				oldrel = table_open(oldrelid, ShareUpdateExclusiveLock);

				pubrel = palloc(sizeof(PublicationRelInfo));
				pubrel->relation = oldrel;

				delrels = lappend(delrels, pubrel);
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
 * Alter the publication schemas.
 *
 * Add or remove schemas to/from publication.
 */
static void
AlterPublicationSchemas(AlterPublicationStmt *stmt,
						HeapTuple tup, List *schemaidlist)
{
	Form_pg_publication pubform = (Form_pg_publication) GETSTRUCT(tup);

	/*
	 * Nothing to do if no objects, except in SET: for that it is quite
	 * possible that user has not specified any schemas in which case we need
	 * to remove all the existing schemas.
	 */
	if (!schemaidlist && stmt->action != DEFELEM_SET)
		return;

	/*
	 * Schema lock is held until the publication is altered to prevent
	 * concurrent schema deletion.
	 */
	LockSchemaList(schemaidlist);
	if (stmt->action == DEFELEM_ADD)
	{
		List	   *rels;
		List	   *reloids;

		reloids = GetPublicationRelations(pubform->oid, PUBLICATION_PART_ROOT);
		rels = OpenRelIdList(reloids);

		CheckObjSchemaNotAlreadyInPublication(rels, schemaidlist,
											  PUBLICATIONOBJ_TABLES_IN_SCHEMA);

		CloseTableList(rels);
		PublicationAddSchemas(pubform->oid, schemaidlist, false, stmt);
	}
	else if (stmt->action == DEFELEM_DROP)
		PublicationDropSchemas(pubform->oid, schemaidlist, false);
	else						/* DEFELEM_SET */
	{
		List	   *oldschemaids = GetPublicationSchemas(pubform->oid);
		List	   *delschemas = NIL;

		/* Identify which schemas should be dropped */
		delschemas = list_difference_oid(oldschemaids, schemaidlist);

		/*
		 * Schema lock is held until the publication is altered to prevent
		 * concurrent schema deletion.
		 */
		LockSchemaList(delschemas);

		/* And drop them */
		PublicationDropSchemas(pubform->oid, delschemas, true);

		/*
		 * Don't bother calculating the difference for adding, we'll catch and
		 * skip existing ones when doing catalog update.
		 */
		PublicationAddSchemas(pubform->oid, schemaidlist, true, stmt);
	}
}

/*
 * Check if relations and schemas can be in a given publication and throw
 * appropriate error if not.
 */
static void
CheckAlterPublication(AlterPublicationStmt *stmt, HeapTuple tup,
					  List *tables, List *schemaidlist)
{
	Form_pg_publication pubform = (Form_pg_publication) GETSTRUCT(tup);

	if ((stmt->action == DEFELEM_ADD || stmt->action == DEFELEM_SET) &&
		schemaidlist && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to add or set schemas")));

	/*
	 * Check that user is allowed to manipulate the publication tables in
	 * schema
	 */
	if (schemaidlist && pubform->puballtables)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("publication \"%s\" is defined as FOR ALL TABLES",
						NameStr(pubform->pubname)),
				 errdetail("Tables from schema cannot be added to, dropped from, or set on FOR ALL TABLES publications.")));

	/* Check that user is allowed to manipulate the publication tables. */
	if (tables && pubform->puballtables)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("publication \"%s\" is defined as FOR ALL TABLES",
						NameStr(pubform->pubname)),
				 errdetail("Tables cannot be added to or dropped from FOR ALL TABLES publications.")));
}

/*
 * Alter the existing publication.
 *
 * This is dispatcher function for AlterPublicationOptions,
 * AlterPublicationSchemas and AlterPublicationTables.
 */
void
AlterPublication(ParseState *pstate, AlterPublicationStmt *stmt)
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
		AlterPublicationOptions(pstate, stmt, rel, tup);
	else
	{
		List	   *relations = NIL;
		List	   *schemaidlist = NIL;

		ObjectsInPublicationToOids(stmt->pubobjects, pstate, &relations,
								   &schemaidlist);

		CheckAlterPublication(stmt, tup, relations, schemaidlist);

		/*
		 * Lock the publication so nobody else can do anything with it. This
		 * prevents concurrent alter to add table(s) that were already going
		 * to become part of the publication by adding corresponding schema(s)
		 * via this command and similarly it will prevent the concurrent
		 * addition of schema(s) for which there is any corresponding table
		 * being added by this command.
		 */
		LockDatabaseObject(PublicationRelationId, pubform->oid, 0,
						   AccessExclusiveLock);

		/*
		 * It is possible that by the time we acquire the lock on publication,
		 * concurrent DDL has removed it. We can test this by checking the
		 * existence of publication.
		 */
		if (!SearchSysCacheExists1(PUBLICATIONOID,
								   ObjectIdGetDatum(pubform->oid)))
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_OBJECT),
					errmsg("publication \"%s\" does not exist",
						   stmt->pubname));

		AlterPublicationTables(stmt, tup, relations, schemaidlist);
		AlterPublicationSchemas(stmt, tup, schemaidlist);
	}

	/* Cleanup. */
	heap_freetuple(tup);
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
	List	   *relids = NIL;

	rel = table_open(PublicationRelRelationId, RowExclusiveLock);

	tup = SearchSysCache1(PUBLICATIONREL, ObjectIdGetDatum(proid));

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for publication table %u",
			 proid);

	pubrel = (Form_pg_publication_rel) GETSTRUCT(tup);

	/*
	 * Invalidate relcache so that publication info is rebuilt.
	 *
	 * For the partitioned tables, we must invalidate all partitions contained
	 * in the respective partition hierarchies, not just the one explicitly
	 * mentioned in the publication. This is required because we implicitly
	 * publish the child tables when the parent table is published.
	 */
	relids = GetPubPartitionOptionRelations(relids, PUBLICATION_PART_ALL,
											pubrel->prrelid);

	InvalidatePublicationRels(relids);

	CatalogTupleDelete(rel, &tup->t_self);

	ReleaseSysCache(tup);

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
 * Remove schema from publication by mapping OID.
 */
void
RemovePublicationSchemaById(Oid psoid)
{
	Relation	rel;
	HeapTuple	tup;
	List	   *schemaRels = NIL;
	Form_pg_publication_namespace pubsch;

	rel = table_open(PublicationNamespaceRelationId, RowExclusiveLock);

	tup = SearchSysCache1(PUBLICATIONNAMESPACE, ObjectIdGetDatum(psoid));

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for publication schema %u", psoid);

	pubsch = (Form_pg_publication_namespace) GETSTRUCT(tup);

	/*
	 * Invalidate relcache so that publication info is rebuilt. See
	 * RemovePublicationRelById for why we need to consider all the
	 * partitions.
	 */
	schemaRels = GetSchemaPublicationRelations(pubsch->pnnspid,
											   PUBLICATION_PART_ALL);
	InvalidatePublicationRels(schemaRels);

	CatalogTupleDelete(rel, &tup->t_self);

	ReleaseSysCache(tup);

	table_close(rel, RowExclusiveLock);
}

/*
 * Open relations specified by a relid list.
 * The returned tables are locked in ShareUpdateExclusiveLock mode in order to
 * add them to a publication.
 */
static List *
OpenRelIdList(List *relids)
{
	ListCell   *lc;
	List	   *rels = NIL;

	foreach(lc, relids)
	{
		PublicationRelInfo *pub_rel;
		Oid			relid = lfirst_oid(lc);
		Relation	rel = table_open(relid,
									 ShareUpdateExclusiveLock);

		pub_rel = palloc(sizeof(PublicationRelInfo));
		pub_rel->relation = rel;
		rels = lappend(rels, pub_rel);
	}

	return rels;
}

/*
 * Open relations specified by a PublicationTable list.
 * The returned tables are locked in ShareUpdateExclusiveLock mode in order to
 * add them to a publication.
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
		PublicationTable *t = lfirst_node(PublicationTable, lc);
		bool		recurse = t->relation->inh;
		Relation	rel;
		Oid			myrelid;
		PublicationRelInfo *pub_rel;

		/* Allow query cancel in case this takes a long time */
		CHECK_FOR_INTERRUPTS();

		rel = table_openrv(t->relation, ShareUpdateExclusiveLock);
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

		pub_rel = palloc(sizeof(PublicationRelInfo));
		pub_rel->relation = rel;
		rels = lappend(rels, pub_rel);
		relids = lappend_oid(relids, myrelid);

		/*
		 * Add children of this rel, if requested, so that they too are added
		 * to the publication.  A partitioned table can't have any inheritance
		 * children other than its partitions, which need not be explicitly
		 * added to the publication.
		 */
		if (recurse && rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
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
				pub_rel = palloc(sizeof(PublicationRelInfo));
				pub_rel->relation = rel;
				rels = lappend(rels, pub_rel);
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
		PublicationRelInfo *pub_rel;

		pub_rel = (PublicationRelInfo *) lfirst(lc);
		table_close(pub_rel->relation, NoLock);
	}
}

/*
 * Lock the schemas specified in the schema list in AccessShareLock mode in
 * order to prevent concurrent schema deletion.
 */
static void
LockSchemaList(List *schemalist)
{
	ListCell   *lc;

	foreach(lc, schemalist)
	{
		Oid			schemaid = lfirst_oid(lc);

		/* Allow query cancel in case this takes a long time */
		CHECK_FOR_INTERRUPTS();
		LockDatabaseObject(NamespaceRelationId, schemaid, 0, AccessShareLock);

		/*
		 * It is possible that by the time we acquire the lock on schema,
		 * concurrent DDL has removed it. We can test this by checking the
		 * existence of schema.
		 */
		if (!SearchSysCacheExists1(NAMESPACEOID, ObjectIdGetDatum(schemaid)))
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_SCHEMA),
					errmsg("schema with OID %u does not exist", schemaid));
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
		PublicationRelInfo *pub_rel = (PublicationRelInfo *) lfirst(lc);
		Relation	rel = pub_rel->relation;
		ObjectAddress obj;

		/* Must be owner of the table or superuser. */
		if (!pg_class_ownercheck(RelationGetRelid(rel), GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(rel->rd_rel->relkind),
						   RelationGetRelationName(rel));

		obj = publication_add_relation(pubid, pub_rel, if_not_exists);
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
		PublicationRelInfo *pubrel = (PublicationRelInfo *) lfirst(lc);
		Relation	rel = pubrel->relation;
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
 * Add listed schemas to the publication.
 */
static void
PublicationAddSchemas(Oid pubid, List *schemas, bool if_not_exists,
					  AlterPublicationStmt *stmt)
{
	ListCell   *lc;

	Assert(!stmt || !stmt->for_all_tables);

	foreach(lc, schemas)
	{
		Oid			schemaid = lfirst_oid(lc);
		ObjectAddress obj;

		obj = publication_add_schema(pubid, schemaid, if_not_exists);
		if (stmt)
		{
			EventTriggerCollectSimpleCommand(obj, InvalidObjectAddress,
											 (Node *) stmt);

			InvokeObjectPostCreateHook(PublicationNamespaceRelationId,
									   obj.objectId, 0);
		}
	}
}

/*
 * Remove listed schemas from the publication.
 */
static void
PublicationDropSchemas(Oid pubid, List *schemas, bool missing_ok)
{
	ObjectAddress obj;
	ListCell   *lc;
	Oid			psid;

	foreach(lc, schemas)
	{
		Oid			schemaid = lfirst_oid(lc);

		psid = GetSysCacheOid2(PUBLICATIONNAMESPACEMAP,
							   Anum_pg_publication_namespace_oid,
							   ObjectIdGetDatum(schemaid),
							   ObjectIdGetDatum(pubid));
		if (!OidIsValid(psid))
		{
			if (missing_ok)
				continue;

			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("tables from schema \"%s\" are not part of the publication",
							get_namespace_name(schemaid))));
		}

		ObjectAddressSet(obj, PublicationNamespaceRelationId, psid);
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

		if (!superuser_arg(newOwnerId) && is_schema_publication(form->oid))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to change owner of publication \"%s\"",
							NameStr(form->pubname)),
					 errhint("The owner of a FOR ALL TABLES IN SCHEMA publication must be a superuser.")));
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
