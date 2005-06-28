/*-------------------------------------------------------------------------
 *
 * user.c
 *	  Commands for manipulating roles (formerly called users).
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/backend/commands/user.c,v 1.152 2005/06/28 05:08:55 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/indexing.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "commands/user.h"
#include "libpq/crypt.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/flatfiles.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


extern bool Password_encryption;

static List *roleNamesToIds(List *memberNames);
static void AddRoleMems(const char *rolename, Oid roleid,
						List *memberNames, List *memberIds,
						Oid grantorId, bool admin_opt);
static void DelRoleMems(const char *rolename, Oid roleid,
						List *memberNames, List *memberIds,
						bool admin_opt);


/*
 * CREATE ROLE
 */
void
CreateRole(CreateRoleStmt *stmt)
{
	Relation	pg_authid_rel;
	TupleDesc	pg_authid_dsc;
	HeapTuple	tuple;
	Datum		new_record[Natts_pg_authid];
	char		new_record_nulls[Natts_pg_authid];
	Oid			roleid;
	ListCell   *item;
	ListCell   *option;
	char	   *password = NULL;		/* user password */
	bool		encrypt_password = Password_encryption; /* encrypt password? */
	char		encrypted_password[MD5_PASSWD_LEN + 1];
	bool		issuper = false;		/* Make the user a superuser? */
	bool		createrole = false;		/* Can this user create roles? */
	bool		createdb = false;		/* Can the user create databases? */
	bool		canlogin = false;		/* Can this user login? */
	List	   *roleElts = NIL;			/* roles the user is a member of */
	List	   *rolememElts = NIL;	/* roles which will be members of this role */
	char	   *validUntil = NULL;		/* The time the login is valid
										 * until */
	DefElem    *dpassword = NULL;
	DefElem    *dcreatedb = NULL;
	DefElem    *dcreaterole = NULL;
	DefElem    *dcanlogin = NULL;
	DefElem    *droleElts = NULL;
	DefElem    *drolememElts = NULL;
	DefElem    *dvalidUntil = NULL;

	/* Extract options from the statement node tree */
	foreach(option, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "password") == 0 ||
			strcmp(defel->defname, "encryptedPassword") == 0 ||
			strcmp(defel->defname, "unencryptedPassword") == 0)
		{
			if (dpassword)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dpassword = defel;
			if (strcmp(defel->defname, "encryptedPassword") == 0)
				encrypt_password = true;
			else if (strcmp(defel->defname, "unencryptedPassword") == 0)
				encrypt_password = false;
		}
		else if (strcmp(defel->defname, "sysid") == 0)
		{
			ereport(WARNING,
					(errmsg("SYSID can no longer be specified")));
		}
		else if (strcmp(defel->defname, "createrole") == 0)
		{
			if (dcreaterole)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dcreaterole = defel;
		}
		else if (strcmp(defel->defname, "createdb") == 0)
		{
			if (dcreatedb)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dcreatedb = defel;
		}
		else if (strcmp(defel->defname, "canlogin") == 0)
		{
			if (dcanlogin)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dcanlogin = defel;
		}
		else if (strcmp(defel->defname, "roleElts") == 0)
		{
			if (droleElts)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			droleElts = defel;
		}
		else if (strcmp(defel->defname, "rolememElts") == 0)
		{
			if (drolememElts)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			drolememElts = defel;
		}
		else if (strcmp(defel->defname, "validUntil") == 0)
		{
			if (dvalidUntil)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dvalidUntil = defel;
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	if (dcreatedb)
		createdb = intVal(dcreatedb->arg) != 0;
	if (dcreaterole)
	{
		createrole = intVal(dcreaterole->arg) != 0;
		/* XXX issuper is implied by createrole for now */
		issuper = createrole;
	}
	if (dcanlogin)
		canlogin = intVal(dcanlogin->arg) != 0;
	if (dvalidUntil)
		validUntil = strVal(dvalidUntil->arg);
	if (dpassword)
		password = strVal(dpassword->arg);
	if (droleElts)
		roleElts = (List *) droleElts->arg;
	if (drolememElts)
		rolememElts = (List *) drolememElts->arg;

	/* Check some permissions first */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create roles")));

	if (strcmp(stmt->role, "public") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("role name \"%s\" is reserved",
						stmt->role)));

	/*
	 * Check the pg_authid relation to be certain the role doesn't
	 * already exist.  Note we secure exclusive lock because
	 * we need to protect our eventual update of the flat auth file.
	 */
	pg_authid_rel = heap_open(AuthIdRelationId, ExclusiveLock);
	pg_authid_dsc = RelationGetDescr(pg_authid_rel);

	tuple = SearchSysCache(AUTHNAME,
						   PointerGetDatum(stmt->role),
						   0, 0, 0);
	if (HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("role \"%s\" already exists",
						stmt->role)));

	/*
	 * Build a tuple to insert
	 */
	MemSet(new_record, 0, sizeof(new_record));
	MemSet(new_record_nulls, ' ', sizeof(new_record_nulls));

	new_record[Anum_pg_authid_rolname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->role));

	new_record[Anum_pg_authid_rolsuper - 1] = BoolGetDatum(issuper);
	new_record[Anum_pg_authid_rolcreaterole - 1] = BoolGetDatum(createrole);
	new_record[Anum_pg_authid_rolcreatedb - 1] = BoolGetDatum(createdb);
	/* superuser gets catupdate right by default */
	new_record[Anum_pg_authid_rolcatupdate - 1] = BoolGetDatum(issuper);
	new_record[Anum_pg_authid_rolcanlogin - 1] = BoolGetDatum(canlogin);

	if (password)
	{
		if (!encrypt_password || isMD5(password))
			new_record[Anum_pg_authid_rolpassword - 1] =
				DirectFunctionCall1(textin, CStringGetDatum(password));
		else
		{
			if (!EncryptMD5(password, stmt->role, strlen(stmt->role),
							encrypted_password))
				elog(ERROR, "password encryption failed");
			new_record[Anum_pg_authid_rolpassword - 1] =
				DirectFunctionCall1(textin, CStringGetDatum(encrypted_password));
		}
	}
	else
		new_record_nulls[Anum_pg_authid_rolpassword - 1] = 'n';

	if (validUntil)
		new_record[Anum_pg_authid_rolvaliduntil - 1] =
			DirectFunctionCall3(timestamptz_in,
								CStringGetDatum(validUntil),
								ObjectIdGetDatum(InvalidOid),
								Int32GetDatum(-1));

	else
		new_record_nulls[Anum_pg_authid_rolvaliduntil - 1] = 'n';

	new_record_nulls[Anum_pg_authid_rolconfig - 1] = 'n';

	tuple = heap_formtuple(pg_authid_dsc, new_record, new_record_nulls);

	/*
	 * Insert new record in the pg_authid table
	 */
	roleid = simple_heap_insert(pg_authid_rel, tuple);
	Assert(OidIsValid(roleid));

	/* Update indexes */
	CatalogUpdateIndexes(pg_authid_rel, tuple);

	/*
	 * Add the new role to the specified existing roles.
	 */
	foreach(item, roleElts)
	{
		char   *oldrolename = strVal(lfirst(item));
		Oid		oldroleid = get_roleid_checked(oldrolename);

		AddRoleMems(oldrolename, oldroleid,
					list_make1(makeString(stmt->role)),
					list_make1_oid(roleid),
					GetUserId(), false);
	}

	/*
	 * Add the specified members to this new role.
	 */
	AddRoleMems(stmt->role, roleid,
				rolememElts, roleNamesToIds(rolememElts),
				GetUserId(), false);

	/*
	 * Now we can clean up; but keep lock until commit (to avoid possible
	 * deadlock when commit code tries to acquire lock).
	 */
	heap_close(pg_authid_rel, NoLock);

	/*
	 * Set flag to update flat auth file at commit.
	 */
	auth_file_update_needed();
}


