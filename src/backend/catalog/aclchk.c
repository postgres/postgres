/*-------------------------------------------------------------------------
 *
 * aclchk.c
 *	  Routines to check access control permissions.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/aclchk.c,v 1.91.2.2 2004/01/14 03:45:02 tgl Exp $
 *
 * NOTES
 *	  See acl.h.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_database.h"
#include "catalog/pg_group.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static void ExecuteGrantStmt_Relation(GrantStmt *stmt);
static void ExecuteGrantStmt_Database(GrantStmt *stmt);
static void ExecuteGrantStmt_Function(GrantStmt *stmt);
static void ExecuteGrantStmt_Language(GrantStmt *stmt);
static void ExecuteGrantStmt_Namespace(GrantStmt *stmt);

static const char *privilege_to_string(AclMode privilege);

static AclResult aclcheck(Acl *acl, AclId userid, AclMode mode);


#ifdef ACLDEBUG
static
dumpacl(Acl *acl)
{
	int			i;
	AclItem    *aip;

	elog(DEBUG2, "acl size = %d, # acls = %d",
		 ACL_SIZE(acl), ACL_NUM(acl));
	aip = ACL_DAT(acl);
	for (i = 0; i < ACL_NUM(acl); ++i)
		elog(DEBUG2, "	acl[%d]: %s", i,
			 DatumGetCString(DirectFunctionCall1(aclitemout,
											 PointerGetDatum(aip + i))));
}
#endif   /* ACLDEBUG */


/*
 * Determine the effective grantor ID for a GRANT or REVOKE operation.
 *
 * Ordinarily this is just the current user, but when a superuser does
 * GRANT or REVOKE, we pretend he is the object owner.  This ensures that
 * all granted privileges appear to flow from the object owner, and there
 * are never multiple "original sources" of a privilege.
 */
static AclId
select_grantor(AclId ownerId)
{
	AclId		grantorId;

	grantorId = GetUserId();

	/* fast path if no difference */
	if (grantorId == ownerId)
		return grantorId;

	if (superuser())
		grantorId = ownerId;

	return grantorId;
}


/*
 * If is_grant is true, adds the given privileges for the list of
 * grantees to the existing old_acl.  If is_grant is false, the
 * privileges for the given grantees are removed from old_acl.
 *
 * NB: the original old_acl is pfree'd.
 */
static Acl *
merge_acl_with_grant(Acl *old_acl, bool is_grant,
					 bool grant_option, DropBehavior behavior,
					 List *grantees, AclMode privileges,
					 AclId grantor_uid, AclId owner_uid)
{
	unsigned	modechg;
	List	   *j;
	Acl		   *new_acl;

	modechg = is_grant ? ACL_MODECHG_ADD : ACL_MODECHG_DEL;

#ifdef ACLDEBUG
	dumpacl(old_acl);
#endif
	new_acl = old_acl;

	foreach(j, grantees)
	{
		PrivGrantee *grantee = (PrivGrantee *) lfirst(j);
		AclItem		aclitem;
		uint32		idtype;
		Acl		   *newer_acl;
		bool		grantee_is_owner = false;

		if (grantee->username)
		{
			aclitem.ai_grantee = get_usesysid(grantee->username);

			idtype = ACL_IDTYPE_UID;

			grantee_is_owner = (aclitem.ai_grantee == owner_uid);
		}
		else if (grantee->groupname)
		{
			aclitem.ai_grantee = get_grosysid(grantee->groupname);

			idtype = ACL_IDTYPE_GID;
		}
		else
		{
			aclitem.ai_grantee = ACL_ID_WORLD;

			idtype = ACL_IDTYPE_WORLD;
		}

		/*
		 * Grant options can only be granted to individual users, not
		 * groups or public.  The reason is that if a user would re-grant
		 * a privilege that he held through a group having a grant option,
		 * and later the user is removed from the group, the situation is
		 * impossible to clean up.
		 */
		if (is_grant && grant_option && idtype != ACL_IDTYPE_UID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_GRANT_OPERATION),
					 errmsg("grant options can only be granted to individual users")));

		if (!is_grant && grant_option && grantee_is_owner)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_GRANT_OPERATION),
					 errmsg("cannot revoke grant options from owner")));

		aclitem.ai_grantor = grantor_uid;

		ACLITEM_SET_PRIVS_IDTYPE(aclitem,
								 (is_grant || !grant_option) ? privileges : ACL_NO_RIGHTS,
								 (grant_option || (!is_grant && !grantee_is_owner)) ? privileges : ACL_NO_RIGHTS,
								 idtype);

		newer_acl = aclinsert3(new_acl, &aclitem, modechg, behavior);

		/* avoid memory leak when there are many grantees */
		pfree(new_acl);
		new_acl = newer_acl;

#ifdef ACLDEBUG
		dumpacl(new_acl);
#endif
	}

	return new_acl;
}


/*
 * Called to execute the utility commands GRANT and REVOKE
 */
void
ExecuteGrantStmt(GrantStmt *stmt)
{
	switch (stmt->objtype)
	{
		case ACL_OBJECT_RELATION:
			ExecuteGrantStmt_Relation(stmt);
			break;
		case ACL_OBJECT_DATABASE:
			ExecuteGrantStmt_Database(stmt);
			break;
		case ACL_OBJECT_FUNCTION:
			ExecuteGrantStmt_Function(stmt);
			break;
		case ACL_OBJECT_LANGUAGE:
			ExecuteGrantStmt_Language(stmt);
			break;
		case ACL_OBJECT_NAMESPACE:
			ExecuteGrantStmt_Namespace(stmt);
			break;
		default:
			elog(ERROR, "unrecognized GrantStmt.objtype: %d",
				 (int) stmt->objtype);
	}
}

