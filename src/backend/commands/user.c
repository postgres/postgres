/*-------------------------------------------------------------------------
 *
 * user.c
 *	  Commands for manipulating roles (formerly called users).
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/backend/commands/user.c,v 1.174 2006/10/04 00:29:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "commands/comment.h"
#include "commands/user.h"
#include "libpq/md5.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
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


/* Check if current user has createrole privileges */
static bool
have_createrole_privilege(void)
{
	bool		result = false;
	HeapTuple	utup;

	/* Superusers can always do everything */
	if (superuser())
		return true;

	utup = SearchSysCache(AUTHOID,
						  ObjectIdGetDatum(GetUserId()),
						  0, 0, 0);
	if (HeapTupleIsValid(utup))
	{
		result = ((Form_pg_authid) GETSTRUCT(utup))->rolcreaterole;
		ReleaseSysCache(utup);
	}
	return result;
}


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
	char	   *password = NULL;	/* user password */
	bool		encrypt_password = Password_encryption; /* encrypt password? */
	char		encrypted_password[MD5_PASSWD_LEN + 1];
	bool		issuper = false;	/* Make the user a superuser? */
	bool		inherit = true; /* Auto inherit privileges? */
	bool		createrole = false;		/* Can this user create roles? */
	bool		createdb = false;		/* Can the user create databases? */
	bool		canlogin = false;		/* Can this user login? */
	int			connlimit = -1; /* maximum connections allowed */
	List	   *addroleto = NIL;	/* roles to make this a member of */
	List	   *rolemembers = NIL;		/* roles to be members of this role */
	List	   *adminmembers = NIL;		/* roles to be admins of this role */
	char	   *validUntil = NULL;		/* time the login is valid until */
	DefElem    *dpassword = NULL;
	DefElem    *dissuper = NULL;
	DefElem    *dinherit = NULL;
	DefElem    *dcreaterole = NULL;
	DefElem    *dcreatedb = NULL;
	DefElem    *dcanlogin = NULL;
	DefElem    *dconnlimit = NULL;
	DefElem    *daddroleto = NULL;
	DefElem    *drolemembers = NULL;
	DefElem    *dadminmembers = NULL;
	DefElem    *dvalidUntil = NULL;

	/* The defaults can vary depending on the original statement type */
	switch (stmt->stmt_type)
	{
		case ROLESTMT_ROLE:
			break;
		case ROLESTMT_USER:
			canlogin = true;
			/* may eventually want inherit to default to false here */
			break;
		case ROLESTMT_GROUP:
			break;
	}

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
			ereport(NOTICE,
					(errmsg("SYSID can no longer be specified")));
		}
		else if (strcmp(defel->defname, "superuser") == 0)
		{
			if (dissuper)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dissuper = defel;
		}
		else if (strcmp(defel->defname, "inherit") == 0)
		{
			if (dinherit)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dinherit = defel;
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
		else if (strcmp(defel->defname, "connectionlimit") == 0)
		{
			if (dconnlimit)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dconnlimit = defel;
		}
		else if (strcmp(defel->defname, "addroleto") == 0)
		{
			if (daddroleto)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			daddroleto = defel;
		}
		else if (strcmp(defel->defname, "rolemembers") == 0)
		{
			if (drolemembers)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			drolemembers = defel;
		}
		else if (strcmp(defel->defname, "adminmembers") == 0)
		{
			if (dadminmembers)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dadminmembers = defel;
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

	if (dpassword && dpassword->arg)
		password = strVal(dpassword->arg);
	if (dissuper)
		issuper = intVal(dissuper->arg) != 0;
	if (dinherit)
		inherit = intVal(dinherit->arg) != 0;
	if (dcreaterole)
		createrole = intVal(dcreaterole->arg) != 0;
	if (dcreatedb)
		createdb = intVal(dcreatedb->arg) != 0;
	if (dcanlogin)
		canlogin = intVal(dcanlogin->arg) != 0;
	if (dconnlimit)
		connlimit = intVal(dconnlimit->arg);
	if (daddroleto)
		addroleto = (List *) daddroleto->arg;
	if (drolemembers)
		rolemembers = (List *) drolemembers->arg;
	if (dadminmembers)
		adminmembers = (List *) dadminmembers->arg;
	if (dvalidUntil)
		validUntil = strVal(dvalidUntil->arg);

	/* Check some permissions first */
	if (issuper)
	{
		if (!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to create superusers")));
	}
	else
	{
		if (!have_createrole_privilege())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to create role")));
	}

	if (strcmp(stmt->role, "public") == 0 ||
		strcmp(stmt->role, "none") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("role name \"%s\" is reserved",
						stmt->role)));

	/*
	 * Check the pg_authid relation to be certain the role doesn't already
	 * exist.
	 */
	pg_authid_rel = heap_open(AuthIdRelationId, RowExclusiveLock);
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
	new_record[Anum_pg_authid_rolinherit - 1] = BoolGetDatum(inherit);
	new_record[Anum_pg_authid_rolcreaterole - 1] = BoolGetDatum(createrole);
	new_record[Anum_pg_authid_rolcreatedb - 1] = BoolGetDatum(createdb);
	/* superuser gets catupdate right by default */
	new_record[Anum_pg_authid_rolcatupdate - 1] = BoolGetDatum(issuper);
	new_record[Anum_pg_authid_rolcanlogin - 1] = BoolGetDatum(canlogin);
	new_record[Anum_pg_authid_rolconnlimit - 1] = Int32GetDatum(connlimit);

	if (password)
	{
		if (!encrypt_password || isMD5(password))
			new_record[Anum_pg_authid_rolpassword - 1] =
				DirectFunctionCall1(textin, CStringGetDatum(password));
		else
		{
			if (!pg_md5_encrypt(password, stmt->role, strlen(stmt->role),
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
	CatalogUpdateIndexes(pg_authid_rel, tuple);

	/*
	 * Advance command counter so we can see new record; else tests in
	 * AddRoleMems may fail.
	 */
	if (addroleto || adminmembers || rolemembers)
		CommandCounterIncrement();

	/*
	 * Add the new role to the specified existing roles.
	 */
	foreach(item, addroleto)
	{
		char	   *oldrolename = strVal(lfirst(item));
		Oid			oldroleid = get_roleid_checked(oldrolename);

		AddRoleMems(oldrolename, oldroleid,
					list_make1(makeString(stmt->role)),
					list_make1_oid(roleid),
					GetUserId(), false);
	}

	/*
	 * Add the specified members to this new role. adminmembers get the admin
	 * option, rolemembers don't.
	 */
	AddRoleMems(stmt->role, roleid,
				adminmembers, roleNamesToIds(adminmembers),
				GetUserId(), true);
	AddRoleMems(stmt->role, roleid,
				rolemembers, roleNamesToIds(rolemembers),
				GetUserId(), false);

	/*
	 * Close pg_authid, but keep lock till commit (this is important to
	 * prevent any risk of deadlock failure while updating flat file)
	 */
	heap_close(pg_authid_rel, NoLock);

	/*
	 * Set flag to update flat auth file at commit.
	 */
	auth_file_update_needed();
}


/*
 * ALTER ROLE
 *
 * Note: the rolemembers option accepted here is intended to support the
 * backwards-compatible ALTER GROUP syntax.  Although it will work to say
 * "ALTER ROLE role ROLE rolenames", we don't document it.
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
	char	   *password = NULL;	/* user password */
	bool		encrypt_password = Password_encryption; /* encrypt password? */
	char		encrypted_password[MD5_PASSWD_LEN + 1];
	int			issuper = -1;	/* Make the user a superuser? */
	int			inherit = -1;	/* Auto inherit privileges? */
	int			createrole = -1;	/* Can this user create roles? */
	int			createdb = -1;	/* Can the user create databases? */
	int			canlogin = -1;	/* Can this user login? */
	int			connlimit = -1; /* maximum connections allowed */
	List	   *rolemembers = NIL;		/* roles to be added/removed */
	char	   *validUntil = NULL;		/* time the login is valid until */
	DefElem    *dpassword = NULL;
	DefElem    *dissuper = NULL;
	DefElem    *dinherit = NULL;
	DefElem    *dcreaterole = NULL;
	DefElem    *dcreatedb = NULL;
	DefElem    *dcanlogin = NULL;
	DefElem    *dconnlimit = NULL;
	DefElem    *drolemembers = NULL;
	DefElem    *dvalidUntil = NULL;
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
		else if (strcmp(defel->defname, "superuser") == 0)
		{
			if (dissuper)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dissuper = defel;
		}
		else if (strcmp(defel->defname, "inherit") == 0)
		{
			if (dinherit)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dinherit = defel;
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
		else if (strcmp(defel->defname, "connectionlimit") == 0)
		{
			if (dconnlimit)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dconnlimit = defel;
		}
		else if (strcmp(defel->defname, "rolemembers") == 0 &&
				 stmt->action != 0)
		{
			if (drolemembers)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			drolemembers = defel;
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

	if (dpassword && dpassword->arg)
		password = strVal(dpassword->arg);
	if (dissuper)
		issuper = intVal(dissuper->arg);
	if (dinherit)
		inherit = intVal(dinherit->arg);
	if (dcreaterole)
		createrole = intVal(dcreaterole->arg);
	if (dcreatedb)
		createdb = intVal(dcreatedb->arg);
	if (dcanlogin)
		canlogin = intVal(dcanlogin->arg);
	if (dconnlimit)
		connlimit = intVal(dconnlimit->arg);
	if (drolemembers)
		rolemembers = (List *) drolemembers->arg;
	if (dvalidUntil)
		validUntil = strVal(dvalidUntil->arg);

	/*
	 * Scan the pg_authid relation to be certain the user exists.
	 */
	pg_authid_rel = heap_open(AuthIdRelationId, RowExclusiveLock);
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
	 * To mess with a superuser you gotta be superuser; else you need
	 * createrole, or just want to change your own password
	 */
	if (((Form_pg_authid) GETSTRUCT(tuple))->rolsuper || issuper >= 0)
	{
		if (!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to alter superusers")));
	}
	else if (!have_createrole_privilege())
	{
		if (!(inherit < 0 &&
			  createrole < 0 &&
			  createdb < 0 &&
			  canlogin < 0 &&
			  !dconnlimit &&
			  !rolemembers &&
			  !validUntil &&
			  dpassword &&
			  roleid == GetUserId()))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied")));
	}

	/*
	 * Build an updated tuple, perusing the information just obtained
	 */
	MemSet(new_record, 0, sizeof(new_record));
	MemSet(new_record_nulls, ' ', sizeof(new_record_nulls));
	MemSet(new_record_repl, ' ', sizeof(new_record_repl));

	/*
	 * issuper/createrole/catupdate/etc
	 *
	 * XXX It's rather unclear how to handle catupdate.  It's probably best to
	 * keep it equal to the superuser status, otherwise you could end up with
	 * a situation where no existing superuser can alter the catalogs,
	 * including pg_authid!
	 */
	if (issuper >= 0)
	{
		new_record[Anum_pg_authid_rolsuper - 1] = BoolGetDatum(issuper > 0);
		new_record_repl[Anum_pg_authid_rolsuper - 1] = 'r';

		new_record[Anum_pg_authid_rolcatupdate - 1] = BoolGetDatum(issuper > 0);
		new_record_repl[Anum_pg_authid_rolcatupdate - 1] = 'r';
	}

	if (inherit >= 0)
	{
		new_record[Anum_pg_authid_rolinherit - 1] = BoolGetDatum(inherit > 0);
		new_record_repl[Anum_pg_authid_rolinherit - 1] = 'r';
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

	if (dconnlimit)
	{
		new_record[Anum_pg_authid_rolconnlimit - 1] = Int32GetDatum(connlimit);
		new_record_repl[Anum_pg_authid_rolconnlimit - 1] = 'r';
	}

	/* password */
	if (password)
	{
		if (!encrypt_password || isMD5(password))
			new_record[Anum_pg_authid_rolpassword - 1] =
				DirectFunctionCall1(textin, CStringGetDatum(password));
		else
		{
			if (!pg_md5_encrypt(password, stmt->role, strlen(stmt->role),
								encrypted_password))
				elog(ERROR, "password encryption failed");
			new_record[Anum_pg_authid_rolpassword - 1] =
				DirectFunctionCall1(textin, CStringGetDatum(encrypted_password));
		}
		new_record_repl[Anum_pg_authid_rolpassword - 1] = 'r';
	}

	/* unset password */
	if (dpassword && dpassword->arg == NULL)
	{
		new_record_repl[Anum_pg_authid_rolpassword - 1] = 'r';
		new_record_nulls[Anum_pg_authid_rolpassword - 1] = 'n';
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
	 * Advance command counter so we can see new record; else tests in
	 * AddRoleMems may fail.
	 */
	if (rolemembers)
		CommandCounterIncrement();

	if (stmt->action == +1)		/* add members to role */
		AddRoleMems(stmt->role, roleid,
					rolemembers, roleNamesToIds(rolemembers),
					GetUserId(), false);
	else if (stmt->action == -1)	/* drop members from role */
		DelRoleMems(stmt->role, roleid,
					rolemembers, roleNamesToIds(rolemembers),
					false);

	/*
	 * Close pg_authid, but keep lock till commit (this is important to
	 * prevent any risk of deadlock failure while updating flat file)
	 */
	heap_close(pg_authid_rel, NoLock);

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

	rel = heap_open(AuthIdRelationId, RowExclusiveLock);
	oldtuple = SearchSysCache(AUTHNAME,
							  PointerGetDatum(stmt->role),
							  0, 0, 0);
	if (!HeapTupleIsValid(oldtuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("role \"%s\" does not exist", stmt->role)));

	/*
	 * To mess with a superuser you gotta be superuser; else you need
	 * createrole, or just want to change your own settings
	 */
	if (((Form_pg_authid) GETSTRUCT(oldtuple))->rolsuper)
	{
		if (!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to alter superusers")));
	}
	else
	{
		if (!have_createrole_privilege() &&
			HeapTupleGetOid(oldtuple) != GetUserId())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied")));
	}

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

	newtuple = heap_modifytuple(oldtuple, RelationGetDescr(rel),
								repl_val, repl_null, repl_repl);

	simple_heap_update(rel, &oldtuple->t_self, newtuple);
	CatalogUpdateIndexes(rel, newtuple);

	ReleaseSysCache(oldtuple);
	/* needn't keep lock since we won't be updating the flat file */
	heap_close(rel, RowExclusiveLock);
}


/*
 * DROP ROLE
 */
void
DropRole(DropRoleStmt *stmt)
{
	Relation	pg_authid_rel,
				pg_auth_members_rel;
	ListCell   *item;

	if (!have_createrole_privilege())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to drop role")));

	/*
	 * Scan the pg_authid relation to find the Oid of the role(s) to be
	 * deleted.
	 */
	pg_authid_rel = heap_open(AuthIdRelationId, RowExclusiveLock);
	pg_auth_members_rel = heap_open(AuthMemRelationId, RowExclusiveLock);

	foreach(item, stmt->roles)
	{
		const char *role = strVal(lfirst(item));
		HeapTuple	tuple,
					tmp_tuple;
		ScanKeyData scankey;
		char	   *detail;
		SysScanDesc sscan;
		Oid			roleid;

		tuple = SearchSysCache(AUTHNAME,
							   PointerGetDatum(role),
							   0, 0, 0);
		if (!HeapTupleIsValid(tuple))
		{
			if (!stmt->missing_ok)
			{
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("role \"%s\" does not exist", role)));
			}
			else
			{
				ereport(NOTICE,
						(errmsg("role \"%s\" does not exist, skipping",
								role)));
			}

			continue;
		}

		roleid = HeapTupleGetOid(tuple);

		if (roleid == GetUserId())
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("current user cannot be dropped")));
		if (roleid == GetOuterUserId())
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("current user cannot be dropped")));
		if (roleid == GetSessionUserId())
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("session user cannot be dropped")));

		/*
		 * For safety's sake, we allow createrole holders to drop ordinary
		 * roles but not superuser roles.  This is mainly to avoid the
		 * scenario where you accidentally drop the last superuser.
		 */
		if (((Form_pg_authid) GETSTRUCT(tuple))->rolsuper &&
			!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to drop superusers")));

		/*
		 * Lock the role, so nobody can add dependencies to her while we drop
		 * her.  We keep the lock until the end of transaction.
		 */
		LockSharedObject(AuthIdRelationId, roleid, 0, AccessExclusiveLock);

		/* Check for pg_shdepend entries depending on this role */
		if ((detail = checkSharedDependencies(AuthIdRelationId, roleid)) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
					 errmsg("role \"%s\" cannot be dropped because some objects depend on it",
							role),
					 errdetail("%s", detail)));

		/*
		 * Remove the role from the pg_authid table
		 */
		simple_heap_delete(pg_authid_rel, &tuple->t_self);

		ReleaseSysCache(tuple);

		/*
		 * Remove role from the pg_auth_members table.	We have to remove all
		 * tuples that show it as either a role or a member.
		 *
		 * XXX what about grantor entries?	Maybe we should do one heap scan.
		 */
		ScanKeyInit(&scankey,
					Anum_pg_auth_members_roleid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(roleid));

		sscan = systable_beginscan(pg_auth_members_rel, AuthMemRoleMemIndexId,
								   true, SnapshotNow, 1, &scankey);

		while (HeapTupleIsValid(tmp_tuple = systable_getnext(sscan)))
		{
			simple_heap_delete(pg_auth_members_rel, &tmp_tuple->t_self);
		}

		systable_endscan(sscan);

		ScanKeyInit(&scankey,
					Anum_pg_auth_members_member,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(roleid));

		sscan = systable_beginscan(pg_auth_members_rel, AuthMemMemRoleIndexId,
								   true, SnapshotNow, 1, &scankey);

		while (HeapTupleIsValid(tmp_tuple = systable_getnext(sscan)))
		{
			simple_heap_delete(pg_auth_members_rel, &tmp_tuple->t_self);
		}

		systable_endscan(sscan);

		/*
		 * Remove any comments on this role.
		 */
		DeleteSharedComments(roleid, AuthIdRelationId);

		/*
		 * Advance command counter so that later iterations of this loop will
		 * see the changes already made.  This is essential if, for example,
		 * we are trying to drop both a role and one of its direct members ---
		 * we'll get an error if we try to delete the linking pg_auth_members
		 * tuple twice.  (We do not need a CCI between the two delete loops
		 * above, because it's not allowed for a role to directly contain
		 * itself.)
		 */
		CommandCounterIncrement();
	}

	/*
	 * Now we can clean up; but keep locks until commit (to avoid possible
	 * deadlock failure while updating flat file)
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

	rel = heap_open(AuthIdRelationId, RowExclusiveLock);
	dsc = RelationGetDescr(rel);

	oldtuple = SearchSysCache(AUTHNAME,
							  CStringGetDatum(oldname),
							  0, 0, 0);
	if (!HeapTupleIsValid(oldtuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("role \"%s\" does not exist", oldname)));

	/*
	 * XXX Client applications probably store the session user somewhere, so
	 * renaming it could cause confusion.  On the other hand, there may not be
	 * an actual problem besides a little confusion, so think about this and
	 * decide.	Same for SET ROLE ... we don't restrict renaming the current
	 * effective userid, though.
	 */

	roleid = HeapTupleGetOid(oldtuple);

	if (roleid == GetSessionUserId())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("session user may not be renamed")));
	if (roleid == GetOuterUserId())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("current user may not be renamed")));

	/* make sure the new name doesn't exist */
	if (SearchSysCacheExists(AUTHNAME,
							 CStringGetDatum(newname),
							 0, 0, 0))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("role \"%s\" already exists", newname)));

	if (strcmp(newname, "public") == 0 ||
		strcmp(newname, "none") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("role name \"%s\" is reserved",
						newname)));

	/*
	 * createrole is enough privilege unless you want to mess with a superuser
	 */
	if (((Form_pg_authid) GETSTRUCT(oldtuple))->rolsuper)
	{
		if (!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to rename superusers")));
	}
	else
	{
		if (!have_createrole_privilege())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to rename role")));
	}

	/* OK, construct the modified tuple */
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

	/*
	 * Close pg_authid, but keep lock till commit (this is important to
	 * prevent any risk of deadlock failure while updating flat file)
	 */
	heap_close(rel, NoLock);

	/*
	 * Set flag to update flat auth file at commit.
	 */
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
	Relation	pg_authid_rel;
	Oid			grantor;
	List	   *grantee_ids;
	ListCell   *item;

	if (stmt->grantor)
		grantor = get_roleid_checked(stmt->grantor);
	else
		grantor = GetUserId();

	grantee_ids = roleNamesToIds(stmt->grantee_roles);

	/* AccessShareLock is enough since we aren't modifying pg_authid */
	pg_authid_rel = heap_open(AuthIdRelationId, AccessShareLock);

	/*
	 * Step through all of the granted roles and add/remove entries for the
	 * grantees, or, if admin_opt is set, then just add/remove the admin
	 * option.
	 *
	 * Note: Permissions checking is done by AddRoleMems/DelRoleMems
	 */
	foreach(item, stmt->granted_roles)
	{
		char	   *rolename = strVal(lfirst(item));
		Oid			roleid = get_roleid_checked(rolename);

		if (stmt->is_grant)
			AddRoleMems(rolename, roleid,
						stmt->grantee_roles, grantee_ids,
						grantor, stmt->admin_opt);
		else
			DelRoleMems(rolename, roleid,
						stmt->grantee_roles, grantee_ids,
						stmt->admin_opt);
	}

	/*
	 * Close pg_authid, but keep lock till commit (this is important to
	 * prevent any risk of deadlock failure while updating flat file)
	 */
	heap_close(pg_authid_rel, NoLock);

	/*
	 * Set flag to update flat auth file at commit.
	 */
	auth_file_update_needed();
}