/*
 * ALTER ROLE
 */
void
AlterRole(AlterRoleStmt *stmt)
{
	Datum		new_record[Natts_pg_authid];
	char		new_record_nulls[Natts_pg_authid];
	char		new_record_repl[Natts_pg_authid];
	Relation	pg_authid_rel;
	TupleDesc	pg_authid_dsc;
	HeapTuple	tuple,
				new_tuple;
	ListCell   *option;
	char	   *password = NULL;		/* user password */
	bool		encrypt_password = Password_encryption; /* encrypt password? */
	char		encrypted_password[MD5_PASSWD_LEN + 1];
	int			issuper = -1;			/* Make the user a superuser? */
	int			createrole = -1;		/* Can this user create roles? */
	int			createdb = -1;			/* Can the user create databases? */
	int			canlogin = -1;			/* Can this user login? */
	int			adminopt = 0;	/* Can this user grant this role to others? */
	List	   *rolememElts = NIL;	/* The roles which will be added/removed to this role */
	char	   *validUntil = NULL;		/* The time the login is valid
										 * until */
	DefElem    *dpassword = NULL;
	DefElem    *dcreatedb = NULL;
	DefElem    *dcreaterole = NULL;
	DefElem    *dcanlogin = NULL;
	DefElem    *dadminopt = NULL;
	DefElem    *dvalidUntil = NULL;
	DefElem    *drolememElts = NULL;
	Oid			roleid;

	/* Extract options from the statement node tree */
	foreach(option, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "password") == 0 ||
			strcmp(defel->defname, "encryptedPassword") == 0 ||
			strcmp(defel->defname, "unencryptedPassword") == 0)
		{
			if (dpassword)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dpassword = defel;
			if (strcmp(defel->defname, "encryptedPassword") == 0)
				encrypt_password = true;
			else if (strcmp(defel->defname, "unencryptedPassword") == 0)
				encrypt_password = false;
		}
		else if (strcmp(defel->defname, "createdb") == 0)
		{
			if (dcreatedb)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dcreatedb = defel;
		}
		else if (strcmp(defel->defname, "createrole") == 0)
		{
			if (dcreaterole)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dcreaterole = defel;
		}
		else if (strcmp(defel->defname, "canlogin") == 0)
		{
			if (dcanlogin)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dcanlogin = defel;
		}
		else if (strcmp(defel->defname, "adminopt") == 0)
		{
			if (dadminopt)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dadminopt = defel;
		}
		else if (strcmp(defel->defname, "validUntil") == 0)
		{
			if (dvalidUntil)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dvalidUntil = defel;
		}
		else if (strcmp(defel->defname, "rolememElts") == 0 && stmt->action != 0)
		{
			if (drolememElts)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			drolememElts = defel;
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	if (dcreatedb)
		createdb = intVal(dcreatedb->arg);
	if (dcreaterole)
	{
		createrole = intVal(dcreaterole->arg);
		/* XXX createrole implies issuper for now */
		issuper = createrole;
	}
	if (dcanlogin)
		canlogin = intVal(dcanlogin->arg);
	if (dadminopt)
		adminopt = intVal(dadminopt->arg);
	if (dvalidUntil)
		validUntil = strVal(dvalidUntil->arg);
	if (dpassword)
		password = strVal(dpassword->arg);
	if (drolememElts)
		rolememElts = (List *) drolememElts->arg;

	/* must be superuser or just want to change your own password */
	if (!superuser() &&
		!(issuper < 0 &&
		  createrole < 0 &&
		  createdb < 0 &&
		  canlogin < 0 &&
		  !validUntil &&
		  !rolememElts &&
		  !adminopt &&
		  password &&
		  strcmp(GetUserNameFromId(GetUserId()), stmt->role) == 0))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied")));

	/*
	 * Scan the pg_authid relation to be certain the user exists. Note we
	 * secure exclusive lock to protect our update of the flat auth file.
	 */
	pg_authid_rel = heap_open(AuthIdRelationId, ExclusiveLock);
	pg_authid_dsc = RelationGetDescr(pg_authid_rel);

	tuple = SearchSysCache(AUTHNAME,
						   PointerGetDatum(stmt->role),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("role \"%s\" does not exist", stmt->role)));

	roleid = HeapTupleGetOid(tuple);

	/*
	 * Build an updated tuple, perusing the information just obtained
	 */
	MemSet(new_record, 0, sizeof(new_record));
	MemSet(new_record_nulls, ' ', sizeof(new_record_nulls));
	MemSet(new_record_repl, ' ', sizeof(new_record_repl));

	new_record[Anum_pg_authid_rolname - 1] = DirectFunctionCall1(namein,
											CStringGetDatum(stmt->role));
	new_record_repl[Anum_pg_authid_rolname - 1] = 'r';

	/*
	 * issuper/createrole/catupdate/etc
	 *
	 * XXX It's rather unclear how to handle catupdate.  It's probably best to
	 * keep it equal to the superuser status, otherwise you could end up
	 * with a situation where no existing superuser can alter the
	 * catalogs, including pg_authid!
	 */
	if (issuper >= 0)
	{
		new_record[Anum_pg_authid_rolsuper - 1] = BoolGetDatum(issuper > 0);
		new_record_repl[Anum_pg_authid_rolsuper - 1] = 'r';

		new_record[Anum_pg_authid_rolcatupdate - 1] = BoolGetDatum(issuper > 0);
		new_record_repl[Anum_pg_authid_rolcatupdate - 1] = 'r';
	}

	if (createrole >= 0)
	{
		new_record[Anum_pg_authid_rolcreaterole - 1] = BoolGetDatum(createrole > 0);
		new_record_repl[Anum_pg_authid_rolcreaterole - 1] = 'r';
	}

	if (createdb >= 0)
	{
		new_record[Anum_pg_authid_rolcreatedb - 1] = BoolGetDatum(createdb > 0);
		new_record_repl[Anum_pg_authid_rolcreatedb - 1] = 'r';
	}

	if (canlogin >= 0)
	{
		new_record[Anum_pg_authid_rolcanlogin - 1] = BoolGetDatum(canlogin > 0);
		new_record_repl[Anum_pg_authid_rolcanlogin - 1] = 'r';
	}

	/* password */
	if (password)
	{
		if (!encrypt_password || isMD5(password))
			new_record[Anum_pg_authid_rolpassword - 1] =
				DirectFunctionCall1(textin, CStringGetDatum(password));
		else
		{
			if (!EncryptMD5(password, stmt->role, strlen(stmt->role),
							encrypted_password))
				elog(ERROR, "password encryption failed");
			new_record[Anum_pg_authid_rolpassword - 1] =
				DirectFunctionCall1(textin, CStringGetDatum(encrypted_password));
		}
		new_record_repl[Anum_pg_authid_rolpassword - 1] = 'r';
	}

	/* valid until */
	if (validUntil)
	{
		new_record[Anum_pg_authid_rolvaliduntil - 1] =
			DirectFunctionCall3(timestamptz_in,
								CStringGetDatum(validUntil),
								ObjectIdGetDatum(InvalidOid),
								Int32GetDatum(-1));
		new_record_repl[Anum_pg_authid_rolvaliduntil - 1] = 'r';
	}

	new_tuple = heap_modifytuple(tuple, pg_authid_dsc, new_record,
								 new_record_nulls, new_record_repl);
	simple_heap_update(pg_authid_rel, &tuple->t_self, new_tuple);

	/* Update indexes */
	CatalogUpdateIndexes(pg_authid_rel, new_tuple);

	ReleaseSysCache(tuple);
	heap_freetuple(new_tuple);

	/*
	 * Now we can clean up; but keep lock until commit (to avoid possible
	 * deadlock when commit code tries to acquire lock).
	 */
	heap_close(pg_authid_rel, NoLock);

	if (stmt->action == +1)		/* add members to role */
		AddRoleMems(stmt->role, roleid,
					rolememElts, roleNamesToIds(rolememElts),
					GetUserId(), adminopt);
	else if (stmt->action == -1)	/* drop members from role */
		DelRoleMems(stmt->role, roleid,
					rolememElts, roleNamesToIds(rolememElts),
					adminopt);

	/*
	 * Set flag to update flat auth file at commit.
	 */
	auth_file_update_needed();
}


