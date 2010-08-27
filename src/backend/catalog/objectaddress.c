/*-------------------------------------------------------------------------
 *
 * objectaddress.c
 *	  functions for working with ObjectAddresses
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/catalog/objectaddress.c,v 1.1 2010/08/27 11:47:41 rhaas Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/sysattr.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_class.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_database.h"
#include "catalog/pg_language.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_largeobject_metadata.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/proclang.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "nodes/makefuncs.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_type.h"
#include "rewrite/rewriteSupport.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/rel.h"
#include "utils/tqual.h"

static ObjectAddress get_object_address_unqualified(ObjectType objtype,
							   List *qualname);
static Relation get_relation_by_qualified_name(ObjectType objtype,
							   List *objname, LOCKMODE lockmode);
static ObjectAddress get_object_address_relobject(ObjectType objtype,
							 List *objname, Relation *relp);
static ObjectAddress get_object_address_attribute(ObjectType objtype,
							 List *objname, Relation *relp, LOCKMODE lockmode);
static ObjectAddress get_object_address_opcf(ObjectType objtype, List *objname,
						List *objargs);
static bool object_exists(ObjectAddress address);

/*
 * Translate an object name and arguments (as passed by the parser) to an
 * ObjectAddress.
 *
 * The returned object will be locked using the specified lockmode.  If a
 * sub-object is looked up, the parent object will be locked instead.
 *
 * If the object is a relation or a child object of a relation (e.g. an
 * attribute or contraint), the relation is also opened and *relp receives
 * the open relcache entry pointer; otherwise, *relp is set to NULL.  This
 * is a bit grotty but it makes life simpler, since the caller will
 * typically need the relcache entry too.  Caller must close the relcache
 * entry when done with it.  The relation is locked with the specified lockmode
 * if the target object is the relation itself or an attribute, but for other
 * child objects, only AccessShareLock is acquired on the relation.
 *
 * We don't currently provide a function to release the locks acquired here;
 * typically, the lock must be held until commit to guard against a concurrent
 * drop operation.
 */
ObjectAddress
get_object_address(ObjectType objtype, List *objname, List *objargs,
				   Relation *relp, LOCKMODE lockmode)
{
	ObjectAddress	address;
	Relation		relation = NULL;

	/* Some kind of lock must be taken. */
	Assert(lockmode != NoLock);

	switch (objtype)
	{
		case OBJECT_INDEX:
		case OBJECT_SEQUENCE:
		case OBJECT_TABLE:
		case OBJECT_VIEW:
			relation =
				get_relation_by_qualified_name(objtype, objname, lockmode);
			address.classId = RelationRelationId;
			address.objectId = RelationGetRelid(relation);
			address.objectSubId = 0;
			break;
		case OBJECT_COLUMN:
			address =
				get_object_address_attribute(objtype, objname, &relation,
					lockmode);
			break;
		case OBJECT_RULE:
		case OBJECT_TRIGGER:
		case OBJECT_CONSTRAINT:
			address = get_object_address_relobject(objtype, objname, &relation);
			break;
		case OBJECT_DATABASE:
		case OBJECT_TABLESPACE:
		case OBJECT_ROLE:
		case OBJECT_SCHEMA:
		case OBJECT_LANGUAGE:
			address = get_object_address_unqualified(objtype, objname);
			break;
		case OBJECT_TYPE:
			address.classId = TypeRelationId;
			address.objectId =
				typenameTypeId(NULL, makeTypeNameFromNameList(objname), NULL);
			address.objectSubId = 0;
			break;
		case OBJECT_AGGREGATE:
			address.classId = ProcedureRelationId;
			address.objectId = LookupAggNameTypeNames(objname, objargs, false);
			address.objectSubId = 0;
			break;
		case OBJECT_FUNCTION:
			address.classId = ProcedureRelationId;
			address.objectId = LookupFuncNameTypeNames(objname, objargs, false);
			address.objectSubId = 0;
			break;
		case OBJECT_OPERATOR:
			Assert(list_length(objargs) == 2);
			address.classId = OperatorRelationId;
			address.objectId =
				LookupOperNameTypeNames(NULL, objname,
										(TypeName *) linitial(objargs),
										(TypeName *) lsecond(objargs),
										false, -1);
			address.objectSubId = 0;
			break;
		case OBJECT_CONVERSION:
			address.classId = ConversionRelationId;
			address.objectId = get_conversion_oid(objname, false);
			address.objectSubId = 0;
			break;
		case OBJECT_OPCLASS:
		case OBJECT_OPFAMILY:
			address = get_object_address_opcf(objtype, objname, objargs);
			break;
		case OBJECT_LARGEOBJECT:
			Assert(list_length(objname) == 1);
			address.classId = LargeObjectRelationId;
			address.objectId = oidparse(linitial(objname));
			address.objectSubId = 0;
			if (!LargeObjectExists(address.objectId))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("large object %u does not exist",
								address.objectId)));
			break;
		case OBJECT_CAST:
			{
				TypeName *sourcetype = (TypeName *) linitial(objname);
				TypeName *targettype = (TypeName *) linitial(objargs);
				Oid sourcetypeid = typenameTypeId(NULL, sourcetype, NULL);
				Oid targettypeid = typenameTypeId(NULL, targettype, NULL);

				address.classId = CastRelationId;
				address.objectId =
					get_cast_oid(sourcetypeid, targettypeid, false);
				address.objectSubId = 0;
			}
			break;
		case OBJECT_TSPARSER:
			address.classId = TSParserRelationId;
			address.objectId = get_ts_parser_oid(objname, false);
			address.objectSubId = 0;
			break;
		case OBJECT_TSDICTIONARY:
			address.classId = TSDictionaryRelationId;
			address.objectId = get_ts_dict_oid(objname, false);
			address.objectSubId = 0;
			break;
		case OBJECT_TSTEMPLATE:
			address.classId = TSTemplateRelationId;
			address.objectId = get_ts_template_oid(objname, false);
			address.objectSubId = 0;
			break;
		case OBJECT_TSCONFIGURATION:
			address.classId = TSConfigRelationId;
			address.objectId = get_ts_config_oid(objname, false);
			address.objectSubId = 0;
			break;
		default:
			elog(ERROR, "unrecognized objtype: %d", (int) objtype);
	}

	/*
	 * If we're dealing with a relation or attribute, then the relation is
	 * already locked.  If we're dealing with any other type of object, we need
	 * to lock it and then verify that it still exists.
	 */
	if (address.classId != RelationRelationId)
	{
		if (IsSharedRelation(address.classId))
			LockSharedObject(address.classId, address.objectId, 0, lockmode);
		else
			LockDatabaseObject(address.classId, address.objectId, 0, lockmode);
		/* Did it go away while we were waiting for the lock? */
		if (!object_exists(address))
			elog(ERROR, "cache lookup failed for class %u object %u subobj %d",
				 address.classId, address.objectId, address.objectSubId);
	}

	/* Return the object address and the relation. */
	*relp = relation;
	return address;
}

