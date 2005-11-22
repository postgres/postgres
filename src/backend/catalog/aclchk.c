/*-------------------------------------------------------------------------
 *
 * aclchk.c
 *	  Routines to check access control permissions.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/catalog/aclchk.c,v 1.120.2.1 2005/11/22 18:23:06 momjian Exp $
 *
 * NOTES
 *	  See acl.h.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_database.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_tablespace.h"
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
static void ExecuteGrantStmt_Tablespace(GrantStmt *stmt);

static AclMode string_to_privilege(const char *privname);
static const char *privilege_to_string(AclMode privilege);


#ifdef ACLDEBUG
static void
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
					 Oid grantorId, Oid ownerId)
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
		AclItem aclitem;
		Acl		   *newer_acl;

		if (grantee->rolname)
			aclitem.	ai_grantee = get_roleid_checked(grantee->rolname);

		else
			aclitem.	ai_grantee = ACL_ID_PUBLIC;

		/*
		 * Grant options can only be granted to individual roles, not PUBLIC.
		 * The reason is that if a user would re-grant a privilege that he
		 * held through PUBLIC, and later the user is removed, the situation
		 * is impossible to clean up.
		 */
		if (is_grant && grant_option && aclitem.ai_grantee == ACL_ID_PUBLIC)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_GRANT_OPERATION),
					 errmsg("grant options can only be granted to roles")));

		aclitem.	ai_grantor = grantorId;

		/*
		 * The asymmetry in the conditions here comes from the spec.  In
		 * GRANT, the grant_option flag signals WITH GRANT OPTION, which means
		 * to grant both the basic privilege and its grant option. But in
		 * REVOKE, plain revoke revokes both the basic privilege and its grant
		 * option, while REVOKE GRANT OPTION revokes only the option.
		 */
		ACLITEM_SET_PRIVS_GOPTIONS(aclitem,
					(is_grant || !grant_option) ? privileges : ACL_NO_RIGHTS,
				   (!is_grant || grant_option) ? privileges : ACL_NO_RIGHTS);

		newer_acl = aclupdate(new_acl, &aclitem, modechg, ownerId, behavior);

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
		case ACL_OBJECT_TABLESPACE:
			ExecuteGrantStmt_Tablespace(stmt);
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

	if (stmt->privileges == NIL)
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
			char	   *privname = strVal(lfirst(i));
			AclMode		priv = string_to_privilege(privname);

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
		AclMode		avail_goptions;
		AclMode		this_privileges;
		Acl		   *old_acl;
		Acl		   *new_acl;
		Oid			grantorId;
		Oid			ownerId;
		HeapTuple	newtuple;
		Datum		values[Natts_pg_class];
		char		nulls[Natts_pg_class];
		char		replaces[Natts_pg_class];
		int			noldmembers;
		int			nnewmembers;
		Oid		   *oldmembers;
		Oid		   *newmembers;

		/* open pg_class */
		relation = heap_open(RelationRelationId, RowExclusiveLock);
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

		/*
		 * Get owner ID and working copy of existing ACL. If there's no ACL,
		 * substitute the proper default.
		 */
		ownerId = pg_class_tuple->relowner;
		aclDatum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_relacl,
								   &isNull);
		if (isNull)
			old_acl = acldefault(ACL_OBJECT_RELATION, ownerId);
		else
			old_acl = DatumGetAclPCopy(aclDatum);

		/* Determine ID to do the grant as, and available grant options */
		select_best_grantor(GetUserId(), privileges,
							old_acl, ownerId,
							&grantorId, &avail_goptions);

		/*
		 * If we found no grant options, consider whether to issue a hard
		 * error.  Per spec, having any privilege at all on the object will
		 * get you by here.
		 */
		if (avail_goptions == ACL_NO_RIGHTS)
		{
			if (pg_class_aclmask(relOid,
								 grantorId,
								 ACL_ALL_RIGHTS_RELATION | ACL_GRANT_OPTION_FOR(ACL_ALL_RIGHTS_RELATION),
								 ACLMASK_ANY) == ACL_NO_RIGHTS)
				aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_CLASS,
							   relvar->relname);
		}

		/*
		 * Restrict the operation to what we can actually grant or revoke, and
		 * issue a warning if appropriate.	(For REVOKE this isn't quite what
		 * the spec says to do: the spec seems to want a warning only if no
		 * privilege bits actually change in the ACL. In practice that
		 * behavior seems much too noisy, as well as inconsistent with the
		 * GRANT case.)
		 */
		this_privileges = privileges & ACL_OPTION_TO_PRIVS(avail_goptions);
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
		 * Generate new ACL.
		 *
		 * We need the members of both old and new ACLs so we can correct the
		 * shared dependency information.
		 */
		noldmembers = aclmembers(old_acl, &oldmembers);

		new_acl = merge_acl_with_grant(old_acl, stmt->is_grant,
									   stmt->grant_option, stmt->behavior,
									   stmt->grantees, this_privileges,
									   grantorId, ownerId);

		nnewmembers = aclmembers(new_acl, &newmembers);

		/* finished building new ACL value, now insert it */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, ' ', sizeof(nulls));
		MemSet(replaces, ' ', sizeof(replaces));

		replaces[Anum_pg_class_relacl - 1] = 'r';
		values[Anum_pg_class_relacl - 1] = PointerGetDatum(new_acl);

		newtuple = heap_modifytuple(tuple, RelationGetDescr(relation), values, nulls, replaces);

		simple_heap_update(relation, &newtuple->t_self, newtuple);

		/* keep the catalog indexes up to date */
		CatalogUpdateIndexes(relation, newtuple);

		/* Update the shared dependency ACL info */
		updateAclDependencies(RelationRelationId, relOid,
							  ownerId, stmt->is_grant,
							  noldmembers, oldmembers,
							  nnewmembers, newmembers);

		ReleaseSysCache(tuple);

		pfree(new_acl);

		heap_close(relation, RowExclusiveLock);

		/* prevent error when processing duplicate objects */
		CommandCounterIncrement();
	}
}