/*
 * ALTER ROLE ... SET
 */
void
AlterRoleSet(AlterRoleSetStmt *stmt)
{
	char	   *valuestr;
	HeapTuple	oldtuple,
				newtuple;
	Relation	rel;
	Datum		repl_val[Natts_pg_authid];
	char		repl_null[Natts_pg_authid];
	char		repl_repl[Natts_pg_authid];
	int			i;

	valuestr = flatten_set_variable_args(stmt->variable, stmt->value);

	/*
	 * RowExclusiveLock is sufficient, because we don't need to update the
	 * flat auth file.
	 */
	rel = heap_open(AuthIdRelationId, RowExclusiveLock);
	oldtuple = SearchSysCache(AUTHNAME,
							  PointerGetDatum(stmt->role),
							  0, 0, 0);
	if (!HeapTupleIsValid(oldtuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("role \"%s\" does not exist", stmt->role)));

	if (!(superuser() ||
		(HeapTupleGetOid(oldtuple) == GetUserId())))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied")));

	for (i = 0; i < Natts_pg_authid; i++)
		repl_repl[i] = ' ';

	repl_repl[Anum_pg_authid_rolconfig - 1] = 'r';
	if (strcmp(stmt->variable, "all") == 0 && valuestr == NULL)
	{
		/* RESET ALL */
		repl_null[Anum_pg_authid_rolconfig - 1] = 'n';
	}
	else
	{
		Datum		datum;
		bool		isnull;
		ArrayType  *array;

		repl_null[Anum_pg_authid_rolconfig - 1] = ' ';

		datum = SysCacheGetAttr(AUTHNAME, oldtuple,
								Anum_pg_authid_rolconfig, &isnull);

		array = isnull ? NULL : DatumGetArrayTypeP(datum);

		if (valuestr)
			array = GUCArrayAdd(array, stmt->variable, valuestr);
		else
			array = GUCArrayDelete(array, stmt->variable);

		if (array)
			repl_val[Anum_pg_authid_rolconfig - 1] = PointerGetDatum(array);
		else
			repl_null[Anum_pg_authid_rolconfig - 1] = 'n';
	}

	newtuple = heap_modifytuple(oldtuple, RelationGetDescr(rel), repl_val, repl_null, repl_repl);
	simple_heap_update(rel, &oldtuple->t_self, newtuple);

	CatalogUpdateIndexes(rel, newtuple);

	ReleaseSysCache(oldtuple);
	heap_close(rel, RowExclusiveLock);
}