static void
ExecuteGrantStmt_Relation(GrantStmt *stmt)
{
	AclMode		privileges;
	List	   *i;

	if (lfirsti(stmt->privileges) == ACL_ALL_RIGHTS)
		privileges = ACL_ALL_RIGHTS_RELATION;
	else
	{
		privileges = ACL_NO_RIGHTS;
		foreach(i, stmt->privileges)
		{
			AclMode		priv = lfirsti(i);

			if (priv & ~((AclMode) ACL_ALL_RIGHTS_RELATION))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_GRANT_OPERATION),
						 errmsg("invalid privilege type %s for table",
								privilege_to_string(priv))));
			privileges |= priv;
		}
	}

	foreach(i, stmt->objects)
	{
		RangeVar   *relvar = (RangeVar *) lfirst(i);
		Oid			relOid;
		Relation	relation;
		HeapTuple	tuple;
		Form_pg_class pg_class_tuple;
		Datum		aclDatum;
		bool		isNull;
		Acl		   *old_acl;
		Acl		   *new_acl;
		AclId		grantorId;
		AclId		ownerId;
		HeapTuple	newtuple;
		Datum		values[Natts_pg_class];
		char		nulls[Natts_pg_class];
		char		replaces[Natts_pg_class];

		/* open pg_class */
		relation = heap_openr(RelationRelationName, RowExclusiveLock);
		relOid = RangeVarGetRelid(relvar, false);
		tuple = SearchSysCache(RELOID,
							   ObjectIdGetDatum(relOid),
							   0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", relOid);
		pg_class_tuple = (Form_pg_class) GETSTRUCT(tuple);

		ownerId = pg_class_tuple->relowner;
		grantorId = select_grantor(ownerId);

		if (stmt->is_grant
			&& !pg_class_ownercheck(relOid, GetUserId())
			&& pg_class_aclcheck(relOid, GetUserId(),
								 ACL_GRANT_OPTION_FOR(privileges)) != ACLCHECK_OK)
			aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_CLASS, relvar->relname);

		/* Not sensible to grant on an index */
		if (pg_class_tuple->relkind == RELKIND_INDEX)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is an index",
							relvar->relname)));

		/*
		 * If there's no ACL, substitute the proper default.
		 */
		aclDatum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_relacl,
								   &isNull);
		if (isNull)
			old_acl = acldefault(ACL_OBJECT_RELATION, ownerId);
		else
			/* get a detoasted copy of the ACL */
			old_acl = DatumGetAclPCopy(aclDatum);

		new_acl = merge_acl_with_grant(old_acl, stmt->is_grant,
									   stmt->grant_option, stmt->behavior,
									   stmt->grantees, privileges,
									   grantorId, ownerId);

		/* finished building new ACL value, now insert it */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, ' ', sizeof(nulls));
		MemSet(replaces, ' ', sizeof(replaces));

		replaces[Anum_pg_class_relacl - 1] = 'r';
		values[Anum_pg_class_relacl - 1] = PointerGetDatum(new_acl);

		newtuple = heap_modifytuple(tuple, relation, values, nulls, replaces);

		ReleaseSysCache(tuple);

		simple_heap_update(relation, &newtuple->t_self, newtuple);

		/* keep the catalog indexes up to date */
		CatalogUpdateIndexes(relation, newtuple);

		pfree(new_acl);

		heap_close(relation, RowExclusiveLock);
	}
}