static void
ExecuteGrantStmt_Database(GrantStmt *stmt)
{
	AclMode		privileges;
	bool		all_privs;
	ListCell   *i;

	if (stmt->privileges == NIL)
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
			char	   *privname = strVal(lfirst(i));
			AclMode		priv = string_to_privilege(privname);

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
		AclMode		avail_goptions;
		AclMode		this_privileges;
		Acl		   *old_acl;
		Acl		   *new_acl;
		Oid			grantorId;
		Oid			ownerId;
		HeapTuple	newtuple;
		Datum		values[Natts_pg_database];
		char		nulls[Natts_pg_database];
		char		replaces[Natts_pg_database];
		int			noldmembers;
		int			nnewmembers;
		Oid		   *oldmembers;
		Oid		   *newmembers;

		relation = heap_open(DatabaseRelationId, RowExclusiveLock);
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

		/*
		 * Get owner ID and working copy of existing ACL. If there's no ACL,
		 * substitute the proper default.
		 */
		ownerId = pg_database_tuple->datdba;
		aclDatum = heap_getattr(tuple, Anum_pg_database_datacl,
								RelationGetDescr(relation), &isNull);
		if (isNull)
			old_acl = acldefault(ACL_OBJECT_DATABASE, ownerId);
		else
			old_acl = DatumGetAclPCopy(aclDatum);

		/* Determine ID to do the grant as, and available grant options */
		select_best_grantor(GetUserId(), privileges,
							old_acl, ownerId,
							&grantorId, &avail_goptions);

		/*
		 * If we found no grant options, consider whether to issue a hard
		 * error.  Per spec, having any privilege at all on the object will
		 * get you by here.
		 */
		if (avail_goptions == ACL_NO_RIGHTS)
		{
			if (pg_database_aclmask(HeapTupleGetOid(tuple),
									grantorId,
									ACL_ALL_RIGHTS_DATABASE | ACL_GRANT_OPTION_FOR(ACL_ALL_RIGHTS_DATABASE),
									ACLMASK_ANY) == ACL_NO_RIGHTS)
				aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_DATABASE,
							   NameStr(pg_database_tuple->datname));
		}

		/*
		 * Restrict the operation to what we can actually grant or revoke, and
		 * issue a warning if appropriate.	(For REVOKE this isn't quite what
		 * the spec says to do: the spec seems to want a warning only if no
		 * privilege bits actually change in the ACL. In practice that
		 * behavior seems much too noisy, as well as inconsistent with the
		 * GRANT case.)
		 */
		this_privileges = privileges & ACL_OPTION_TO_PRIVS(avail_goptions);
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
		 * Generate new ACL.
		 *
		 * We need the members of both old and new ACLs so we can correct the
		 * shared dependency information.
		 */
		noldmembers = aclmembers(old_acl, &oldmembers);

		new_acl = merge_acl_with_grant(old_acl, stmt->is_grant,
									   stmt->grant_option, stmt->behavior,
									   stmt->grantees, this_privileges,
									   grantorId, ownerId);

		nnewmembers = aclmembers(new_acl, &newmembers);

		/* finished building new ACL value, now insert it */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, ' ', sizeof(nulls));
		MemSet(replaces, ' ', sizeof(replaces));

		replaces[Anum_pg_database_datacl - 1] = 'r';
		values[Anum_pg_database_datacl - 1] = PointerGetDatum(new_acl);

		newtuple = heap_modifytuple(tuple, RelationGetDescr(relation), values, nulls, replaces);

		simple_heap_update(relation, &newtuple->t_self, newtuple);

		/* keep the catalog indexes up to date */
		CatalogUpdateIndexes(relation, newtuple);

		/* Update the shared dependency ACL info */
		updateAclDependencies(DatabaseRelationId, HeapTupleGetOid(tuple),
							  ownerId, stmt->is_grant,
							  noldmembers, oldmembers,
							  nnewmembers, newmembers);

		pfree(new_acl);

		heap_endscan(scan);

		heap_close(relation, RowExclusiveLock);

		/* prevent error when processing duplicate objects */
		CommandCounterIncrement();
	}
}

