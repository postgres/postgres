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
 *	  $PostgreSQL: pgsql/src/backend/catalog/aclchk.c,v 1.103 2004/06/01 21:49:22 tgl Exp $
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
#include "catalog/pg_conversion.h"
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
	ListCell   *j;
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

		if (grantee->username)
		{
			aclitem.ai_grantee = get_usesysid(grantee->username);

			idtype = ACL_IDTYPE_UID;
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

		aclitem.ai_grantor = grantor_uid;

		/*
		 * The asymmetry in the conditions here comes from the spec.  In
		 * GRANT, the grant_option flag signals WITH GRANT OPTION, which means
		 * to grant both the basic privilege and its grant option.  But in
		 * REVOKE, plain revoke revokes both the basic privilege and its
		 * grant option, while REVOKE GRANT OPTION revokes only the option.
		 */
		ACLITEM_SET_PRIVS_IDTYPE(aclitem,
								 (is_grant || !grant_option) ? privileges : ACL_NO_RIGHTS,
								 (!is_grant || grant_option) ? privileges : ACL_NO_RIGHTS,
								 idtype);

		newer_acl = aclupdate(new_acl, &aclitem, modechg, owner_uid, behavior);

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
	bool		all_privs;
	ListCell   *i;

	if (linitial_int(stmt->privileges) == ACL_ALL_RIGHTS)
	{
		all_privs = true;
		privileges = ACL_ALL_RIGHTS_RELATION;
	}
	else
	{
		all_privs = false;
		privileges = ACL_NO_RIGHTS;
		foreach(i, stmt->privileges)
		{
			AclMode		priv = lfirst_int(i);

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
		AclMode		my_goptions;
		AclMode		this_privileges;
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

		/* Not sensible to grant on an index */
		if (pg_class_tuple->relkind == RELKIND_INDEX)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is an index",
							relvar->relname)));

		/* Composite types aren't tables either */
		if (pg_class_tuple->relkind == RELKIND_COMPOSITE_TYPE)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is a composite type",
							relvar->relname)));

		ownerId = pg_class_tuple->relowner;
		grantorId = select_grantor(ownerId);

		/*
		 * Must be owner or have some privilege on the object (per spec,
		 * any privilege will get you by here).  The owner is always
		 * treated as having all grant options.
		 */
		if (pg_class_ownercheck(relOid, GetUserId()))
			my_goptions = ACL_ALL_RIGHTS_RELATION;
		else
		{
			AclMode		my_rights;

			my_rights = pg_class_aclmask(relOid,
										 GetUserId(),
										 ACL_ALL_RIGHTS_RELATION | ACL_GRANT_OPTION_FOR(ACL_ALL_RIGHTS_RELATION),
										 ACLMASK_ALL);
			if (my_rights == ACL_NO_RIGHTS)
				aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_CLASS,
							   relvar->relname);
			my_goptions = ACL_OPTION_TO_PRIVS(my_rights);
		}

		/*
		 * Restrict the operation to what we can actually grant or revoke,
		 * and issue a warning if appropriate.  (For REVOKE this isn't quite
		 * what the spec says to do: the spec seems to want a warning only
		 * if no privilege bits actually change in the ACL.  In practice
		 * that behavior seems much too noisy, as well as inconsistent with
		 * the GRANT case.)
		 */
		this_privileges = privileges & my_goptions;
		if (stmt->is_grant)
		{
			if (this_privileges == 0)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_GRANTED),
						 errmsg("no privileges were granted")));
			else if (!all_privs && this_privileges != privileges)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_GRANTED),
						 errmsg("not all privileges were granted")));
		}
		else
		{
			if (this_privileges == 0)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_REVOKED),
						 errmsg("no privileges could be revoked")));
			else if (!all_privs && this_privileges != privileges)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_REVOKED),
						 errmsg("not all privileges could be revoked")));
		}

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
									   stmt->grantees, this_privileges,
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
	bool		all_privs;
	ListCell   *i;

	if (linitial_int(stmt->privileges) == ACL_ALL_RIGHTS)
	{
		all_privs = true;
		privileges = ACL_ALL_RIGHTS_DATABASE;
	}
	else
	{
		all_privs = false;
		privileges = ACL_NO_RIGHTS;
		foreach(i, stmt->privileges)
		{
			AclMode		priv = lfirst_int(i);

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
		AclMode		my_goptions;
		AclMode		this_privileges;
		Acl		   *old_acl;
		Acl		   *new_acl;
		AclId		grantorId;
		AclId		ownerId;
		HeapTuple	newtuple;
		Datum		values[Natts_pg_database];
		char		nulls[Natts_pg_database];
		char		replaces[Natts_pg_database];

		relation = heap_openr(DatabaseRelationName, RowExclusiveLock);
		ScanKeyInit(&entry[0],
					Anum_pg_database_datname,
					BTEqualStrategyNumber, F_NAMEEQ,
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

		/*
		 * Must be owner or have some privilege on the object (per spec,
		 * any privilege will get you by here).  The owner is always
		 * treated as having all grant options.
		 */
		if (pg_database_ownercheck(HeapTupleGetOid(tuple), GetUserId()))
			my_goptions = ACL_ALL_RIGHTS_DATABASE;
		else
		{
			AclMode		my_rights;

			my_rights = pg_database_aclmask(HeapTupleGetOid(tuple),
											GetUserId(),
											ACL_ALL_RIGHTS_DATABASE | ACL_GRANT_OPTION_FOR(ACL_ALL_RIGHTS_DATABASE),
											ACLMASK_ALL);
			if (my_rights == ACL_NO_RIGHTS)
				aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_DATABASE,
							   NameStr(pg_database_tuple->datname));
			my_goptions = ACL_OPTION_TO_PRIVS(my_rights);
		}

		/*
		 * Restrict the operation to what we can actually grant or revoke,
		 * and issue a warning if appropriate.  (For REVOKE this isn't quite
		 * what the spec says to do: the spec seems to want a warning only
		 * if no privilege bits actually change in the ACL.  In practice
		 * that behavior seems much too noisy, as well as inconsistent with
		 * the GRANT case.)
		 */
		this_privileges = privileges & my_goptions;
		if (stmt->is_grant)
		{
			if (this_privileges == 0)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_GRANTED),
						 errmsg("no privileges were granted")));
			else if (!all_privs && this_privileges != privileges)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_GRANTED),
						 errmsg("not all privileges were granted")));
		}
		else
		{
			if (this_privileges == 0)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_REVOKED),
						 errmsg("no privileges could be revoked")));
			else if (!all_privs && this_privileges != privileges)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_REVOKED),
						 errmsg("not all privileges could be revoked")));
		}

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
									   stmt->grantees, this_privileges,
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
	bool		all_privs;
	ListCell   *i;

	if (linitial_int(stmt->privileges) == ACL_ALL_RIGHTS)
	{
		all_privs = true;
		privileges = ACL_ALL_RIGHTS_FUNCTION;
	}
	else
	{
		all_privs = false;
		privileges = ACL_NO_RIGHTS;
		foreach(i, stmt->privileges)
		{
			AclMode		priv = lfirst_int(i);

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
		AclMode		my_goptions;
		AclMode		this_privileges;
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

		/*
		 * Must be owner or have some privilege on the object (per spec,
		 * any privilege will get you by here).  The owner is always
		 * treated as having all grant options.
		 */
		if (pg_proc_ownercheck(oid, GetUserId()))
			my_goptions = ACL_ALL_RIGHTS_FUNCTION;
		else
		{
			AclMode		my_rights;

			my_rights = pg_proc_aclmask(oid,
										GetUserId(),
										ACL_ALL_RIGHTS_FUNCTION | ACL_GRANT_OPTION_FOR(ACL_ALL_RIGHTS_FUNCTION),
										ACLMASK_ALL);
			if (my_rights == ACL_NO_RIGHTS)
				aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_PROC,
							   NameStr(pg_proc_tuple->proname));
			my_goptions = ACL_OPTION_TO_PRIVS(my_rights);
		}

		/*
		 * Restrict the operation to what we can actually grant or revoke,
		 * and issue a warning if appropriate.  (For REVOKE this isn't quite
		 * what the spec says to do: the spec seems to want a warning only
		 * if no privilege bits actually change in the ACL.  In practice
		 * that behavior seems much too noisy, as well as inconsistent with
		 * the GRANT case.)
		 */
		this_privileges = privileges & my_goptions;
		if (stmt->is_grant)
		{
			if (this_privileges == 0)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_GRANTED),
						 errmsg("no privileges were granted")));
			else if (!all_privs && this_privileges != privileges)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_GRANTED),
						 errmsg("not all privileges were granted")));
		}
		else
		{
			if (this_privileges == 0)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_REVOKED),
						 errmsg("no privileges could be revoked")));
			else if (!all_privs && this_privileges != privileges)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_REVOKED),
						 errmsg("not all privileges could be revoked")));
		}

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
									   stmt->grantees, this_privileges,
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
	bool		all_privs;
	ListCell   *i;

	if (linitial_int(stmt->privileges) == ACL_ALL_RIGHTS)
	{
		all_privs = true;
		privileges = ACL_ALL_RIGHTS_LANGUAGE;
	}
	else
	{
		all_privs = false;
		privileges = ACL_NO_RIGHTS;
		foreach(i, stmt->privileges)
		{
			AclMode		priv = lfirst_int(i);

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
		AclMode		my_goptions;
		AclMode		this_privileges;
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

		if (!pg_language_tuple->lanpltrusted)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("language \"%s\" is not trusted", langname)));

		/*
		 * Note: for now, languages are treated as owned by the bootstrap
		 * user.  We should add an owner column to pg_language instead.
		 */
		ownerId = BOOTSTRAP_USESYSID;
		grantorId = select_grantor(ownerId);

		/*
		 * Must be owner or have some privilege on the object (per spec,
		 * any privilege will get you by here).  The owner is always
		 * treated as having all grant options.
		 */
		if (superuser())		/* XXX no ownercheck() available */
			my_goptions = ACL_ALL_RIGHTS_LANGUAGE;
		else
		{
			AclMode		my_rights;

			my_rights = pg_language_aclmask(HeapTupleGetOid(tuple),
											GetUserId(),
											ACL_ALL_RIGHTS_LANGUAGE | ACL_GRANT_OPTION_FOR(ACL_ALL_RIGHTS_LANGUAGE),
											ACLMASK_ALL);
			if (my_rights == ACL_NO_RIGHTS)
				aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_LANGUAGE,
							   NameStr(pg_language_tuple->lanname));
			my_goptions = ACL_OPTION_TO_PRIVS(my_rights);
		}

		/*
		 * Restrict the operation to what we can actually grant or revoke,
		 * and issue a warning if appropriate.  (For REVOKE this isn't quite
		 * what the spec says to do: the spec seems to want a warning only
		 * if no privilege bits actually change in the ACL.  In practice
		 * that behavior seems much too noisy, as well as inconsistent with
		 * the GRANT case.)
		 */
		this_privileges = privileges & my_goptions;
		if (stmt->is_grant)
		{
			if (this_privileges == 0)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_GRANTED),
						 errmsg("no privileges were granted")));
			else if (!all_privs && this_privileges != privileges)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_GRANTED),
						 errmsg("not all privileges were granted")));
		}
		else
		{
			if (this_privileges == 0)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_REVOKED),
						 errmsg("no privileges could be revoked")));
			else if (!all_privs && this_privileges != privileges)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_REVOKED),
						 errmsg("not all privileges could be revoked")));
		}

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
									   stmt->grantees, this_privileges,
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
	bool		all_privs;
	ListCell   *i;

	if (linitial_int(stmt->privileges) == ACL_ALL_RIGHTS)
	{
		all_privs = true;
		privileges = ACL_ALL_RIGHTS_NAMESPACE;
	}
	else
	{
		all_privs = false;
		privileges = ACL_NO_RIGHTS;
		foreach(i, stmt->privileges)
		{
			AclMode		priv = lfirst_int(i);

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
		AclMode		my_goptions;
		AclMode		this_privileges;
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

		/*
		 * Must be owner or have some privilege on the object (per spec,
		 * any privilege will get you by here).  The owner is always
		 * treated as having all grant options.
		 */
		if (pg_namespace_ownercheck(HeapTupleGetOid(tuple), GetUserId()))
			my_goptions = ACL_ALL_RIGHTS_NAMESPACE;
		else
		{
			AclMode		my_rights;

			my_rights = pg_namespace_aclmask(HeapTupleGetOid(tuple),
											 GetUserId(),
											 ACL_ALL_RIGHTS_NAMESPACE | ACL_GRANT_OPTION_FOR(ACL_ALL_RIGHTS_NAMESPACE),
											 ACLMASK_ALL);
			if (my_rights == ACL_NO_RIGHTS)
				aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_NAMESPACE,
							   nspname);
			my_goptions = ACL_OPTION_TO_PRIVS(my_rights);
		}

		/*
		 * Restrict the operation to what we can actually grant or revoke,
		 * and issue a warning if appropriate.  (For REVOKE this isn't quite
		 * what the spec says to do: the spec seems to want a warning only
		 * if no privilege bits actually change in the ACL.  In practice
		 * that behavior seems much too noisy, as well as inconsistent with
		 * the GRANT case.)
		 */
		this_privileges = privileges & my_goptions;
		if (stmt->is_grant)
		{
			if (this_privileges == 0)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_GRANTED),
						 errmsg("no privileges were granted")));
			else if (!all_privs && this_privileges != privileges)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_GRANTED),
						 errmsg("not all privileges were granted")));
		}
		else
		{
			if (this_privileges == 0)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_REVOKED),
						 errmsg("no privileges could be revoked")));
			else if (!all_privs && this_privileges != privileges)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING_PRIVILEGE_NOT_REVOKED),
						 errmsg("not all privileges could be revoked")));
		}

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
									   stmt->grantees, this_privileges,
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
 * Exported routine for examining a user's privileges for a table
 *
 * See aclmask() for a description of the API.
 *
 * Note: we give lookup failure the full ereport treatment because the
 * has_table_privilege() family of functions allow users to pass
 * any random OID to this function.  Likewise for the sibling functions
 * below.
 */