static void
ExecuteGrantStmt_Database(GrantStmt *stmt)
{
	AclMode		privileges;
	List	   *i;

	if (lfirsti(stmt->privileges) == ACL_ALL_RIGHTS)
		privileges = ACL_ALL_RIGHTS_DATABASE;
	else
	{
		privileges = ACL_NO_RIGHTS;
		foreach(i, stmt->privileges)
		{
			AclMode		priv = lfirsti(i);

			if (priv & ~((AclMode) ACL_ALL_RIGHTS_DATABASE))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_GRANT_OPERATION),
						 errmsg("invalid privilege type %s for database",
								privilege_to_string(priv))));
			privileges |= priv;
		}
	}

	foreach(i, stmt->objects)
	{
		char	   *dbname = strVal(lfirst(i));
		Relation	relation;
		ScanKeyData entry[1];
		HeapScanDesc scan;
		HeapTuple	tuple;
		Form_pg_database pg_database_tuple;
		Datum		aclDatum;
		bool		isNull;
		Acl		   *old_acl;
		Acl		   *new_acl;
		AclId		grantorId;
		AclId		ownerId;
		HeapTuple	newtuple;
		Datum		values[Natts_pg_database];
		char		nulls[Natts_pg_database];
		char		replaces[Natts_pg_database];

		relation = heap_openr(DatabaseRelationName, RowExclusiveLock);
		ScanKeyEntryInitialize(&entry[0], 0,
							   Anum_pg_database_datname, F_NAMEEQ,
							   CStringGetDatum(dbname));
		scan = heap_beginscan(relation, SnapshotNow, 1, entry);
		tuple = heap_getnext(scan, ForwardScanDirection);
		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_DATABASE),
					 errmsg("database \"%s\" does not exist", dbname)));
		pg_database_tuple = (Form_pg_database) GETSTRUCT(tuple);

		ownerId = pg_database_tuple->datdba;
		grantorId = select_grantor(ownerId);

		if (stmt->is_grant
			&& !pg_database_ownercheck(HeapTupleGetOid(tuple), GetUserId())
			&& pg_database_aclcheck(HeapTupleGetOid(tuple), GetUserId(),
									ACL_GRANT_OPTION_FOR(privileges)) != ACLCHECK_OK)
			aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_DATABASE,
						   NameStr(pg_database_tuple->datname));

		/*
		 * If there's no ACL, substitute the proper default.
		 */
		aclDatum = heap_getattr(tuple, Anum_pg_database_datacl,
								RelationGetDescr(relation), &isNull);
		if (isNull)
			old_acl = acldefault(ACL_OBJECT_DATABASE, ownerId);
		else
			/* get a detoasted copy of the ACL */
			old_acl = DatumGetAclPCopy(aclDatum);

		new_acl = merge_acl_with_grant(old_acl, stmt->is_grant,
									   stmt->grant_option, stmt->behavior,
									   stmt->grantees, privileges,
									   grantorId, ownerId);

		/* finished building new ACL value, now insert it */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, ' ', sizeof(nulls));
		MemSet(replaces, ' ', sizeof(replaces));

		replaces[Anum_pg_database_datacl - 1] = 'r';
		values[Anum_pg_database_datacl - 1] = PointerGetDatum(new_acl);

		newtuple = heap_modifytuple(tuple, relation, values, nulls, replaces);

		simple_heap_update(relation, &newtuple->t_self, newtuple);

		/* keep the catalog indexes up to date */
		CatalogUpdateIndexes(relation, newtuple);

		pfree(new_acl);

		heap_endscan(scan);

		heap_close(relation, RowExclusiveLock);
	}
}

static void
ExecuteGrantStmt_Function(GrantStmt *stmt)
{
	AclMode		privileges;
	List	   *i;

	if (lfirsti(stmt->privileges) == ACL_ALL_RIGHTS)
		privileges = ACL_ALL_RIGHTS_FUNCTION;
	else
	{
		privileges = ACL_NO_RIGHTS;
		foreach(i, stmt->privileges)
		{
			AclMode		priv = lfirsti(i);

			if (priv & ~((AclMode) ACL_ALL_RIGHTS_FUNCTION))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_GRANT_OPERATION),
						 errmsg("invalid privilege type %s for function",
								privilege_to_string(priv))));
			privileges |= priv;
		}
	}

	foreach(i, stmt->objects)
	{
		FuncWithArgs *func = (FuncWithArgs *) lfirst(i);
		Oid			oid;
		Relation	relation;
		HeapTuple	tuple;
		Form_pg_proc pg_proc_tuple;
		Datum		aclDatum;
		bool		isNull;
		Acl		   *old_acl;
		Acl		   *new_acl;
		AclId		grantorId;
		AclId		ownerId;
		HeapTuple	newtuple;
		Datum		values[Natts_pg_proc];
		char		nulls[Natts_pg_proc];
		char		replaces[Natts_pg_proc];

		oid = LookupFuncNameTypeNames(func->funcname, func->funcargs, false);

		relation = heap_openr(ProcedureRelationName, RowExclusiveLock);
		tuple = SearchSysCache(PROCOID,
							   ObjectIdGetDatum(oid),
							   0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for function %u", oid);
		pg_proc_tuple = (Form_pg_proc) GETSTRUCT(tuple);

		ownerId = pg_proc_tuple->proowner;
		grantorId = select_grantor(ownerId);

		if (stmt->is_grant
			&& !pg_proc_ownercheck(oid, GetUserId())
			&& pg_proc_aclcheck(oid, GetUserId(),
								ACL_GRANT_OPTION_FOR(privileges)) != ACLCHECK_OK)
			aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_PROC,
						   NameStr(pg_proc_tuple->proname));

		/*
		 * If there's no ACL, substitute the proper default.
		 */
		aclDatum = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_proacl,
								   &isNull);
		if (isNull)
			old_acl = acldefault(ACL_OBJECT_FUNCTION, ownerId);
		else
			/* get a detoasted copy of the ACL */
			old_acl = DatumGetAclPCopy(aclDatum);

		new_acl = merge_acl_with_grant(old_acl, stmt->is_grant,
									   stmt->grant_option, stmt->behavior,
									   stmt->grantees, privileges,
									   grantorId, ownerId);

		/* finished building new ACL value, now insert it */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, ' ', sizeof(nulls));
		MemSet(replaces, ' ', sizeof(replaces));

		replaces[Anum_pg_proc_proacl - 1] = 'r';
		values[Anum_pg_proc_proacl - 1] = PointerGetDatum(new_acl);

		newtuple = heap_modifytuple(tuple, relation, values, nulls, replaces);

		ReleaseSysCache(tuple);

		simple_heap_update(relation, &newtuple->t_self, newtuple);

		/* keep the catalog indexes up to date */
		CatalogUpdateIndexes(relation, newtuple);

		pfree(new_acl);

		heap_close(relation, RowExclusiveLock);
	}
}