static void
ExecuteGrantStmt_Function(GrantStmt *stmt)
{
	AclMode		privileges;
	bool		all_privs;
	ListCell   *i;

	if (stmt->privileges == NIL)
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
			char	   *privname = strVal(lfirst(i));
			AclMode		priv = string_to_privilege(privname);

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
		AclMode		avail_goptions;
		AclMode		this_privileges;
		Acl		   *old_acl;
		Acl		   *new_acl;
		Oid			grantorId;
		Oid			ownerId;
		HeapTuple	newtuple;
		Datum		values[Natts_pg_proc];
		char		nulls[Natts_pg_proc];
		char		replaces[Natts_pg_proc];
		int			noldmembers;
		int			nnewmembers;
		Oid		   *oldmembers;
		Oid		   *newmembers;

		oid = LookupFuncNameTypeNames(func->funcname, func->funcargs, false);

		relation = heap_open(ProcedureRelationId, RowExclusiveLock);
		tuple = SearchSysCache(PROCOID,
							   ObjectIdGetDatum(oid),
							   0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for function %u", oid);
		pg_proc_tuple = (Form_pg_proc) GETSTRUCT(tuple);

		/*
		 * Get owner ID and working copy of existing ACL. If there's no ACL,
		 * substitute the proper default.
		 */
		ownerId = pg_proc_tuple->proowner;
		aclDatum = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_proacl,
								   &isNull);
		if (isNull)
			old_acl = acldefault(ACL_OBJECT_FUNCTION, ownerId);
		else
			old_acl = DatumGetAclPCopy(aclDatum);

		/* Determine ID to do the grant as, and available grant options */
		select_best_grantor(GetUserId(), privileges,
							old_acl, ownerId,
							&grantorId, &avail_goptions);

		/*
		 * If we found no grant options, consider whether to issue a hard
		 * error.  Per spec, having any privilege at all on the object will
		 * get you by here.
		 */
		if (avail_goptions == ACL_NO_RIGHTS)
		{
			if (pg_proc_aclmask(oid,
								grantorId,
								ACL_ALL_RIGHTS_FUNCTION | ACL_GRANT_OPTION_FOR(ACL_ALL_RIGHTS_FUNCTION),
								ACLMASK_ANY) == ACL_NO_RIGHTS)
				aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_PROC,
							   NameStr(pg_proc_tuple->proname));
		}

		/*
		 * Restrict the operation to what we can actually grant or revoke, and
		 * issue a warning if appropriate.	(For REVOKE this isn't quite what
		 * the spec says to do: the spec seems to want a warning only if no
		 * privilege bits actually change in the ACL. In practice that
		 * behavior seems much too noisy, as well as inconsistent with the
		 * GRANT case.)
		 */
		this_privileges = privileges & ACL_OPTION_TO_PRIVS(avail_goptions);
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
		 * Generate new ACL.
		 *
		 * We need the members of both old and new ACLs so we can correct the
		 * shared dependency information.
		 */
		noldmembers = aclmembers(old_acl, &oldmembers);

		new_acl = merge_acl_with_grant(old_acl, stmt->is_grant,
									   stmt->grant_option, stmt->behavior,
									   stmt->grantees, this_privileges,
									   grantorId, ownerId);

		nnewmembers = aclmembers(new_acl, &newmembers);

		/* finished building new ACL value, now insert it */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, ' ', sizeof(nulls));
		MemSet(replaces, ' ', sizeof(replaces));

		replaces[Anum_pg_proc_proacl - 1] = 'r';
		values[Anum_pg_proc_proacl - 1] = PointerGetDatum(new_acl);

		newtuple = heap_modifytuple(tuple, RelationGetDescr(relation), values, nulls, replaces);

		simple_heap_update(relation, &newtuple->t_self, newtuple);

		/* keep the catalog indexes up to date */
		CatalogUpdateIndexes(relation, newtuple);

		/* Update the shared dependency ACL info */
		updateAclDependencies(ProcedureRelationId, oid,
							  ownerId, stmt->is_grant,
							  noldmembers, oldmembers,
							  nnewmembers, newmembers);

		ReleaseSysCache(tuple);

		pfree(new_acl);

		heap_close(relation, RowExclusiveLock);

		/* prevent error when processing duplicate objects */
		CommandCounterIncrement();
	}
}