AclMode
pg_class_aclmask(Oid table_oid, AclId userid,
				 AclMode mask, AclMaskHow how)
{
	AclMode		result;
	bool		usesuper,
				usecatupd;
	HeapTuple	tuple;
	Form_pg_class classForm;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	AclId		ownerId;

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
				 errmsg("relation with OID %u does not exist",
						table_oid)));
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
	if ((mask & (ACL_INSERT | ACL_UPDATE | ACL_DELETE)) &&
		IsSystemClass(classForm) &&
		classForm->relkind != RELKIND_VIEW &&
		!usecatupd &&
		!allowSystemTableMods)
	{
#ifdef ACLDEBUG
		elog(DEBUG2, "permission denied for system catalog update");
#endif
		mask &= ~(ACL_INSERT | ACL_UPDATE | ACL_DELETE);
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
		return mask;
	}

	/*
	 * Normal case: get the relation's ACL from pg_class
	 */
	ownerId = classForm->relowner;

	aclDatum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_relacl,
							   &isNull);
	if (isNull)
	{
		/* No ACL, so build default ACL */
		acl = acldefault(ACL_OBJECT_RELATION, ownerId);
		aclDatum = (Datum) 0;
	}
	else
	{
		/* detoast rel's ACL if necessary */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclmask(acl, userid, ownerId, mask, how);

	/* if we have a detoasted copy, free it */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	ReleaseSysCache(tuple);

	return result;
}