/*
 * DropOwnedObjects
 *
 * Drop the objects owned by a given list of roles.
 */
void
DropOwnedObjects(DropOwnedStmt *stmt)
{
	List	   *role_ids = roleNamesToIds(stmt->roles);
	ListCell   *cell;

	/* Check privileges */
	foreach(cell, role_ids)
	{
		Oid			roleid = lfirst_oid(cell);

		if (!has_privs_of_role(GetUserId(), roleid))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to drop objects")));
	}

	/* Ok, do it */
	shdepDropOwned(role_ids, stmt->behavior);
}

/*
 * ReassignOwnedObjects
 *
 * Give the objects owned by a given list of roles away to another user.
 */
void
ReassignOwnedObjects(ReassignOwnedStmt *stmt)
{
	List	   *role_ids = roleNamesToIds(stmt->roles);
	ListCell   *cell;
	Oid			newrole;

	/* Check privileges */
	foreach(cell, role_ids)
	{
		Oid			roleid = lfirst_oid(cell);

		if (!has_privs_of_role(GetUserId(), roleid))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to reassign objects")));
	}

	/* Must have privileges on the receiving side too */
	newrole = get_roleid_checked(stmt->newrole);

	if (!has_privs_of_role(GetUserId(), newrole))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to reassign objects")));

	/* Ok, do it */
	shdepReassignOwned(role_ids, newrole);
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
		char	   *rolename = strVal(lfirst(l));
		Oid			roleid = get_roleid_checked(rolename);

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
 *
 * Note: caller is responsible for calling auth_file_update_needed().
 */
static void
AddRoleMems(const char *rolename, Oid roleid,
			List *memberNames, List *memberIds,
			Oid grantorId, bool admin_opt)
{
	Relation	pg_authmem_rel;
	TupleDesc	pg_authmem_dsc;
	ListCell   *nameitem;
	ListCell   *iditem;

	Assert(list_length(memberNames) == list_length(memberIds));

	/* Skip permission check if nothing to do */
	if (!memberIds)
		return;

	/*
	 * Check permissions: must have createrole or admin option on the role to
	 * be changed.	To mess with a superuser role, you gotta be superuser.
	 */
	if (superuser_arg(roleid))
	{
		if (!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to alter superusers")));
	}
	else
	{
		if (!have_createrole_privilege() &&
			!is_admin_of_role(grantorId, roleid))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must have admin option on role \"%s\"",
							rolename)));
	}

	/* XXX not sure about this check */
	if (grantorId != GetUserId() && !superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to set grantor")));

	pg_authmem_rel = heap_open(AuthMemRelationId, RowExclusiveLock);
	pg_authmem_dsc = RelationGetDescr(pg_authmem_rel);

	forboth(nameitem, memberNames, iditem, memberIds)
	{
		const char *membername = strVal(lfirst(nameitem));
		Oid			memberid = lfirst_oid(iditem);
		HeapTuple	authmem_tuple;
		HeapTuple	tuple;
		Datum		new_record[Natts_pg_auth_members];
		char		new_record_nulls[Natts_pg_auth_members];
		char		new_record_repl[Natts_pg_auth_members];

		/*
		 * Refuse creation of membership loops, including the trivial case
		 * where a role is made a member of itself.  We do this by checking to
		 * see if the target role is already a member of the proposed member
		 * role.  We have to ignore possible superuserness, however, else we
		 * could never grant membership in a superuser-privileged role.
		 */
		if (is_member_of_role_nosuper(roleid, memberid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_GRANT_OPERATION),
					 (errmsg("role \"%s\" is a member of role \"%s\"",
							 rolename, membername))));

		/*
		 * Check if entry for this role/member already exists; if so, give
		 * warning unless we are adding admin option.
		 */
		authmem_tuple = SearchSysCache(AUTHMEMROLEMEM,
									   ObjectIdGetDatum(roleid),
									   ObjectIdGetDatum(memberid),
									   0, 0);
		if (HeapTupleIsValid(authmem_tuple) &&
			(!admin_opt ||
			 ((Form_pg_auth_members) GETSTRUCT(authmem_tuple))->admin_option))
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

		/* CCI after each change, in case there are duplicates in list */
		CommandCounterIncrement();
	}

	/*
	 * Close pg_authmem, but keep lock till commit (this is important to
	 * prevent any risk of deadlock failure while updating flat file)
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
 *
 * Note: caller is responsible for calling auth_file_update_needed().
 */