static void
ExecuteGrantStmt_Language(GrantStmt *stmt)
{
	AclMode		privileges;
	bool		all_privs;
	ListCell   *i;

	if (stmt->privileges == NIL)
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
			char	   *privname = strVal(lfirst(i));
			AclMode		priv = string_to_privilege(privname);

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
		AclMode		avail_goptions;
		AclMode		this_privileges;
		Acl		   *old_acl;
		Acl		   *new_acl;
		Oid			grantorId;
		Oid			ownerId;
		HeapTuple	newtuple;
		Datum		values[Natts_pg_language];
		char		nulls[Natts_pg_language];
		char		replaces[Natts_pg_language];
		int			noldmembers;
		int			nnewmembers;
		Oid		   *oldmembers;
		Oid		   *newmembers;

		relation = heap_open(LanguageRelationId, RowExclusiveLock);
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
					 errmsg("language \"%s\" is not trusted", langname),
				   errhint("Only superusers may use untrusted languages.")));

		/*
		 * Get owner ID and working copy of existing ACL. If there's no ACL,
		 * substitute the proper default.
		 *
		 * Note: for now, languages are treated as owned by the bootstrap
		 * user. We should add an owner column to pg_language instead.
		 */
		ownerId = BOOTSTRAP_SUPERUSERID;
		aclDatum = SysCacheGetAttr(LANGNAME, tuple, Anum_pg_language_lanacl,
								   &isNull);
		if (isNull)
			old_acl = acldefault(ACL_OBJECT_LANGUAGE, ownerId);
		else
			old_acl = DatumGetAclPCopy(aclDatum);

		/* Determine ID to do the grant as, and available grant options */
		select_best_grantor(GetUserId(), privileges,
							old_acl, ownerId,
							&grantorId, &avail_goptions);

		/*
		 * If we found no grant options, consider whether to issue a hard
		 * error.  Per spec, having any privilege at all on the object will
		 * get you by here.
		 */
		if (avail_goptions == ACL_NO_RIGHTS)
		{
			if (pg_language_aclmask(HeapTupleGetOid(tuple),
									grantorId,
									ACL_ALL_RIGHTS_LANGUAGE | ACL_GRANT_OPTION_FOR(ACL_ALL_RIGHTS_LANGUAGE),
									ACLMASK_ANY) == ACL_NO_RIGHTS)
				aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_LANGUAGE,
							   NameStr(pg_language_tuple->lanname));
		}

		/*
		 * Restrict the operation to what we can actually grant or revoke, and
		 * issue a warning if appropriate.	(For REVOKE this isn't quite what
		 * the spec says to do: the spec seems to want a warning only if no
		 * privilege bits actually change in the ACL. In practice that
		 * behavior seems much too noisy, as well as inconsistent with the
		 * GRANT case.)
		 */
		this_privileges = privileges & ACL_OPTION_TO_PRIVS(avail_goptions);
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
		 * Generate new ACL.
		 *
		 * We need the members of both old and new ACLs so we can correct the
		 * shared dependency information.
		 */
		noldmembers = aclmembers(old_acl, &oldmembers);

		new_acl = merge_acl_with_grant(old_acl, stmt->is_grant,
									   stmt->grant_option, stmt->behavior,
									   stmt->grantees, this_privileges,
									   grantorId, ownerId);

		nnewmembers = aclmembers(new_acl, &newmembers);

		/* finished building new ACL value, now insert it */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, ' ', sizeof(nulls));
		MemSet(replaces, ' ', sizeof(replaces));

		replaces[Anum_pg_language_lanacl - 1] = 'r';
		values[Anum_pg_language_lanacl - 1] = PointerGetDatum(new_acl);

		newtuple = heap_modifytuple(tuple, RelationGetDescr(relation), values, nulls, replaces);

		simple_heap_update(relation, &newtuple->t_self, newtuple);

		/* keep the catalog indexes up to date */
		CatalogUpdateIndexes(relation, newtuple);

		/* Update the shared dependency ACL info */
		updateAclDependencies(LanguageRelationId, HeapTupleGetOid(tuple),
							  ownerId, stmt->is_grant,
							  noldmembers, oldmembers,
							  nnewmembers, newmembers);

		ReleaseSysCache(tuple);

		pfree(new_acl);

		heap_close(relation, RowExclusiveLock);

		/* prevent error when processing duplicate objects */
		CommandCounterIncrement();
	}
}

static void
ExecuteGrantStmt_Namespace(GrantStmt *stmt)
{
	AclMode		privileges;
	bool		all_privs;
	ListCell   *i;

	if (stmt->privileges == NIL)
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
			char	   *privname = strVal(lfirst(i));
			AclMode		priv = string_to_privilege(privname);

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
		AclMode		avail_goptions;
		AclMode		this_privileges;
		Acl		   *old_acl;
		Acl		   *new_acl;
		Oid			grantorId;
		Oid			ownerId;
		HeapTuple	newtuple;
		Datum		values[Natts_pg_namespace];
		char		nulls[Natts_pg_namespace];
		char		replaces[Natts_pg_namespace];
		int			noldmembers;
		int			nnewmembers;
		Oid		   *oldmembers;
		Oid		   *newmembers;

		relation = heap_open(NamespaceRelationId, RowExclusiveLock);
		tuple = SearchSysCache(NAMESPACENAME,
							   CStringGetDatum(nspname),
							   0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("schema \"%s\" does not exist", nspname)));
		pg_namespace_tuple = (Form_pg_namespace) GETSTRUCT(tuple);

		/*
		 * Get owner ID and working copy of existing ACL. If there's no ACL,
		 * substitute the proper default.
		 */
		ownerId = pg_namespace_tuple->nspowner;
		aclDatum = SysCacheGetAttr(NAMESPACENAME, tuple,
								   Anum_pg_namespace_nspacl,
								   &isNull);
		if (isNull)
			old_acl = acldefault(ACL_OBJECT_NAMESPACE, ownerId);
		else
			old_acl = DatumGetAclPCopy(aclDatum);

		/* Determine ID to do the grant as, and available grant options */
		select_best_grantor(GetUserId(), privileges,
							old_acl, ownerId,
							&grantorId, &avail_goptions);

		/*
		 * If we found no grant options, consider whether to issue a hard
		 * error.  Per spec, having any privilege at all on the object will
		 * get you by here.
		 */
		if (avail_goptions == ACL_NO_RIGHTS)
		{
			if (pg_namespace_aclmask(HeapTupleGetOid(tuple),
									 grantorId,
									 ACL_ALL_RIGHTS_NAMESPACE | ACL_GRANT_OPTION_FOR(ACL_ALL_RIGHTS_NAMESPACE),
									 ACLMASK_ANY) == ACL_NO_RIGHTS)
				aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_NAMESPACE,
							   nspname);
		}

		/*
		 * Restrict the operation to what we can actually grant or revoke, and
		 * issue a warning if appropriate.	(For REVOKE this isn't quite what
		 * the spec says to do: the spec seems to want a warning only if no
		 * privilege bits actually change in the ACL. In practice that
		 * behavior seems much too noisy, as well as inconsistent with the
		 * GRANT case.)
		 */
		this_privileges = privileges & ACL_OPTION_TO_PRIVS(avail_goptions);
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
		 * Generate new ACL.
		 *
		 * We need the members of both old and new ACLs so we can correct the
		 * shared dependency information.
		 */
		noldmembers = aclmembers(old_acl, &oldmembers);

		new_acl = merge_acl_with_grant(old_acl, stmt->is_grant,
									   stmt->grant_option, stmt->behavior,
									   stmt->grantees, this_privileges,
									   grantorId, ownerId);

		nnewmembers = aclmembers(new_acl, &newmembers);

		/* finished building new ACL value, now insert it */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, ' ', sizeof(nulls));
		MemSet(replaces, ' ', sizeof(replaces));

		replaces[Anum_pg_namespace_nspacl - 1] = 'r';
		values[Anum_pg_namespace_nspacl - 1] = PointerGetDatum(new_acl);

		newtuple = heap_modifytuple(tuple, RelationGetDescr(relation), values, nulls, replaces);

		simple_heap_update(relation, &newtuple->t_self, newtuple);

		/* keep the catalog indexes up to date */
		CatalogUpdateIndexes(relation, newtuple);

		/* Update the shared dependency ACL info */
		updateAclDependencies(NamespaceRelationId, HeapTupleGetOid(tuple),
							  ownerId, stmt->is_grant,
							  noldmembers, oldmembers,
							  nnewmembers, newmembers);

		ReleaseSysCache(tuple);

		pfree(new_acl);

		heap_close(relation, RowExclusiveLock);

		/* prevent error when processing duplicate objects */
		CommandCounterIncrement();
	}
}