/*
 * DROP ROLE
 */
void
DropRole(DropRoleStmt *stmt)
{
	Relation	pg_authid_rel, pg_auth_members_rel;
	ListCell   *item;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to drop roles")));

	/*
	 * Scan the pg_authid relation to find the Oid of the role to be
	 * deleted.  Note we secure exclusive lock, because we need to protect
	 * our update of the flat auth file.
	 */
	pg_authid_rel = heap_open(AuthIdRelationId, ExclusiveLock);
	pg_auth_members_rel = heap_open(AuthMemRelationId, ExclusiveLock);

	foreach(item, stmt->roles)
	{
		const char *role = strVal(lfirst(item));
		HeapTuple	tuple,
					tmp_tuple;
		Relation	pg_rel;
		TupleDesc	pg_dsc;
		ScanKeyData scankey;
		HeapScanDesc scan;
		CatCList	*auth_mem_list;
		Oid			roleid;
		int			i;

		tuple = SearchSysCache(AUTHNAME,
							   PointerGetDatum(role),
							   0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("role \"%s\" does not exist", role)));

		roleid = HeapTupleGetOid(tuple);

		if (roleid == GetUserId())
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("current role cannot be dropped")));
		if (roleid == GetSessionUserId())
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("session role cannot be dropped")));

		/*
		 * Check if role still owns a database. If so, error out.
		 *
		 * (It used to be that this function would drop the database
		 * automatically. This is not only very dangerous for people that
		 * don't read the manual, it doesn't seem to be the behaviour one
		 * would expect either.) -- petere 2000/01/14)
		 */
		pg_rel = heap_open(DatabaseRelationId, AccessShareLock);
		pg_dsc = RelationGetDescr(pg_rel);

		ScanKeyInit(&scankey,
					Anum_pg_database_datdba,
					BTEqualStrategyNumber, F_OIDEQ,
					roleid);

		scan = heap_beginscan(pg_rel, SnapshotNow, 1, &scankey);

		if ((tmp_tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
		{
			char	   *dbname;

			dbname = NameStr(((Form_pg_database) GETSTRUCT(tmp_tuple))->datname);
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("role \"%s\" cannot be dropped", role),
				   errdetail("The role owns database \"%s\".", dbname)));
		}

		heap_endscan(scan);
		heap_close(pg_rel, AccessShareLock);

		/*
		 * Somehow we'd have to check for tables, views, etc. owned by the
		 * role as well, but those could be spread out over all sorts of
		 * databases which we don't have access to (easily).
		 */

		/*
		 * Remove the role from the pg_authid table
		 */
		simple_heap_delete(pg_authid_rel, &tuple->t_self);

		ReleaseSysCache(tuple);

		/*
		 * Remove role from roles
		 *
		 * scan pg_auth_members and remove tuples which have 
		 * roleid == member or roleid == role
		 */
		auth_mem_list = SearchSysCacheList(AUTHMEMROLEMEM, 1,
											ObjectIdGetDatum(roleid),
											0, 0, 0);

		for (i = 0; i < auth_mem_list->n_members; i++)
		{
			HeapTuple	authmemtup = &auth_mem_list->members[i]->tuple;
			simple_heap_delete(pg_auth_members_rel, &authmemtup->t_self);
		}
		ReleaseSysCacheList(auth_mem_list);

		auth_mem_list = SearchSysCacheList(AUTHMEMMEMROLE, 1,
											ObjectIdGetDatum(roleid),
											0, 0, 0);

		for (i = 0; i < auth_mem_list->n_members; i++)
		{
			HeapTuple	authmemtup = &auth_mem_list->members[i]->tuple;
			simple_heap_delete(pg_auth_members_rel, &authmemtup->t_self);
		}
		ReleaseSysCacheList(auth_mem_list);
	}

	/*
	 * Now we can clean up; but keep lock until commit (to avoid possible
	 * deadlock when commit code tries to acquire lock).
	 */
	heap_close(pg_auth_members_rel, NoLock);
	heap_close(pg_authid_rel, NoLock);

	/*
	 * Set flag to update flat auth file at commit.
	 */
	auth_file_update_needed();
}