static void
ExecuteGrantStmt_Language(GrantStmt *stmt)
{
	AclMode		privileges;
	List	   *i;

	if (lfirsti(stmt->privileges) == ACL_ALL_RIGHTS)
		privileges = ACL_ALL_RIGHTS_LANGUAGE;
	else
	{
		privileges = ACL_NO_RIGHTS;
		foreach(i, stmt->privileges)
		{
			AclMode		priv = lfirsti(i);

			if (priv & ~((AclMode) ACL_ALL_RIGHTS_LANGUAGE))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_GRANT_OPERATION),
						 errmsg("invalid privilege type %s for language",
								privilege_to_string(priv))));
			privileges |= priv;
		}
	}

	foreach(i, stmt->objects)
	{
		char	   *langname = strVal(lfirst(i));
		Relation	relation;
		HeapTuple	tuple;
		Form_pg_language pg_language_tuple;
		Datum		aclDatum;
		bool		isNull;
		Acl		   *old_acl;
		Acl		   *new_acl;
		AclId		grantorId;
		AclId		ownerId;
		HeapTuple	newtuple;
		Datum		values[Natts_pg_language];
		char		nulls[Natts_pg_language];
		char		replaces[Natts_pg_language];

		relation = heap_openr(LanguageRelationName, RowExclusiveLock);
		tuple = SearchSysCache(LANGNAME,
							   PointerGetDatum(langname),
							   0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("language \"%s\" does not exist", langname)));
		pg_language_tuple = (Form_pg_language) GETSTRUCT(tuple);

		/*
		 * Note: for now, languages are treated as owned by the bootstrap
		 * user.  We should add an owner column to pg_language instead.
		 */
		ownerId = BOOTSTRAP_USESYSID;
		grantorId = select_grantor(ownerId);

		if (stmt->is_grant
			&& !superuser()		/* XXX no ownercheck() available */
			&& pg_language_aclcheck(HeapTupleGetOid(tuple), GetUserId(),
									ACL_GRANT_OPTION_FOR(privileges)) != ACLCHECK_OK)
			aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_LANGUAGE,
						   NameStr(pg_language_tuple->lanname));

		if (!pg_language_tuple->lanpltrusted)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("language \"%s\" is not trusted", langname)));

		/*
		 * If there's no ACL, substitute the proper default.
		 */
		aclDatum = SysCacheGetAttr(LANGNAME, tuple, Anum_pg_language_lanacl,
								   &isNull);
		if (isNull)
			old_acl = acldefault(ACL_OBJECT_LANGUAGE, ownerId);
		else
			/* get a detoasted copy of the ACL */
			old_acl = DatumGetAclPCopy(aclDatum);

		new_acl = merge_acl_with_grant(old_acl, stmt->is_grant,
									   stmt->grant_option, stmt->behavior,
									   stmt->grantees, privileges,
									   grantorId, ownerId);

		/* finished building new ACL value, now insert it */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, ' ', sizeof(nulls));
		MemSet(replaces, ' ', sizeof(replaces));

		replaces[Anum_pg_language_lanacl - 1] = 'r';
		values[Anum_pg_language_lanacl - 1] = PointerGetDatum(new_acl);

		newtuple = heap_modifytuple(tuple, relation, values, nulls, replaces);

		ReleaseSysCache(tuple);

		simple_heap_update(relation, &newtuple->t_self, newtuple);

		/* keep the catalog indexes up to date */
		CatalogUpdateIndexes(relation, newtuple);

		pfree(new_acl);

		heap_close(relation, RowExclusiveLock);
	}
}

static void
ExecuteGrantStmt_Namespace(GrantStmt *stmt)
{
	AclMode		privileges;
	List	   *i;

	if (lfirsti(stmt->privileges) == ACL_ALL_RIGHTS)
		privileges = ACL_ALL_RIGHTS_NAMESPACE;
	else
	{
		privileges = ACL_NO_RIGHTS;
		foreach(i, stmt->privileges)
		{
			AclMode		priv = lfirsti(i);

			if (priv & ~((AclMode) ACL_ALL_RIGHTS_NAMESPACE))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_GRANT_OPERATION),
						 errmsg("invalid privilege type %s for schema",
								privilege_to_string(priv))));
			privileges |= priv;
		}
	}

	foreach(i, stmt->objects)
	{
		char	   *nspname = strVal(lfirst(i));
		Relation	relation;
		HeapTuple	tuple;
		Form_pg_namespace pg_namespace_tuple;
		Datum		aclDatum;
		bool		isNull;
		Acl		   *old_acl;
		Acl		   *new_acl;
		AclId		grantorId;
		AclId		ownerId;
		HeapTuple	newtuple;
		Datum		values[Natts_pg_namespace];
		char		nulls[Natts_pg_namespace];
		char		replaces[Natts_pg_namespace];

		relation = heap_openr(NamespaceRelationName, RowExclusiveLock);
		tuple = SearchSysCache(NAMESPACENAME,
							   CStringGetDatum(nspname),
							   0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("schema \"%s\" does not exist", nspname)));
		pg_namespace_tuple = (Form_pg_namespace) GETSTRUCT(tuple);

		ownerId = pg_namespace_tuple->nspowner;
		grantorId = select_grantor(ownerId);

		if (stmt->is_grant
			&& !pg_namespace_ownercheck(HeapTupleGetOid(tuple), GetUserId())
			&& pg_namespace_aclcheck(HeapTupleGetOid(tuple), GetUserId(),
									 ACL_GRANT_OPTION_FOR(privileges)) != ACLCHECK_OK)
			aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_NAMESPACE,
						   nspname);

		/*
		 * If there's no ACL, substitute the proper default.
		 */
		aclDatum = SysCacheGetAttr(NAMESPACENAME, tuple,
								   Anum_pg_namespace_nspacl,
								   &isNull);
		if (isNull)
			old_acl = acldefault(ACL_OBJECT_NAMESPACE, ownerId);
		else
			/* get a detoasted copy of the ACL */
			old_acl = DatumGetAclPCopy(aclDatum);

		new_acl = merge_acl_with_grant(old_acl, stmt->is_grant,
									   stmt->grant_option, stmt->behavior,
									   stmt->grantees, privileges,
									   grantorId, ownerId);

		/* finished building new ACL value, now insert it */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, ' ', sizeof(nulls));
		MemSet(replaces, ' ', sizeof(replaces));

		replaces[Anum_pg_namespace_nspacl - 1] = 'r';
		values[Anum_pg_namespace_nspacl - 1] = PointerGetDatum(new_acl);

		newtuple = heap_modifytuple(tuple, relation, values, nulls, replaces);

		ReleaseSysCache(tuple);

		simple_heap_update(relation, &newtuple->t_self, newtuple);

		/* keep the catalog indexes up to date */
		CatalogUpdateIndexes(relation, newtuple);

		pfree(new_acl);

		heap_close(relation, RowExclusiveLock);
	}
}