static void
ExecuteGrantStmt_Tablespace(GrantStmt *stmt)
{
	AclMode		privileges;
	bool		all_privs;
	ListCell   *i;

	if (stmt->privileges == NIL)
	{
		all_privs = true;
		privileges = ACL_ALL_RIGHTS_TABLESPACE;
	}
	else
	{
		all_privs = false;
		privileges = ACL_NO_RIGHTS;
		foreach(i, stmt->privileges)
		{
			char	   *privname = strVal(lfirst(i));
			AclMode		priv = string_to_privilege(privname);

			if (priv & ~((AclMode) ACL_ALL_RIGHTS_TABLESPACE))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_GRANT_OPERATION),
						 errmsg("invalid privilege type %s for tablespace",
								privilege_to_string(priv))));
			privileges |= priv;
		}
	}

	foreach(i, stmt->objects)
	{
		char	   *spcname = strVal(lfirst(i));
		Relation	relation;
		ScanKeyData entry[1];
		HeapScanDesc scan;
		HeapTuple	tuple;
		Form_pg_tablespace pg_tablespace_tuple;
		Datum		aclDatum;
		bool		isNull;
		AclMode		avail_goptions;
		AclMode		this_privileges;
		Acl		   *old_acl;
		Acl		   *new_acl;
		Oid			grantorId;
		Oid			ownerId;
		HeapTuple	newtuple;
		Datum		values[Natts_pg_tablespace];
		char		nulls[Natts_pg_tablespace];
		char		replaces[Natts_pg_tablespace];
		int			noldmembers;
		int			nnewmembers;
		Oid		   *oldmembers;
		Oid		   *newmembers;

		relation = heap_open(TableSpaceRelationId, RowExclusiveLock);
		ScanKeyInit(&entry[0],
					Anum_pg_tablespace_spcname,
					BTEqualStrategyNumber, F_NAMEEQ,
					CStringGetDatum(spcname));
		scan = heap_beginscan(relation, SnapshotNow, 1, entry);
		tuple = heap_getnext(scan, ForwardScanDirection);
		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("tablespace \"%s\" does not exist", spcname)));
		pg_tablespace_tuple = (Form_pg_tablespace) GETSTRUCT(tuple);

		/*
		 * Get owner ID and working copy of existing ACL. If there's no ACL,
		 * substitute the proper default.
		 */
		ownerId = pg_tablespace_tuple->spcowner;
		aclDatum = heap_getattr(tuple, Anum_pg_tablespace_spcacl,
								RelationGetDescr(relation), &isNull);
		if (isNull)
			old_acl = acldefault(ACL_OBJECT_TABLESPACE, ownerId);
		else
			old_acl = DatumGetAclPCopy(aclDatum);

		/* Determine ID to do the grant as, and available grant options */
		select_best_grantor(GetUserId(), privileges,
							old_acl, ownerId,
							&grantorId, &avail_goptions);

		/*
		 * If we found no grant options, consider whether to issue a hard
		 * error.  Per spec, having any privilege at all on the object will
		 * get you by here.
		 */
		if (avail_goptions == ACL_NO_RIGHTS)
		{
			if (pg_tablespace_aclmask(HeapTupleGetOid(tuple),
									  grantorId,
									  ACL_ALL_RIGHTS_TABLESPACE | ACL_GRANT_OPTION_FOR(ACL_ALL_RIGHTS_TABLESPACE),
									  ACLMASK_ANY) == ACL_NO_RIGHTS)
				aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_TABLESPACE,
							   spcname);
		}

		/*
		 * Restrict the operation to what we can actually grant or revoke, and
		 * issue a warning if appropriate.	(For REVOKE this isn't quite what
		 * the spec says to do: the spec seems to want a warning only if no
		 * privilege bits actually change in the ACL. In practice that
		 * behavior seems much too noisy, as well as inconsistent with the
		 * GRANT case.)
		 */
		this_privileges = privileges & ACL_OPTION_TO_PRIVS(avail_goptions);
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
		 * Generate new ACL.
		 *
		 * We need the members of both old and new ACLs so we can correct the
		 * shared dependency information.
		 */
		noldmembers = aclmembers(old_acl, &oldmembers);

		new_acl = merge_acl_with_grant(old_acl, stmt->is_grant,
									   stmt->grant_option, stmt->behavior,
									   stmt->grantees, this_privileges,
									   grantorId, ownerId);

		nnewmembers = aclmembers(new_acl, &newmembers);

		/* finished building new ACL value, now insert it */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, ' ', sizeof(nulls));
		MemSet(replaces, ' ', sizeof(replaces));

		replaces[Anum_pg_tablespace_spcacl - 1] = 'r';
		values[Anum_pg_tablespace_spcacl - 1] = PointerGetDatum(new_acl);

		newtuple = heap_modifytuple(tuple, RelationGetDescr(relation), values, nulls, replaces);

		simple_heap_update(relation, &newtuple->t_self, newtuple);

		/* keep the catalog indexes up to date */
		CatalogUpdateIndexes(relation, newtuple);

		/* Update the shared dependency ACL info */
		updateAclDependencies(TableSpaceRelationId, HeapTupleGetOid(tuple),
							  ownerId, stmt->is_grant,
							  noldmembers, oldmembers,
							  nnewmembers, newmembers);

		pfree(new_acl);

		heap_endscan(scan);
		heap_close(relation, RowExclusiveLock);

		/* prevent error when processing duplicate objects */
		CommandCounterIncrement();
	}
}