/*
 * Rename role
 */
void
RenameRole(const char *oldname, const char *newname)
{
	HeapTuple	oldtuple,
				newtuple;
	TupleDesc	dsc;
	Relation	rel;
	Datum		datum;
	bool		isnull;
	Datum		repl_val[Natts_pg_authid];
	char		repl_null[Natts_pg_authid];
	char		repl_repl[Natts_pg_authid];
	int			i;
	Oid			roleid;

	/* ExclusiveLock because we need to update the password file */
	rel = heap_open(AuthIdRelationId, ExclusiveLock);
	dsc = RelationGetDescr(rel);

	oldtuple = SearchSysCache(AUTHNAME,
							  CStringGetDatum(oldname),
							  0, 0, 0);
	if (!HeapTupleIsValid(oldtuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("role \"%s\" does not exist", oldname)));

	/*
	 * XXX Client applications probably store the session user somewhere,
	 * so renaming it could cause confusion.  On the other hand, there may
	 * not be an actual problem besides a little confusion, so think about
	 * this and decide.
	 */

	roleid = HeapTupleGetOid(oldtuple);

	if (roleid == GetSessionUserId())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("session role may not be renamed")));

	/* make sure the new name doesn't exist */
	if (SearchSysCacheExists(AUTHNAME,
							 CStringGetDatum(newname),
							 0, 0, 0))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("role \"%s\" already exists", newname)));

	/* must be superuser */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to rename roles")));

	for (i = 0; i < Natts_pg_authid; i++)
		repl_repl[i] = ' ';

	repl_repl[Anum_pg_authid_rolname - 1] = 'r';
	repl_val[Anum_pg_authid_rolname - 1] = DirectFunctionCall1(namein,
											   CStringGetDatum(newname));
	repl_null[Anum_pg_authid_rolname - 1] = ' ';

	datum = heap_getattr(oldtuple, Anum_pg_authid_rolpassword, dsc, &isnull);

	if (!isnull && isMD5(DatumGetCString(DirectFunctionCall1(textout, datum))))
	{
		/* MD5 uses the username as salt, so just clear it on a rename */
		repl_repl[Anum_pg_authid_rolpassword - 1] = 'r';
		repl_null[Anum_pg_authid_rolpassword - 1] = 'n';

		ereport(NOTICE,
				(errmsg("MD5 password cleared because of role rename")));
	}

	newtuple = heap_modifytuple(oldtuple, dsc, repl_val, repl_null, repl_repl);
	simple_heap_update(rel, &oldtuple->t_self, newtuple);

	CatalogUpdateIndexes(rel, newtuple);

	ReleaseSysCache(oldtuple);
	heap_close(rel, NoLock);

	auth_file_update_needed();
}