static const char *
privilege_to_string(AclMode privilege)
{
	switch (privilege)
	{
		case ACL_INSERT:
			return "INSERT";
		case ACL_SELECT:
			return "SELECT";
		case ACL_UPDATE:
			return "UPDATE";
		case ACL_DELETE:
			return "DELETE";
		case ACL_RULE:
			return "RULE";
		case ACL_REFERENCES:
			return "REFERENCES";
		case ACL_TRIGGER:
			return "TRIGGER";
		case ACL_EXECUTE:
			return "EXECUTE";
		case ACL_USAGE:
			return "USAGE";
		case ACL_CREATE:
			return "CREATE";
		case ACL_CREATE_TEMP:
			return "TEMP";
		default:
			elog(ERROR, "unrecognized privilege: %d", (int) privilege);
	}
	return NULL;				/* appease compiler */
}


AclId
get_grosysid(char *groname)
{
	HeapTuple	tuple;
	AclId		id = 0;

	tuple = SearchSysCache(GRONAME,
						   PointerGetDatum(groname),
						   0, 0, 0);
	if (HeapTupleIsValid(tuple))
	{
		id = ((Form_pg_group) GETSTRUCT(tuple))->grosysid;
		ReleaseSysCache(tuple);
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("group \"%s\" does not exist", groname)));
	return id;
}

/*
 * Convert group ID to name, or return NULL if group can't be found
 */
char *
get_groname(AclId grosysid)
{
	HeapTuple	tuple;
	char	   *name = NULL;

	tuple = SearchSysCache(GROSYSID,
						   ObjectIdGetDatum(grosysid),
						   0, 0, 0);
	if (HeapTupleIsValid(tuple))
	{
		name = pstrdup(NameStr(((Form_pg_group) GETSTRUCT(tuple))->groname));
		ReleaseSysCache(tuple);
	}
	return name;
}

/*
 * Is user a member of group?
 */
static bool
in_group(AclId uid, AclId gid)
{
	bool		result = false;
	HeapTuple	tuple;
	Datum		att;
	bool		isNull;
	IdList	   *glist;
	AclId	   *aidp;
	int			i,
				num;

	tuple = SearchSysCache(GROSYSID,
						   ObjectIdGetDatum(gid),
						   0, 0, 0);
	if (HeapTupleIsValid(tuple))
	{
		att = SysCacheGetAttr(GROSYSID,
							  tuple,
							  Anum_pg_group_grolist,
							  &isNull);
		if (!isNull)
		{
			/* be sure the IdList is not toasted */
			glist = DatumGetIdListP(att);
			/* scan it */
			num = IDLIST_NUM(glist);
			aidp = IDLIST_DAT(glist);
			for (i = 0; i < num; ++i)
			{
				if (aidp[i] == uid)
				{
					result = true;
					break;
				}
			}
			/* if IdList was toasted, free detoasted copy */
			if ((Pointer) glist != DatumGetPointer(att))
				pfree(glist);
		}
		ReleaseSysCache(tuple);
	}
	else
		ereport(WARNING,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("group with ID %u does not exist", gid)));
	return result;
}


/*
 * aclcheck
 *
 * Returns ACLCHECK_OK if the 'userid' has ACL entries in 'acl' to
 * satisfy any one of the requirements of 'mode'.  Returns an
 * appropriate ACLCHECK_* error code otherwise.
 */