/*
 * Find an ObjectAddress for a type of object that is identified by an
 * unqualified name.
 */
static ObjectAddress
get_object_address_unqualified(ObjectType objtype, List *qualname)
{
	const char *name;
	ObjectAddress address;

	/*
	 * The types of names handled by this function are not permitted to be
	 * schema-qualified or catalog-qualified.
	 */
	if (list_length(qualname) != 1)
	{
		const char *msg;

		switch (objtype)
		{
			case OBJECT_DATABASE:
				msg = gettext_noop("database name cannot be qualified");
				break;
			case OBJECT_TABLESPACE:
				msg = gettext_noop("tablespace name cannot be qualified");
				break;
			case OBJECT_ROLE:
				msg = gettext_noop("role name cannot be qualified");
				break;
			case OBJECT_SCHEMA:
				msg = gettext_noop("schema name cannot be qualified");
				break;
			case OBJECT_LANGUAGE:
				msg = gettext_noop("language name cannot be qualified");
				break;
			default:
				elog(ERROR, "unrecognized objtype: %d", (int) objtype);
				msg = NULL;			/* placate compiler */
		}
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s", _(msg))));
	}

	/* Format is valid, extract the actual name. */
	name = strVal(linitial(qualname));

	/* Translate name to OID. */
	switch (objtype)
	{
		case OBJECT_DATABASE:
			address.classId = DatabaseRelationId;
			address.objectId = get_database_oid(name, false);
			address.objectSubId = 0;
			break;
		case OBJECT_TABLESPACE:
			address.classId = TableSpaceRelationId;
			address.objectId = get_tablespace_oid(name, false);
			address.objectSubId = 0;
			break;
		case OBJECT_ROLE:
			address.classId = AuthIdRelationId;
			address.objectId = get_role_oid(name, false);
			address.objectSubId = 0;
			break;
		case OBJECT_SCHEMA:
			address.classId = NamespaceRelationId;
			address.objectId = get_namespace_oid(name, false);
			address.objectSubId = 0;
			break;
		case OBJECT_LANGUAGE:
			address.classId = LanguageRelationId;
			address.objectId = get_language_oid(name, false);
			address.objectSubId = 0;
			break;
		default:
			elog(ERROR, "unrecognized objtype: %d", (int) objtype);
			/* placate compiler, which doesn't know elog won't return */
			address.classId = InvalidOid;
			address.objectId = InvalidOid;
			address.objectSubId = 0;
	}

	return address;
}