/*
 * GrantRoleStmt
 *
 * Grant/Revoke roles to/from roles
 */
void
GrantRole(GrantRoleStmt *stmt)
{
	Oid			grantor;
	List	   *grantee_ids;
	ListCell   *item;

	if (stmt->grantor)
		grantor = get_roleid_checked(stmt->grantor);
	else
		grantor = GetUserId();

	grantee_ids = roleNamesToIds(stmt->grantee_roles);

	/*
	 * Step through all of the granted roles and add/remove
	 * entries for the grantees, or, if admin_opt is set, then
	 * just add/remove the admin option.
	 *
	 * Note: Permissions checking is done by AddRoleMems/DelRoleMems
	 */
	foreach(item, stmt->granted_roles)
	{
		char   *rolename = strVal(lfirst(item));
		Oid		roleid = get_roleid_checked(rolename);

		if (stmt->is_grant)
			AddRoleMems(rolename, roleid,
						stmt->grantee_roles, grantee_ids,
						grantor, stmt->admin_opt);
		else
			DelRoleMems(rolename, roleid,
						stmt->grantee_roles, grantee_ids,
						stmt->admin_opt);
	}
}

/*
 * roleNamesToIds
 *
 * Given a list of role names (as String nodes), generate a list of role OIDs
 * in the same order.
 */
static List *
roleNamesToIds(List *memberNames)
{
	List	   *result = NIL;
	ListCell   *l;

	foreach(l, memberNames)
	{
		char   *rolename = strVal(lfirst(l));
		Oid		roleid = get_roleid_checked(rolename);

		result = lappend_oid(result, roleid);
	}
	return result;
}