static AclResult
aclcheck(Acl *acl, AclId userid, AclMode mode)
{
	AclItem    *aidat;
	int			i,
				num;

	/*
	 * Null ACL should not happen, since caller should have inserted
	 * appropriate default
	 */
	if (acl == NULL)
	{
		elog(ERROR, "null ACL");
		return ACLCHECK_NO_PRIV;
	}

	num = ACL_NUM(acl);
	aidat = ACL_DAT(acl);

	/*
	 * See if privilege is granted directly to user or to public
	 */
	for (i = 0; i < num; i++)
		if (ACLITEM_GET_IDTYPE(aidat[i]) == ACL_IDTYPE_WORLD
			|| (ACLITEM_GET_IDTYPE(aidat[i]) == ACL_IDTYPE_UID
				&& aidat[i].ai_grantee == userid))
		{
			if (aidat[i].ai_privs & mode)
				return ACLCHECK_OK;
		}

	/*
	 * See if he has the permission via any group (do this in a separate
	 * pass to avoid expensive(?) lookups in pg_group)
	 */
	for (i = 0; i < num; i++)
		if (ACLITEM_GET_IDTYPE(aidat[i]) == ACL_IDTYPE_GID
			&& aidat[i].ai_privs & mode
			&& in_group(userid, aidat[i].ai_grantee))
			return ACLCHECK_OK;

	/* If here, doesn't have the privilege. */
	return ACLCHECK_NO_PRIV;
}


/*
 * Standardized reporting of aclcheck permissions failures.
 *
 * Note: we do not double-quote the %s's below, because many callers
 * supply strings that might be already quoted.
 */

static const char *const no_priv_msg[MAX_ACL_KIND] =
{
	/* ACL_KIND_CLASS */
	gettext_noop("permission denied for relation %s"),
	/* ACL_KIND_DATABASE */
	gettext_noop("permission denied for database %s"),
	/* ACL_KIND_PROC */
	gettext_noop("permission denied for function %s"),
	/* ACL_KIND_OPER */
	gettext_noop("permission denied for operator %s"),
	/* ACL_KIND_TYPE */
	gettext_noop("permission denied for type %s"),
	/* ACL_KIND_LANGUAGE */
	gettext_noop("permission denied for language %s"),
	/* ACL_KIND_NAMESPACE */
	gettext_noop("permission denied for schema %s"),
	/* ACL_KIND_OPCLASS */
	gettext_noop("permission denied for operator class %s"),
	/* ACL_KIND_CONVERSION */
	gettext_noop("permission denied for conversion %s")
};

static const char *const not_owner_msg[MAX_ACL_KIND] =
{
	/* ACL_KIND_CLASS */
	gettext_noop("must be owner of relation %s"),
	/* ACL_KIND_DATABASE */
	gettext_noop("must be owner of database %s"),
	/* ACL_KIND_PROC */
	gettext_noop("must be owner of function %s"),
	/* ACL_KIND_OPER */
	gettext_noop("must be owner of operator %s"),
	/* ACL_KIND_TYPE */
	gettext_noop("must be owner of type %s"),
	/* ACL_KIND_LANGUAGE */
	gettext_noop("must be owner of language %s"),
	/* ACL_KIND_NAMESPACE */
	gettext_noop("must be owner of schema %s"),
	/* ACL_KIND_OPCLASS */
	gettext_noop("must be owner of operator class %s"),
	/* ACL_KIND_CONVERSION */
	gettext_noop("must be owner of conversion %s")
};


void
aclcheck_error(AclResult aclerr, AclObjectKind objectkind,
			   const char *objectname)
{
	switch (aclerr)
	{
		case ACLCHECK_OK:
			/* no error, so return to caller */
			break;
		case ACLCHECK_NO_PRIV:
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg(no_priv_msg[objectkind], objectname)));
			break;
		case ACLCHECK_NOT_OWNER:
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg(not_owner_msg[objectkind], objectname)));
			break;
		default:
			elog(ERROR, "unrecognized AclResult: %d", (int) aclerr);
			break;
	}
}


/*
 * Exported routine for checking a user's access privileges to a table
 *
 * Note: we give lookup failure the full ereport treatment because the
 * has_table_privilege() family of functions allow users to pass
 * any random OID to this function.  Likewise for the sibling functions
 * below.
 */
AclResult
pg_class_aclcheck(Oid table_oid, AclId userid, AclMode mode)
{
	AclResult	result;
	bool		usesuper,
				usecatupd;
	HeapTuple	tuple;
	Form_pg_class classForm;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;

	/*
	 * Validate userid, find out if he is superuser, also get usecatupd
	 */
	tuple = SearchSysCache(SHADOWSYSID,
						   ObjectIdGetDatum(userid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("user with ID %u does not exist", userid)));

	usecatupd = ((Form_pg_shadow) GETSTRUCT(tuple))->usecatupd;

	ReleaseSysCache(tuple);

	usesuper = superuser_arg(userid);

	/*
	 * Now get the relation's tuple from pg_class
	 */
	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(table_oid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
			  errmsg("relation with OID %u does not exist", table_oid)));
	classForm = (Form_pg_class) GETSTRUCT(tuple);

	/*
	 * Deny anyone permission to update a system catalog unless
	 * pg_shadow.usecatupd is set.	(This is to let superusers protect
	 * themselves from themselves.)  Also allow it if allowSystemTableMods.
	 *
	 * As of 7.4 we have some updatable system views; those shouldn't
	 * be protected in this way.  Assume the view rules can take care
	 * of themselves.
	 */
	if ((mode & (ACL_INSERT | ACL_UPDATE | ACL_DELETE)) &&
		IsSystemClass(classForm) &&
		classForm->relkind != RELKIND_VIEW &&
		!usecatupd &&
		!allowSystemTableMods)
	{
#ifdef ACLDEBUG
		elog(DEBUG2, "permission denied for system catalog update");
#endif
		ReleaseSysCache(tuple);
		return ACLCHECK_NO_PRIV;
	}

	/*
	 * Otherwise, superusers bypass all permission-checking.
	 */
	if (usesuper)
	{
#ifdef ACLDEBUG
		elog(DEBUG2, "%u is superuser, home free", userid);
#endif
		ReleaseSysCache(tuple);
		return ACLCHECK_OK;
	}

	/*
	 * Normal case: get the relation's ACL from pg_class
	 */
	aclDatum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_relacl,
							   &isNull);
	if (isNull)
	{
		/* No ACL, so build default ACL */
		AclId		ownerId = classForm->relowner;

		acl = acldefault(ACL_OBJECT_RELATION, ownerId);
		aclDatum = (Datum) 0;
	}
	else
	{
		/* detoast rel's ACL if necessary */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclcheck(acl, userid, mode);

	/* if we have a detoasted copy, free it */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	ReleaseSysCache(tuple);

	return result;
}