/*
 * Locate a relation by qualified name.
 */
static Relation
get_relation_by_qualified_name(ObjectType objtype, List *objname,
							   LOCKMODE lockmode)
{
	Relation relation;

	relation = relation_openrv(makeRangeVarFromNameList(objname), lockmode);
	switch (objtype)
	{
		case OBJECT_INDEX:
			if (relation->rd_rel->relkind != RELKIND_INDEX)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is not an index",
								RelationGetRelationName(relation))));
			break;
		case OBJECT_SEQUENCE:
			if (relation->rd_rel->relkind != RELKIND_SEQUENCE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is not a sequence",
								RelationGetRelationName(relation))));
			break;
		case OBJECT_TABLE:
			if (relation->rd_rel->relkind != RELKIND_RELATION)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is not a table",
								RelationGetRelationName(relation))));
			break;
		case OBJECT_VIEW:
			if (relation->rd_rel->relkind != RELKIND_VIEW)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("\"%s\" is not a view",
								RelationGetRelationName(relation))));
			break;
		default:
			elog(ERROR, "unrecognized objtype: %d", (int) objtype);
			break;
	}

	return relation;
}

/*
 * Find object address for an object that is attached to a relation.
 *
 * Note that we take only an AccessShareLock on the relation.  We need not
 * pass down the LOCKMODE from get_object_address(), because that is the lock
 * mode for the object itself, not the relation to which it is attached.
 */
static ObjectAddress
get_object_address_relobject(ObjectType objtype, List *objname, Relation *relp)
{
	ObjectAddress address;
	Relation	relation = NULL;
	int			nnames;
	const char *depname;

	/* Extract name of dependent object. */
	depname = strVal(lfirst(list_tail(objname)));

	/* Separate relation name from dependent object name. */
	nnames = list_length(objname);
	if (nnames < 2)
	{
		Oid		reloid;

		/*
		 * For compatibility with very old releases, we sometimes allow users
		 * to attempt to specify a rule without mentioning the relation name.
		 * If there's only rule by that name in the entire database, this will
		 * work.  But objects other than rules don't get this special
		 * treatment.
		 */
		if (objtype != OBJECT_RULE)
			elog(ERROR, "must specify relation and object name");
		address.classId = RewriteRelationId;
		address.objectId = get_rewrite_oid_without_relid(depname, &reloid);
		address.objectSubId = 0;
		relation = heap_open(reloid, AccessShareLock);
	}
	else
	{
		List	   *relname;
		Oid			reloid;

		/* Extract relation name and open relation. */
		relname = list_truncate(list_copy(objname), nnames - 1);
		relation = heap_openrv(makeRangeVarFromNameList(relname),
							   AccessShareLock);
		reloid = RelationGetRelid(relation);

		switch (objtype)
		{
			case OBJECT_RULE:
				address.classId = RewriteRelationId;
				address.objectId = get_rewrite_oid(reloid, depname, false);
				address.objectSubId = 0;
				break;
			case OBJECT_TRIGGER:
				address.classId = TriggerRelationId;
				address.objectId = get_trigger_oid(reloid, depname, false);
				address.objectSubId = 0;
				break;
			case OBJECT_CONSTRAINT:
				address.classId = ConstraintRelationId;
				address.objectId = get_constraint_oid(reloid, depname, false);
				address.objectSubId = 0;
				break;
			default:
				elog(ERROR, "unrecognized objtype: %d", (int) objtype);
				/* placate compiler, which doesn't know elog won't return */
				address.classId = InvalidOid;
				address.objectId = InvalidOid;
				address.objectSubId = 0;
		}
	}

	/* Done. */
	*relp = relation;
	return address;
}

/*
 * Find the ObjectAddress for an attribute.
 */
static ObjectAddress
get_object_address_attribute(ObjectType objtype, List *objname,
							 Relation *relp, LOCKMODE lockmode)
{
	ObjectAddress	address;
	List	   *relname;
	Oid			reloid;
	Relation	relation;
	const char *attname;

	/* Extract relation name and open relation. */
	attname = strVal(lfirst(list_tail(objname)));
	relname = list_truncate(list_copy(objname), list_length(objname) - 1);
	relation = heap_openrv(makeRangeVarFromNameList(relname), lockmode);
	reloid = RelationGetRelid(relation);

	/* Look up attribute and construct return value. */
	address.classId = RelationRelationId;
	address.objectId = reloid;
	address.objectSubId = get_attnum(reloid, attname);
	if (address.objectSubId == InvalidAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
				 attname, RelationGetRelationName(relation))));

	*relp = relation;
	return address;
}

/*
 * Find the ObjectAddress for an opclass or opfamily.
 */