/*
 * Exported routine for examining a user's privileges for a database
 */
AclMode
pg_database_aclmask(Oid db_oid, AclId userid,
					AclMode mask, AclMaskHow how)
{
	AclMode		result;
	Relation	pg_database;
	ScanKeyData entry[1];
	HeapScanDesc scan;
	HeapTuple	tuple;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	AclId		ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(userid))
		return mask;

	/*
	 * Get the database's ACL from pg_database
	 *
	 * There's no syscache for pg_database, so must look the hard way
	 */
	pg_database = heap_openr(DatabaseRelationName, AccessShareLock);
	ScanKeyInit(&entry[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(db_oid));
	scan = heap_beginscan(pg_database, SnapshotNow, 1, entry);
	tuple = heap_getnext(scan, ForwardScanDirection);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database with OID %u does not exist", db_oid)));

	ownerId = ((Form_pg_database) GETSTRUCT(tuple))->datdba;

	aclDatum = heap_getattr(tuple, Anum_pg_database_datacl,
							RelationGetDescr(pg_database), &isNull);

	if (isNull)
	{
		/* No ACL, so build default ACL */
		acl = acldefault(ACL_OBJECT_DATABASE, ownerId);
		aclDatum = (Datum) 0;
	}
	else
	{
		/* detoast ACL if necessary */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclmask(acl, userid, ownerId, mask, how);

	/* if we have a detoasted copy, free it */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	heap_endscan(scan);
	heap_close(pg_database, AccessShareLock);

	return result;
}

/*
 * Exported routine for examining a user's privileges for a function
 */
AclMode
pg_proc_aclmask(Oid proc_oid, AclId userid,
				AclMode mask, AclMaskHow how)
{
	AclMode		result;
	HeapTuple	tuple;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	AclId		ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(userid))
		return mask;

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

	ownerId = ((Form_pg_proc) GETSTRUCT(tuple))->proowner;

	aclDatum = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_proacl,
							   &isNull);
	if (isNull)
	{
		/* No ACL, so build default ACL */
		acl = acldefault(ACL_OBJECT_FUNCTION, ownerId);
		aclDatum = (Datum) 0;
	}
	else
	{
		/* detoast ACL if necessary */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclmask(acl, userid, ownerId, mask, how);

	/* if we have a detoasted copy, free it */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	ReleaseSysCache(tuple);

	return result;
}