/*
 * Exported routine for checking a user's access privileges to a database
 */
AclResult
pg_database_aclcheck(Oid db_oid, AclId userid, AclMode mode)
{
	AclResult	result;
	Relation	pg_database;
	ScanKeyData entry[1];
	HeapScanDesc scan;
	HeapTuple	tuple;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(userid))
		return ACLCHECK_OK;

	/*
	 * Get the database's ACL from pg_database
	 *
	 * There's no syscache for pg_database, so must look the hard way
	 */
	pg_database = heap_openr(DatabaseRelationName, AccessShareLock);
	ScanKeyEntryInitialize(&entry[0], 0x0,
						   ObjectIdAttributeNumber, F_OIDEQ,
						   ObjectIdGetDatum(db_oid));
	scan = heap_beginscan(pg_database, SnapshotNow, 1, entry);
	tuple = heap_getnext(scan, ForwardScanDirection);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database with OID %u does not exist", db_oid)));

	aclDatum = heap_getattr(tuple, Anum_pg_database_datacl,
							RelationGetDescr(pg_database), &isNull);

	if (isNull)
	{
		/* No ACL, so build default ACL */
		AclId		ownerId;

		ownerId = ((Form_pg_database) GETSTRUCT(tuple))->datdba;
		acl = acldefault(ACL_OBJECT_DATABASE, ownerId);
		aclDatum = (Datum) 0;
	}
	else
	{
		/* detoast ACL if necessary */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclcheck(acl, userid, mode);

	/* if we have a detoasted copy, free it */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	heap_endscan(scan);
	heap_close(pg_database, AccessShareLock);

	return result;
}

/*
 * Exported routine for checking a user's access privileges to a function
 */
AclResult
pg_proc_aclcheck(Oid proc_oid, AclId userid, AclMode mode)
{
	AclResult	result;
	HeapTuple	tuple;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(userid))
		return ACLCHECK_OK;

	/*
	 * Get the function's ACL from pg_proc
	 */
	tuple = SearchSysCache(PROCOID,
						   ObjectIdGetDatum(proc_oid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
			   errmsg("function with OID %u does not exist", proc_oid)));

	aclDatum = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_proacl,
							   &isNull);
	if (isNull)
	{
		/* No ACL, so build default ACL */
		AclId		ownerId;

		ownerId = ((Form_pg_proc) GETSTRUCT(tuple))->proowner;
		acl = acldefault(ACL_OBJECT_FUNCTION, ownerId);
		aclDatum = (Datum) 0;
	}
	else
	{
		/* detoast ACL if necessary */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclcheck(acl, userid, mode);

	/* if we have a detoasted copy, free it */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	ReleaseSysCache(tuple);

	return result;
}

/*
 * Exported routine for checking a user's access privileges to a language
 */
AclResult
pg_language_aclcheck(Oid lang_oid, AclId userid, AclMode mode)
{
	AclResult	result;
	HeapTuple	tuple;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(userid))
		return ACLCHECK_OK;

	/*
	 * Get the language's ACL from pg_language
	 */
	tuple = SearchSysCache(LANGOID,
						   ObjectIdGetDatum(lang_oid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
			   errmsg("language with OID %u does not exist", lang_oid)));

	aclDatum = SysCacheGetAttr(LANGOID, tuple, Anum_pg_language_lanacl,
							   &isNull);
	if (isNull)
	{
		/* No ACL, so build default ACL */
		/* XXX pg_language should have an owner column, but doesn't */
		acl = acldefault(ACL_OBJECT_LANGUAGE, BOOTSTRAP_USESYSID);
		aclDatum = (Datum) 0;
	}
	else
	{
		/* detoast ACL if necessary */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclcheck(acl, userid, mode);

	/* if we have a detoasted copy, free it */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	ReleaseSysCache(tuple);

	return result;
}

/*
 * Exported routine for checking a user's access privileges to a namespace
 */