/*
 * AddRoleMems -- Add given members to the specified role
 *
 * rolename: name of role to add to (used only for error messages)
 * roleid: OID of role to add to
 * memberNames: list of names of roles to add (used only for error messages)
 * memberIds: OIDs of roles to add
 * grantorId: who is granting the membership
 * admin_opt: granting admin option?
 */
static void
AddRoleMems(const char *rolename, Oid roleid,
			List *memberNames, List *memberIds,
			Oid grantorId, bool admin_opt)
{
	Relation	pg_authmem_rel;
	TupleDesc	pg_authmem_dsc;
	ListCell	*nameitem;
	ListCell	*iditem;

	Assert(list_length(memberNames) == list_length(memberIds));

	/* Skip permission check if nothing to do */
	if (!memberIds)
		return;

	/*
	 * Check permissions: must be superuser or have admin option on the
	 * role to be changed.
	 *
	 * XXX: The admin option is not considered to be inherited through
	 * multiple roles, unlike normal 'is_member_of_role' privilege checks.
	 */
	if (!superuser()) 
	{
		HeapTuple authmem_chk_tuple;
		Form_pg_auth_members authmem_chk;

		if (grantorId != GetUserId())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to set grantor ID")));

		authmem_chk_tuple = SearchSysCache(AUTHMEMROLEMEM,
										   ObjectIdGetDatum(roleid),
										   ObjectIdGetDatum(grantorId),
										   0, 0);
		if (!HeapTupleIsValid(authmem_chk_tuple))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser or have admin option on role \"%s\"",
							rolename)));

		authmem_chk = (Form_pg_auth_members) GETSTRUCT(authmem_chk_tuple);
		if (!authmem_chk->admin_option)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser or have admin option on role \"%s\"",
							rolename)));
		ReleaseSysCache(authmem_chk_tuple);
	}

	/*
	 * Secure exclusive lock to protect our update of the flat auth file.
	 */
	pg_authmem_rel = heap_open(AuthMemRelationId, ExclusiveLock);
	pg_authmem_dsc = RelationGetDescr(pg_authmem_rel);

	forboth(nameitem, memberNames, iditem, memberIds)
	{
		const char *membername = strVal(lfirst(nameitem));
		Oid			memberid = lfirst_oid(iditem);
		HeapTuple	authmem_tuple;
		HeapTuple	tuple;
		Datum   new_record[Natts_pg_auth_members];
		char    new_record_nulls[Natts_pg_auth_members];
		char    new_record_repl[Natts_pg_auth_members];

		/*
		 * Check if entry for this role/member already exists;
		 * if so, give warning unless we are adding admin option.
		 */
		authmem_tuple = SearchSysCache(AUTHMEMROLEMEM,
									   ObjectIdGetDatum(roleid),
									   ObjectIdGetDatum(memberid),
									   0, 0);
		if (HeapTupleIsValid(authmem_tuple) && !admin_opt)
		{
			ereport(NOTICE,
					(errmsg("role \"%s\" is already a member of role \"%s\"",
							membername, rolename)));
			ReleaseSysCache(authmem_tuple);
			continue;
		}

		/* Build a tuple to insert or update */
		MemSet(new_record, 0, sizeof(new_record));
		MemSet(new_record_nulls, ' ', sizeof(new_record_nulls));
		MemSet(new_record_repl, ' ', sizeof(new_record_repl));

		new_record[Anum_pg_auth_members_roleid - 1] = ObjectIdGetDatum(roleid);
		new_record[Anum_pg_auth_members_member - 1] = ObjectIdGetDatum(memberid);
		new_record[Anum_pg_auth_members_grantor - 1] = ObjectIdGetDatum(grantorId);
		new_record[Anum_pg_auth_members_admin_option - 1] = BoolGetDatum(admin_opt);

		if (HeapTupleIsValid(authmem_tuple))
		{
			new_record_repl[Anum_pg_auth_members_grantor - 1] = 'r';
			new_record_repl[Anum_pg_auth_members_admin_option - 1] = 'r';
			tuple = heap_modifytuple(authmem_tuple, pg_authmem_dsc,
									 new_record,
									 new_record_nulls, new_record_repl);
			simple_heap_update(pg_authmem_rel, &tuple->t_self, tuple);
			CatalogUpdateIndexes(pg_authmem_rel, tuple);
			ReleaseSysCache(authmem_tuple);
		}
		else
		{
			tuple = heap_formtuple(pg_authmem_dsc,
								   new_record, new_record_nulls);
			simple_heap_insert(pg_authmem_rel, tuple);
			CatalogUpdateIndexes(pg_authmem_rel, tuple);
		}
	}

	/*
	 * Now we can clean up; but keep lock until commit (to avoid possible
	 * deadlock when commit code tries to acquire lock).
	 */
	heap_close(pg_authmem_rel, NoLock);
}