/*
 * Exported routine for examining a user's privileges for a language
 */
AclMode
pg_language_aclmask(Oid lang_oid, AclId userid,
					AclMode mask, AclMaskHow how)
{
	AclMode		result;
	HeapTuple	tuple;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	AclId		ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(userid))
		return mask;

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

	/* XXX pg_language should have an owner column, but doesn't */
	ownerId = BOOTSTRAP_USESYSID;

	aclDatum = SysCacheGetAttr(LANGOID, tuple, Anum_pg_language_lanacl,
							   &isNull);
	if (isNull)
	{
		/* No ACL, so build default ACL */
		acl = acldefault(ACL_OBJECT_LANGUAGE, ownerId);
		aclDatum = (Datum) 0;
	}
	else
	{
		/* detoast ACL if necessary */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclmask(acl, userid, ownerId, mask, how);

	/* if we have a detoasted copy, free it */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	ReleaseSysCache(tuple);

	return result;
}

/*
 * Exported routine for examining a user's privileges for a namespace
 */
AclMode
pg_namespace_aclmask(Oid nsp_oid, AclId userid,
					 AclMode mask, AclMaskHow how)
{
	AclMode		result;
	HeapTuple	tuple;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	AclId		ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(userid))
		return mask;

	/*
	 * If we have been assigned this namespace as a temp namespace,
	 * check to make sure we have CREATE TEMP permission on the database,
	 * and if so act as though we have all standard (but not GRANT OPTION)
	 * permissions on the namespace.  If we don't have CREATE TEMP, act as
	 * though we have only USAGE (and not CREATE) rights.
	 *
	 * This may seem redundant given the check in InitTempTableNamespace,
	 * but it really isn't since current user ID may have changed since then.
	 * The upshot of this behavior is that a SECURITY DEFINER function can
	 * create temp tables that can then be accessed (if permission is granted)
	 * by code in the same session that doesn't have permissions to create
	 * temp tables.
	 *
	 * XXX Would it be safe to ereport a special error message as
	 * InitTempTableNamespace does?  Returning zero here means we'll get a
	 * generic "permission denied for schema pg_temp_N" message, which is not
	 * remarkably user-friendly.
	 */
	if (isTempNamespace(nsp_oid))
	{
		if (pg_database_aclcheck(MyDatabaseId, GetUserId(),
								 ACL_CREATE_TEMP) == ACLCHECK_OK)
			return mask & ACL_ALL_RIGHTS_NAMESPACE;
		else
			return mask & ACL_USAGE;
	}

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

	ownerId = ((Form_pg_namespace) GETSTRUCT(tuple))->nspowner;

	aclDatum = SysCacheGetAttr(NAMESPACEOID, tuple, Anum_pg_namespace_nspacl,
							   &isNull);
	if (isNull)
	{
		/* No ACL, so build default ACL */
		acl = acldefault(ACL_OBJECT_NAMESPACE, ownerId);
		aclDatum = (Datum) 0;
	}
	else
	{
		/* detoast ACL if necessary */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclmask(acl, userid, ownerId, mask, how);

	/* if we have a detoasted copy, free it */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	ReleaseSysCache(tuple);

	return result;
}