AclResult
pg_namespace_aclcheck(Oid nsp_oid, AclId userid, AclMode mode)
{
	AclResult	result;
	HeapTuple	tuple;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;

	/*
	 * If we have been assigned this namespace as a temp namespace, assume
	 * we have all grantable privileges on it.
	 */
	if (isTempNamespace(nsp_oid))
		return ACLCHECK_OK;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(userid))
		return ACLCHECK_OK;

	/*
	 * Get the schema's ACL from pg_namespace
	 */
	tuple = SearchSysCache(NAMESPACEOID,
						   ObjectIdGetDatum(nsp_oid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("schema with OID %u does not exist", nsp_oid)));

	aclDatum = SysCacheGetAttr(NAMESPACEOID, tuple, Anum_pg_namespace_nspacl,
							   &isNull);
	if (isNull)
	{
		/* No ACL, so build default ACL */
		AclId		ownerId;

		ownerId = ((Form_pg_namespace) GETSTRUCT(tuple))->nspowner;
		acl = acldefault(ACL_OBJECT_NAMESPACE, ownerId);
		aclDatum = (Datum) 0;
	}
	else
	{
		/* detoast ACL if necessary */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclcheck(acl, userid, mode);

	/* if we have a detoasted copy, free it */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	ReleaseSysCache(tuple);

	return result;
}


/*
 * Ownership check for a relation (specified by OID).
 */
bool
pg_class_ownercheck(Oid class_oid, AclId userid)
{
	HeapTuple	tuple;
	AclId		owner_id;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(userid))
		return true;

	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(class_oid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
			  errmsg("relation with OID %u does not exist", class_oid)));

	owner_id = ((Form_pg_class) GETSTRUCT(tuple))->relowner;

	ReleaseSysCache(tuple);

	return userid == owner_id;
}

/*
 * Ownership check for a type (specified by OID).
 */
bool
pg_type_ownercheck(Oid type_oid, AclId userid)
{
	HeapTuple	tuple;
	AclId		owner_id;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(userid))
		return true;

	tuple = SearchSysCache(TYPEOID,
						   ObjectIdGetDatum(type_oid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type with OID %u does not exist", type_oid)));

	owner_id = ((Form_pg_type) GETSTRUCT(tuple))->typowner;

	ReleaseSysCache(tuple);

	return userid == owner_id;
}

/*
 * Ownership check for an operator (specified by OID).
 */
bool
pg_oper_ownercheck(Oid oper_oid, AclId userid)
{
	HeapTuple	tuple;
	AclId		owner_id;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(userid))
		return true;

	tuple = SearchSysCache(OPEROID,
						   ObjectIdGetDatum(oper_oid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
			   errmsg("operator with OID %u does not exist", oper_oid)));

	owner_id = ((Form_pg_operator) GETSTRUCT(tuple))->oprowner;

	ReleaseSysCache(tuple);

	return userid == owner_id;
}

/*
 * Ownership check for a function (specified by OID).
 */
bool
pg_proc_ownercheck(Oid proc_oid, AclId userid)
{
	HeapTuple	tuple;
	AclId		owner_id;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(userid))
		return true;

	tuple = SearchSysCache(PROCOID,
						   ObjectIdGetDatum(proc_oid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
			   errmsg("function with OID %u does not exist", proc_oid)));

	owner_id = ((Form_pg_proc) GETSTRUCT(tuple))->proowner;

	ReleaseSysCache(tuple);

	return userid == owner_id;
}

/*
 * Ownership check for a namespace (specified by OID).
 */
bool
pg_namespace_ownercheck(Oid nsp_oid, AclId userid)
{
	HeapTuple	tuple;
	AclId		owner_id;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(userid))
		return true;

	tuple = SearchSysCache(NAMESPACEOID,
						   ObjectIdGetDatum(nsp_oid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("schema with OID %u does not exist", nsp_oid)));

	owner_id = ((Form_pg_namespace) GETSTRUCT(tuple))->nspowner;

	ReleaseSysCache(tuple);

	return userid == owner_id;
}

/*
 * Ownership check for an operator class (specified by OID).
 */
bool
pg_opclass_ownercheck(Oid opc_oid, AclId userid)
{
	HeapTuple	tuple;
	AclId		owner_id;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(userid))
		return true;

	tuple = SearchSysCache(CLAOID,
						   ObjectIdGetDatum(opc_oid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("operator class with OID %u does not exist",
						opc_oid)));

	owner_id = ((Form_pg_opclass) GETSTRUCT(tuple))->opcowner;

	ReleaseSysCache(tuple);

	return userid == owner_id;
}


/*
 * Ownership check for database (specified as OID)
 */
bool
pg_database_ownercheck(Oid db_oid, AclId userid)
{
	Relation	pg_database;
	ScanKeyData entry[1];
	HeapScanDesc scan;
	HeapTuple	dbtuple;
	int32		dba;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(userid))
		return true;

	/* There's no syscache for pg_database, so must look the hard way */
	pg_database = heap_openr(DatabaseRelationName, AccessShareLock);
	ScanKeyEntryInitialize(&entry[0], 0x0,
						   ObjectIdAttributeNumber, F_OIDEQ,
						   ObjectIdGetDatum(db_oid));
	scan = heap_beginscan(pg_database, SnapshotNow, 1, entry);

	dbtuple = heap_getnext(scan, ForwardScanDirection);

	if (!HeapTupleIsValid(dbtuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database with OID %u does not exist", db_oid)));

	dba = ((Form_pg_database) GETSTRUCT(dbtuple))->datdba;

	heap_endscan(scan);
	heap_close(pg_database, AccessShareLock);

	return userid == dba;
}