static void
DelRoleMems(const char *rolename, Oid roleid,
			List *memberNames, List *memberIds,
			bool admin_opt)
{
	Relation	pg_authmem_rel;
	TupleDesc	pg_authmem_dsc;
	ListCell   *nameitem;
	ListCell   *iditem;

	Assert(list_length(memberNames) == list_length(memberIds));

	/* Skip permission check if nothing to do */
	if (!memberIds)
		return;

	/*
	 * Check permissions: must have createrole or admin option on the role to
	 * be changed.	To mess with a superuser role, you gotta be superuser.
	 */
	if (superuser_arg(roleid))
	{
		if (!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to alter superusers")));
	}
	else
	{
		if (!have_createrole_privilege() &&
			!is_admin_of_role(GetUserId(), roleid))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must have admin option on role \"%s\"",
							rolename)));
	}

	pg_authmem_rel = heap_open(AuthMemRelationId, RowExclusiveLock);
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
			Datum		new_record[Natts_pg_auth_members];
			char		new_record_nulls[Natts_pg_auth_members];
			char		new_record_repl[Natts_pg_auth_members];

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

		/* CCI after each change, in case there are duplicates in list */
		CommandCounterIncrement();
	}

	/*
	 * Close pg_authmem, but keep lock till commit (this is important to
	 * prevent any risk of deadlock failure while updating flat file)
	 */
	heap_close(pg_authmem_rel, NoLock);
}