/*
 * DelRoleMems -- Remove given members from the specified role
 *
 * rolename: name of role to del from (used only for error messages)
 * roleid: OID of role to del from
 * memberNames: list of names of roles to del (used only for error messages)
 * memberIds: OIDs of roles to del
 * admin_opt: remove admin option only?
 */
static void
DelRoleMems(const char *rolename, Oid roleid,
			List *memberNames, List *memberIds,
			bool admin_opt)
{
	Relation	pg_authmem_rel;
	TupleDesc	pg_authmem_dsc;
	ListCell	*nameitem;
	ListCell	*iditem;

	Assert(list_length(memberNames) == list_length(memberIds));

	/* Skip permission check if nothing to do */
	if (!memberIds)
		return;

	/*
	 * Check permissions: must be superuser or have admin option on the
	 * role to be changed.
	 *
	 * XXX: The admin option is not considered to be inherited through
	 * multiple roles, unlike normal 'is_member_of_role' privilege checks.
	 */
	if (!superuser()) 
	{
		HeapTuple authmem_chk_tuple;
		Form_pg_auth_members authmem_chk;

		authmem_chk_tuple = SearchSysCache(AUTHMEMROLEMEM,
										   ObjectIdGetDatum(roleid),
										   ObjectIdGetDatum(GetUserId()),
										   0, 0);
		if (!HeapTupleIsValid(authmem_chk_tuple))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser or have admin option on role \"%s\"",
							rolename)));

		authmem_chk = (Form_pg_auth_members) GETSTRUCT(authmem_chk_tuple);
		if (!authmem_chk->admin_option)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser or have admin option on role \"%s\"",
							rolename)));
		ReleaseSysCache(authmem_chk_tuple);
	}

	/*
	 * Secure exclusive lock to protect our update of the flat auth file.
	 */
	pg_authmem_rel = heap_open(AuthMemRelationId, ExclusiveLock);
	pg_authmem_dsc = RelationGetDescr(pg_authmem_rel);

	forboth(nameitem, memberNames, iditem, memberIds)
	{
		const char *membername = strVal(lfirst(nameitem));
		Oid			memberid = lfirst_oid(iditem);
		HeapTuple	authmem_tuple;

		/*
		 * Find entry for this role/member
		 */
		authmem_tuple = SearchSysCache(AUTHMEMROLEMEM,
									   ObjectIdGetDatum(roleid),
									   ObjectIdGetDatum(memberid),
									   0, 0);
		if (!HeapTupleIsValid(authmem_tuple))
		{
			ereport(WARNING,
					(errmsg("role \"%s\" is not a member of role \"%s\"",
							membername, rolename)));
			continue;
		}

		if (!admin_opt)
		{
			/* Remove the entry altogether */
			simple_heap_delete(pg_authmem_rel, &authmem_tuple->t_self);
		}
		else
		{
			/* Just turn off the admin option */
			HeapTuple	tuple;
			Datum   new_record[Natts_pg_auth_members];
			char    new_record_nulls[Natts_pg_auth_members];
			char    new_record_repl[Natts_pg_auth_members];

			/* Build a tuple to update with */
			MemSet(new_record, 0, sizeof(new_record));
			MemSet(new_record_nulls, ' ', sizeof(new_record_nulls));
			MemSet(new_record_repl, ' ', sizeof(new_record_repl));

			new_record[Anum_pg_auth_members_admin_option - 1] = BoolGetDatum(false);
			new_record_repl[Anum_pg_auth_members_admin_option - 1] = 'r';

			tuple = heap_modifytuple(authmem_tuple, pg_authmem_dsc,
									 new_record,
									 new_record_nulls, new_record_repl);
			simple_heap_update(pg_authmem_rel, &tuple->t_self, tuple);
			CatalogUpdateIndexes(pg_authmem_rel, tuple);
		}

		ReleaseSysCache(authmem_tuple);
	}

	/*
	 * Now we can clean up; but keep lock until commit (to avoid possible
	 * deadlock when commit code tries to acquire lock).
	 */
	heap_close(pg_authmem_rel, NoLock);
}