static AclMode
string_to_privilege(const char *privname)
{
	if (strcmp(privname, "insert") == 0)
		return ACL_INSERT;
	if (strcmp(privname, "select") == 0)
		return ACL_SELECT;
	if (strcmp(privname, "update") == 0)
		return ACL_UPDATE;
	if (strcmp(privname, "delete") == 0)
		return ACL_DELETE;
	if (strcmp(privname, "rule") == 0)
		return ACL_RULE;
	if (strcmp(privname, "references") == 0)
		return ACL_REFERENCES;
	if (strcmp(privname, "trigger") == 0)
		return ACL_TRIGGER;
	if (strcmp(privname, "execute") == 0)
		return ACL_EXECUTE;
	if (strcmp(privname, "usage") == 0)
		return ACL_USAGE;
	if (strcmp(privname, "create") == 0)
		return ACL_CREATE;
	if (strcmp(privname, "temporary") == 0)
		return ACL_CREATE_TEMP;
	if (strcmp(privname, "temp") == 0)
		return ACL_CREATE_TEMP;
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("unrecognized privilege type \"%s\"", privname)));
	return 0;					/* appease compiler */
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
	gettext_noop("permission denied for conversion %s"),
	/* ACL_KIND_TABLESPACE */
	gettext_noop("permission denied for tablespace %s")
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
	gettext_noop("must be owner of conversion %s"),
	/* ACL_KIND_TABLESPACE */
	gettext_noop("must be owner of tablespace %s")
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