static ObjectAddress
get_object_address_opcf(ObjectType objtype, List *objname, List *objargs)
{
	Oid			amoid;
	ObjectAddress address;

	Assert(list_length(objargs) == 1);
	amoid = get_am_oid(strVal(linitial(objargs)), false);

	switch (objtype)
	{
		case OBJECT_OPCLASS:
			address.classId = OperatorClassRelationId;
			address.objectId = get_opclass_oid(amoid, objname, false);
			address.objectSubId = 0;
			break;
		case OBJECT_OPFAMILY:
			address.classId = OperatorFamilyRelationId;
			address.objectId = get_opfamily_oid(amoid, objname, false);
			address.objectSubId = 0;
			break;
		default:
			elog(ERROR, "unrecognized objtype: %d", (int) objtype);
			/* placate compiler, which doesn't know elog won't return */
			address.classId = InvalidOid;
			address.objectId = InvalidOid;
			address.objectSubId = 0;
	}

	return address;
}

/*
 * Test whether an object exists.
 */
static bool
object_exists(ObjectAddress address)
{
	int			cache = -1;
	Oid			indexoid = InvalidOid;
	Relation	rel;
	ScanKeyData	skey[1];
	SysScanDesc	sd;
	bool		found;

	/* Sub-objects require special treatment. */
	if (address.objectSubId != 0)
	{
		HeapTuple	atttup;

		/* Currently, attributes are the only sub-objects. */
		Assert(address.classId == RelationRelationId);
		atttup = SearchSysCache2(ATTNUM, ObjectIdGetDatum(address.objectId),
								 Int16GetDatum(address.objectSubId));
		if (!HeapTupleIsValid(atttup))
			found = false;
		else
		{
			found = ((Form_pg_attribute) GETSTRUCT(atttup))->attisdropped;
			ReleaseSysCache(atttup);	
		}
		return found;
	}

	/*
	 * For object types that have a relevant syscache, we use it; for
	 * everything else, we'll have to do an index-scan.  This switch
	 * sets either the cache to be used for the syscache lookup, or the
	 * index to be used for the index scan.
	 */
	switch (address.classId)
	{
		case RelationRelationId:
			cache = RELOID;
			break;
		case RewriteRelationId:
			indexoid = RewriteOidIndexId;
			break;
		case TriggerRelationId:
			indexoid = TriggerOidIndexId;
			break;
		case ConstraintRelationId:
			cache = CONSTROID;
			break;
		case DatabaseRelationId:
			cache = DATABASEOID;
			break;
		case TableSpaceRelationId:
			cache = TABLESPACEOID;
			break;
		case AuthIdRelationId:
			cache = AUTHOID;
			break;
		case NamespaceRelationId:
			cache = NAMESPACEOID;
			break;
		case LanguageRelationId:
			cache = LANGOID;
			break;
		case TypeRelationId:
			cache = TYPEOID;
			break;
		case ProcedureRelationId:
			cache = PROCOID;
			break;
		case OperatorRelationId:
			cache = OPEROID;
			break;
		case ConversionRelationId:
			cache = CONVOID;
			break;
		case OperatorClassRelationId:
			cache = CLAOID;
			break;
		case OperatorFamilyRelationId:
			cache = OPFAMILYOID;
			break;
		case LargeObjectRelationId:
			/*
			 * Weird backward compatibility hack: ObjectAddress notation uses
			 * LargeObjectRelationId for large objects, but since PostgreSQL
			 * 9.0, the relevant catalog is actually
			 * LargeObjectMetadataRelationId.
			 */
			address.classId = LargeObjectMetadataRelationId;
			indexoid = LargeObjectMetadataOidIndexId;
			break;
		case CastRelationId:
			indexoid = CastOidIndexId;
			break;
		case TSParserRelationId:
			cache = TSPARSEROID;
			break;
		case TSDictionaryRelationId:
			cache = TSDICTOID;
			break;
		case TSTemplateRelationId:
			cache = TSTEMPLATEOID;
			break;
		case TSConfigRelationId:
			cache = TSCONFIGOID;
			break;
		default:
			elog(ERROR, "unrecognized classid: %u", address.classId);
	}

	/* Found a syscache? */
	if (cache != -1)
		return SearchSysCacheExists1(cache, ObjectIdGetDatum(address.objectId));

	/* No syscache, so examine the table directly. */
	Assert(OidIsValid(indexoid));
	ScanKeyInit(&skey[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(address.objectId));
	rel = heap_open(address.classId, AccessShareLock);
	sd = systable_beginscan(rel, indexoid, true, SnapshotNow, 1, skey);
	found = HeapTupleIsValid(systable_getnext(sd));
	systable_endscan(sd);
	heap_close(rel, AccessShareLock);
	return found;	
}