/*
 * Exported routine for checking a user's access privileges to a table
 *
 * Returns ACLCHECK_OK if the user has any of the privileges identified by
 * 'mode'; otherwise returns a suitable error code (in practice, always
 * ACLCHECK_NO_PRIV).
 */
AclResult
pg_class_aclcheck(Oid table_oid, AclId userid, AclMode mode)
{
	if (pg_class_aclmask(table_oid, userid, mode, ACLMASK_ANY) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
}

/*
 * Exported routine for checking a user's access privileges to a database
 */
AclResult
pg_database_aclcheck(Oid db_oid, AclId userid, AclMode mode)
{
	if (pg_database_aclmask(db_oid, userid, mode, ACLMASK_ANY) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
}

/*
 * Exported routine for checking a user's access privileges to a function
 */
AclResult
pg_proc_aclcheck(Oid proc_oid, AclId userid, AclMode mode)
{
	if (pg_proc_aclmask(proc_oid, userid, mode, ACLMASK_ANY) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
}

/*
 * Exported routine for checking a user's access privileges to a language
 */
AclResult
pg_language_aclcheck(Oid lang_oid, AclId userid, AclMode mode)
{
	if (pg_language_aclmask(lang_oid, userid, mode, ACLMASK_ANY) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
}

/*
 * Exported routine for checking a user's access privileges to a namespace
 */
AclResult
pg_namespace_aclcheck(Oid nsp_oid, AclId userid, AclMode mode)
{
	if (pg_namespace_aclmask(nsp_oid, userid, mode, ACLMASK_ANY) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
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
	ScanKeyInit(&entry[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
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

/*
 * Ownership check for a conversion (specified by OID).
 */
bool
pg_conversion_ownercheck(Oid conv_oid, AclId userid)
{
	HeapTuple	tuple;
	AclId		owner_id;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(userid))
		return true;

	tuple = SearchSysCache(CONOID,
						   ObjectIdGetDatum(conv_oid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("conversion with OID %u does not exist", conv_oid)));

	owner_id = ((Form_pg_conversion) GETSTRUCT(tuple))->conowner;

	ReleaseSysCache(tuple);

	return userid == owner_id;
}