/* Check if given user has rolcatupdate privilege according to pg_authid */
static bool
has_rolcatupdate(Oid roleid)
{
	bool		rolcatupdate;
	HeapTuple	tuple;

	tuple = SearchSysCache(AUTHOID,
						   ObjectIdGetDatum(roleid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("role with OID %u does not exist", roleid)));

	rolcatupdate = ((Form_pg_authid) GETSTRUCT(tuple))->rolcatupdate;

	ReleaseSysCache(tuple);

	return rolcatupdate;
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
pg_class_aclmask(Oid table_oid, Oid roleid,
				 AclMode mask, AclMaskHow how)
{
	AclMode		result;
	HeapTuple	tuple;
	Form_pg_class classForm;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	Oid			ownerId;

	/*
	 * Must get the relation's tuple from pg_class
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
	 * pg_authid.rolcatupdate is set.	(This is to let superusers protect
	 * themselves from themselves.)  Also allow it if allowSystemTableMods.
	 *
	 * As of 7.4 we have some updatable system views; those shouldn't be
	 * protected in this way.  Assume the view rules can take care of
	 * themselves.
	 */
	if ((mask & (ACL_INSERT | ACL_UPDATE | ACL_DELETE)) &&
		IsSystemClass(classForm) &&
		classForm->relkind != RELKIND_VIEW &&
		!has_rolcatupdate(roleid) &&
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
	if (superuser_arg(roleid))
	{
#ifdef ACLDEBUG
		elog(DEBUG2, "OID %u is superuser, home free", roleid);
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

	result = aclmask(acl, roleid, ownerId, mask, how);

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
pg_database_aclmask(Oid db_oid, Oid roleid,
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
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return mask;

	/*
	 * Get the database's ACL from pg_database
	 *
	 * There's no syscache for pg_database, so must look the hard way
	 */
	pg_database = heap_open(DatabaseRelationId, AccessShareLock);
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

	result = aclmask(acl, roleid, ownerId, mask, how);

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
pg_proc_aclmask(Oid proc_oid, Oid roleid,
				AclMode mask, AclMaskHow how)
{
	AclMode		result;
	HeapTuple	tuple;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
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

	result = aclmask(acl, roleid, ownerId, mask, how);

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
pg_language_aclmask(Oid lang_oid, Oid roleid,
					AclMode mask, AclMaskHow how)
{
	AclMode		result;
	HeapTuple	tuple;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
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
	ownerId = BOOTSTRAP_SUPERUSERID;

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

	result = aclmask(acl, roleid, ownerId, mask, how);

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
pg_namespace_aclmask(Oid nsp_oid, Oid roleid,
					 AclMode mask, AclMaskHow how)
{
	AclMode		result;
	HeapTuple	tuple;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return mask;

	/*
	 * If we have been assigned this namespace as a temp namespace, check to
	 * make sure we have CREATE TEMP permission on the database, and if so act
	 * as though we have all standard (but not GRANT OPTION) permissions on
	 * the namespace.  If we don't have CREATE TEMP, act as though we have
	 * only USAGE (and not CREATE) rights.
	 *
	 * This may seem redundant given the check in InitTempTableNamespace, but
	 * it really isn't since current user ID may have changed since then. The
	 * upshot of this behavior is that a SECURITY DEFINER function can create
	 * temp tables that can then be accessed (if permission is granted) by
	 * code in the same session that doesn't have permissions to create temp
	 * tables.
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

	result = aclmask(acl, roleid, ownerId, mask, how);

	/* if we have a detoasted copy, free it */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	ReleaseSysCache(tuple);

	return result;
}

/*
 * Exported routine for examining a user's privileges for a tablespace
 */
AclMode
pg_tablespace_aclmask(Oid spc_oid, Oid roleid,
					  AclMode mask, AclMaskHow how)
{
	AclMode		result;
	Relation	pg_tablespace;
	ScanKeyData entry[1];
	HeapScanDesc scan;
	HeapTuple	tuple;
	Datum		aclDatum;
	bool		isNull;
	Acl		   *acl;
	Oid			ownerId;

	/*
	 * Only shared relations can be stored in global space; don't let even
	 * superusers override this
	 */
	if (spc_oid == GLOBALTABLESPACE_OID && !IsBootstrapProcessingMode())
		return 0;

	/* Otherwise, superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return mask;

	/*
	 * Get the tablespace's ACL from pg_tablespace
	 *
	 * There's no syscache for pg_tablespace, so must look the hard way
	 */
	pg_tablespace = heap_open(TableSpaceRelationId, AccessShareLock);
	ScanKeyInit(&entry[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(spc_oid));
	scan = heap_beginscan(pg_tablespace, SnapshotNow, 1, entry);
	tuple = heap_getnext(scan, ForwardScanDirection);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace with OID %u does not exist", spc_oid)));

	ownerId = ((Form_pg_tablespace) GETSTRUCT(tuple))->spcowner;

	aclDatum = heap_getattr(tuple, Anum_pg_tablespace_spcacl,
							RelationGetDescr(pg_tablespace), &isNull);

	if (isNull)
	{
		/* No ACL, so build default ACL */
		acl = acldefault(ACL_OBJECT_TABLESPACE, ownerId);
		aclDatum = (Datum) 0;
	}
	else
	{
		/* detoast ACL if necessary */
		acl = DatumGetAclP(aclDatum);
	}

	result = aclmask(acl, roleid, ownerId, mask, how);

	/* if we have a detoasted copy, free it */
	if (acl && (Pointer) acl != DatumGetPointer(aclDatum))
		pfree(acl);

	heap_endscan(scan);
	heap_close(pg_tablespace, AccessShareLock);

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
pg_class_aclcheck(Oid table_oid, Oid roleid, AclMode mode)
{
	if (pg_class_aclmask(table_oid, roleid, mode, ACLMASK_ANY) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
}

/*
 * Exported routine for checking a user's access privileges to a database
 */
AclResult
pg_database_aclcheck(Oid db_oid, Oid roleid, AclMode mode)
{
	if (pg_database_aclmask(db_oid, roleid, mode, ACLMASK_ANY) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
}

/*
 * Exported routine for checking a user's access privileges to a function
 */
AclResult
pg_proc_aclcheck(Oid proc_oid, Oid roleid, AclMode mode)
{
	if (pg_proc_aclmask(proc_oid, roleid, mode, ACLMASK_ANY) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
}

/*
 * Exported routine for checking a user's access privileges to a language
 */
AclResult
pg_language_aclcheck(Oid lang_oid, Oid roleid, AclMode mode)
{
	if (pg_language_aclmask(lang_oid, roleid, mode, ACLMASK_ANY) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
}

/*
 * Exported routine for checking a user's access privileges to a namespace
 */
AclResult
pg_namespace_aclcheck(Oid nsp_oid, Oid roleid, AclMode mode)
{
	if (pg_namespace_aclmask(nsp_oid, roleid, mode, ACLMASK_ANY) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
}

/*
 * Exported routine for checking a user's access privileges to a tablespace
 */
AclResult
pg_tablespace_aclcheck(Oid spc_oid, Oid roleid, AclMode mode)
{
	if (pg_tablespace_aclmask(spc_oid, roleid, mode, ACLMASK_ANY) != 0)
		return ACLCHECK_OK;
	else
		return ACLCHECK_NO_PRIV;
}


/*
 * Ownership check for a relation (specified by OID).
 */
bool
pg_class_ownercheck(Oid class_oid, Oid roleid)
{
	HeapTuple	tuple;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	tuple = SearchSysCache(RELOID,
						   ObjectIdGetDatum(class_oid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("relation with OID %u does not exist", class_oid)));

	ownerId = ((Form_pg_class) GETSTRUCT(tuple))->relowner;

	ReleaseSysCache(tuple);

	return has_privs_of_role(roleid, ownerId);
}

/*
 * Ownership check for a type (specified by OID).
 */
bool
pg_type_ownercheck(Oid type_oid, Oid roleid)
{
	HeapTuple	tuple;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	tuple = SearchSysCache(TYPEOID,
						   ObjectIdGetDatum(type_oid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type with OID %u does not exist", type_oid)));

	ownerId = ((Form_pg_type) GETSTRUCT(tuple))->typowner;

	ReleaseSysCache(tuple);

	return has_privs_of_role(roleid, ownerId);
}

/*
 * Ownership check for an operator (specified by OID).
 */
bool
pg_oper_ownercheck(Oid oper_oid, Oid roleid)
{
	HeapTuple	tuple;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	tuple = SearchSysCache(OPEROID,
						   ObjectIdGetDatum(oper_oid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("operator with OID %u does not exist", oper_oid)));

	ownerId = ((Form_pg_operator) GETSTRUCT(tuple))->oprowner;

	ReleaseSysCache(tuple);

	return has_privs_of_role(roleid, ownerId);
}

/*
 * Ownership check for a function (specified by OID).
 */
bool
pg_proc_ownercheck(Oid proc_oid, Oid roleid)
{
	HeapTuple	tuple;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	tuple = SearchSysCache(PROCOID,
						   ObjectIdGetDatum(proc_oid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function with OID %u does not exist", proc_oid)));

	ownerId = ((Form_pg_proc) GETSTRUCT(tuple))->proowner;

	ReleaseSysCache(tuple);

	return has_privs_of_role(roleid, ownerId);
}

/*
 * Ownership check for a namespace (specified by OID).
 */
bool
pg_namespace_ownercheck(Oid nsp_oid, Oid roleid)
{
	HeapTuple	tuple;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	tuple = SearchSysCache(NAMESPACEOID,
						   ObjectIdGetDatum(nsp_oid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("schema with OID %u does not exist", nsp_oid)));

	ownerId = ((Form_pg_namespace) GETSTRUCT(tuple))->nspowner;

	ReleaseSysCache(tuple);

	return has_privs_of_role(roleid, ownerId);
}

/*
 * Ownership check for a tablespace (specified by OID).
 */
bool
pg_tablespace_ownercheck(Oid spc_oid, Oid roleid)
{
	Relation	pg_tablespace;
	ScanKeyData entry[1];
	HeapScanDesc scan;
	HeapTuple	spctuple;
	Oid			spcowner;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	/* There's no syscache for pg_tablespace, so must look the hard way */
	pg_tablespace = heap_open(TableSpaceRelationId, AccessShareLock);
	ScanKeyInit(&entry[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(spc_oid));
	scan = heap_beginscan(pg_tablespace, SnapshotNow, 1, entry);

	spctuple = heap_getnext(scan, ForwardScanDirection);

	if (!HeapTupleIsValid(spctuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace with OID %u does not exist", spc_oid)));

	spcowner = ((Form_pg_tablespace) GETSTRUCT(spctuple))->spcowner;

	heap_endscan(scan);
	heap_close(pg_tablespace, AccessShareLock);

	return has_privs_of_role(roleid, spcowner);
}

/*
 * Ownership check for an operator class (specified by OID).
 */
bool
pg_opclass_ownercheck(Oid opc_oid, Oid roleid)
{
	HeapTuple	tuple;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	tuple = SearchSysCache(CLAOID,
						   ObjectIdGetDatum(opc_oid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("operator class with OID %u does not exist",
						opc_oid)));

	ownerId = ((Form_pg_opclass) GETSTRUCT(tuple))->opcowner;

	ReleaseSysCache(tuple);

	return has_privs_of_role(roleid, ownerId);
}

/*
 * Ownership check for a database (specified by OID).
 */
bool
pg_database_ownercheck(Oid db_oid, Oid roleid)
{
	Relation	pg_database;
	ScanKeyData entry[1];
	HeapScanDesc scan;
	HeapTuple	dbtuple;
	Oid			dba;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	/* There's no syscache for pg_database, so must look the hard way */
	pg_database = heap_open(DatabaseRelationId, AccessShareLock);
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

	return has_privs_of_role(roleid, dba);
}

/*
 * Ownership check for a conversion (specified by OID).
 */
bool
pg_conversion_ownercheck(Oid conv_oid, Oid roleid)
{
	HeapTuple	tuple;
	Oid			ownerId;

	/* Superusers bypass all permission checking. */
	if (superuser_arg(roleid))
		return true;

	tuple = SearchSysCache(CONOID,
						   ObjectIdGetDatum(conv_oid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("conversion with OID %u does not exist", conv_oid)));

	ownerId = ((Form_pg_conversion) GETSTRUCT(tuple))->conowner;

	ReleaseSysCache(tuple);

	return has_privs_of_role(roleid, ownerId);
}
