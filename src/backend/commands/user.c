/*-------------------------------------------------------------------------
 *
 * user.c
 *	  Commands for manipulating users and groups.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Header: /cvsroot/pgsql/src/backend/commands/user.c,v 1.90.2.1 2003/01/26 23:16:23 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/pg_database.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_group.h"
#include "catalog/indexing.h"
#include "commands/user.h"
#include "libpq/crypt.h"
#include "miscadmin.h"
#include "storage/pmsignal.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


extern bool Password_encryption;

static void CheckPgUserAclNotNull(void);

/*---------------------------------------------------------------------
 * write_password_file / update_pg_pwd
 *
 * copy the modified contents of pg_shadow to a file used by the postmaster
 * for user authentication.  The file is stored as $PGDATA/global/pg_pwd.
 *
 * This function set is both a trigger function for direct updates to pg_shadow
 * as well as being called directly from create/alter/drop user.
 *
 * We raise an error to force transaction rollback if we detect an illegal
 * username or password --- illegal being defined as values that would
 * mess up the pg_pwd parser.
 *---------------------------------------------------------------------
 */
static void
write_password_file(Relation rel)
{
	char	   *filename,
			   *tempname;
	int			bufsize;
	FILE	   *fp;
	mode_t		oumask;
	HeapScanDesc scan;
	HeapTuple	tuple;
	TupleDesc	dsc = RelationGetDescr(rel);

	/*
	 * Create a temporary filename to be renamed later.  This prevents the
	 * backend from clobbering the pg_pwd file while the postmaster might
	 * be reading from it.
	 */
	filename = crypt_getpwdfilename();
	bufsize = strlen(filename) + 12;
	tempname = (char *) palloc(bufsize);

	snprintf(tempname, bufsize, "%s.%d", filename, MyProcPid);
	oumask = umask((mode_t) 077);
	fp = AllocateFile(tempname, "w");
	umask(oumask);
	if (fp == NULL)
		elog(ERROR, "write_password_file: unable to write %s: %m", tempname);

	/* read table */
	scan = heap_beginscan(rel, false, SnapshotSelf, 0, NULL);
	while (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
	{
		Datum		datum_n,
					datum_p,
					datum_v;
		bool		null_n,
					null_p,
					null_v;
		char	   *str_n,
				   *str_p,
				   *str_v;
		int			i;

		datum_n = heap_getattr(tuple, Anum_pg_shadow_usename, dsc, &null_n);
		if (null_n)
			continue;			/* ignore NULL usernames */
		str_n = DatumGetCString(DirectFunctionCall1(nameout, datum_n));

		datum_p = heap_getattr(tuple, Anum_pg_shadow_passwd, dsc, &null_p);

		/*
		 * It can be argued that people having a null password shouldn't
		 * be allowed to connect under password authentication, because
		 * they need to have a password set up first. If you think
		 * assuming an empty password in that case is better, change this
		 * logic to look something like the code for valuntil.
		 */
		if (null_p)
		{
			pfree(str_n);
			continue;
		}
		str_p = DatumGetCString(DirectFunctionCall1(textout, datum_p));

		datum_v = heap_getattr(tuple, Anum_pg_shadow_valuntil, dsc, &null_v);
		if (null_v)
			str_v = pstrdup("\\N");
		else
			str_v = DatumGetCString(DirectFunctionCall1(nabstimeout, datum_v));

		/*
		 * Check for illegal characters in the username and password.
		 */
		i = strcspn(str_n, CRYPT_PWD_FILE_SEPSTR "\n");
		if (str_n[i] != '\0')
			elog(ERROR, "Invalid user name '%s'", str_n);
		i = strcspn(str_p, CRYPT_PWD_FILE_SEPSTR "\n");
		if (str_p[i] != '\0')
			elog(ERROR, "Invalid user password '%s'", str_p);

		/*
		 * The extra columns we emit here are not really necessary. To
		 * remove them, the parser in backend/libpq/crypt.c would need to
		 * be adjusted.
		 */
		fprintf(fp,
				"%s"
				CRYPT_PWD_FILE_SEPSTR
				"0"
				CRYPT_PWD_FILE_SEPSTR
				"x"
				CRYPT_PWD_FILE_SEPSTR
				"x"
				CRYPT_PWD_FILE_SEPSTR
				"x"
				CRYPT_PWD_FILE_SEPSTR
				"x"
				CRYPT_PWD_FILE_SEPSTR
				"%s"
				CRYPT_PWD_FILE_SEPSTR
				"%s\n",
				str_n,
				str_p,
				str_v);

		pfree(str_n);
		pfree(str_p);
		pfree(str_v);
	}
	heap_endscan(scan);

	fflush(fp);
	if (ferror(fp))
		elog(ERROR, "%s: %m", tempname);
	FreeFile(fp);

	/*
	 * Rename the temp file to its final name, deleting the old pg_pwd. We
	 * expect that rename(2) is an atomic action.
	 */
	if (rename(tempname, filename))
		elog(ERROR, "rename %s to %s: %m", tempname, filename);

	pfree((void *) tempname);
	pfree((void *) filename);

	/*
	 * Signal the postmaster to reload its password-file cache.
	 */
	SendPostmasterSignal(PMSIGNAL_PASSWORD_CHANGE);
}



/* This is the wrapper for triggers. */
Datum
update_pg_pwd(PG_FUNCTION_ARGS)
{
	/*
	 * ExclusiveLock ensures no one modifies pg_shadow while we read it,
	 * and that only one backend rewrites the flat file at a time.	It's
	 * OK to allow normal reads of pg_shadow in parallel, however.
	 */
	Relation	rel = heap_openr(ShadowRelationName, ExclusiveLock);

	write_password_file(rel);
	/* OK to release lock, since we did not modify the relation */
	heap_close(rel, ExclusiveLock);
	return PointerGetDatum(NULL);
}



/*
 * CREATE USER
 */
void
CreateUser(CreateUserStmt *stmt)
{
	Relation	pg_shadow_rel;
	TupleDesc	pg_shadow_dsc;
	HeapScanDesc scan;
	HeapTuple	tuple;
	Datum		new_record[Natts_pg_shadow];
	char		new_record_nulls[Natts_pg_shadow];
	bool		user_exists = false,
				sysid_exists = false,
				havesysid = false;
	int			max_id;
	List	   *item,
			   *option;
	char	   *password = NULL;	/* PostgreSQL user password */
	bool		encrypt_password = Password_encryption; /* encrypt password? */
	char		encrypted_password[MD5_PASSWD_LEN + 1];
	int			sysid = 0;		/* PgSQL system id (valid if havesysid) */
	bool		createdb = false;		/* Can the user create databases? */
	bool		createuser = false;		/* Can this user create users? */
	List	   *groupElts = NIL;	/* The groups the user is a member of */
	char	   *validUntil = NULL;		/* The time the login is valid
										 * until */
	DefElem    *dpassword = NULL;
	DefElem    *dsysid = NULL;
	DefElem    *dcreatedb = NULL;
	DefElem    *dcreateuser = NULL;
	DefElem    *dgroupElts = NULL;
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
				elog(ERROR, "CREATE USER: conflicting options");
			dpassword = defel;
			if (strcmp(defel->defname, "encryptedPassword") == 0)
				encrypt_password = true;
			else if (strcmp(defel->defname, "unencryptedPassword") == 0)
				encrypt_password = false;
		}
		else if (strcmp(defel->defname, "sysid") == 0)
		{
			if (dsysid)
				elog(ERROR, "CREATE USER: conflicting options");
			dsysid = defel;
		}
		else if (strcmp(defel->defname, "createdb") == 0)
		{
			if (dcreatedb)
				elog(ERROR, "CREATE USER: conflicting options");
			dcreatedb = defel;
		}
		else if (strcmp(defel->defname, "createuser") == 0)
		{
			if (dcreateuser)
				elog(ERROR, "CREATE USER: conflicting options");
			dcreateuser = defel;
		}
		else if (strcmp(defel->defname, "groupElts") == 0)
		{
			if (dgroupElts)
				elog(ERROR, "CREATE USER: conflicting options");
			dgroupElts = defel;
		}
		else if (strcmp(defel->defname, "validUntil") == 0)
		{
			if (dvalidUntil)
				elog(ERROR, "CREATE USER: conflicting options");
			dvalidUntil = defel;
		}
		else
			elog(ERROR, "CREATE USER: option \"%s\" not recognized",
				 defel->defname);
	}

	if (dcreatedb)
		createdb = intVal(dcreatedb->arg) != 0;
	if (dcreateuser)
		createuser = intVal(dcreateuser->arg) != 0;
	if (dsysid)
	{
		sysid = intVal(dsysid->arg);
		if (sysid <= 0)
			elog(ERROR, "user id must be positive");
		havesysid = true;
	}
	if (dvalidUntil)
		validUntil = strVal(dvalidUntil->arg);
	if (dpassword)
		password = strVal(dpassword->arg);
	if (dgroupElts)
		groupElts = (List *) dgroupElts->arg;

	/* Check some permissions first */
	if (password)
		CheckPgUserAclNotNull();

	if (!superuser())
		elog(ERROR, "CREATE USER: permission denied");

	/*
	 * Scan the pg_shadow relation to be certain the user or id doesn't
	 * already exist.  Note we secure exclusive lock, because we also need
	 * to be sure of what the next usesysid should be, and we need to
	 * protect our update of the flat password file.
	 */
	pg_shadow_rel = heap_openr(ShadowRelationName, ExclusiveLock);
	pg_shadow_dsc = RelationGetDescr(pg_shadow_rel);

	scan = heap_beginscan(pg_shadow_rel, false, SnapshotNow, 0, NULL);
	max_id = 99;				/* start auto-assigned ids at 100 */
	while (!user_exists && !sysid_exists &&
		   HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
	{
		Datum		datum;
		bool		null;

		datum = heap_getattr(tuple, Anum_pg_shadow_usename,
							 pg_shadow_dsc, &null);
		Assert(!null);
		user_exists = (strcmp((char *) DatumGetName(datum), stmt->user) == 0);

		datum = heap_getattr(tuple, Anum_pg_shadow_usesysid,
							 pg_shadow_dsc, &null);
		Assert(!null);
		if (havesysid)			/* customized id wanted */
			sysid_exists = (DatumGetInt32(datum) == sysid);
		else
		{
			/* pick 1 + max */
			if (DatumGetInt32(datum) > max_id)
				max_id = DatumGetInt32(datum);
		}
	}
	heap_endscan(scan);

	if (user_exists)
		elog(ERROR, "CREATE USER: user name \"%s\" already exists",
			 stmt->user);
	if (sysid_exists)
		elog(ERROR, "CREATE USER: sysid %d is already assigned", sysid);

	/* If no sysid given, use max existing id + 1 */
	if (!havesysid)
		sysid = max_id + 1;

	/*
	 * Build a tuple to insert
	 */
	new_record[Anum_pg_shadow_usename - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->user));
	new_record[Anum_pg_shadow_usesysid - 1] = Int32GetDatum(sysid);

	AssertState(BoolIsValid(createdb));
	new_record[Anum_pg_shadow_usecreatedb - 1] = BoolGetDatum(createdb);
	new_record[Anum_pg_shadow_usetrace - 1] = BoolGetDatum(false);
	AssertState(BoolIsValid(createuser));
	new_record[Anum_pg_shadow_usesuper - 1] = BoolGetDatum(createuser);
	/* superuser gets catupd right by default */
	new_record[Anum_pg_shadow_usecatupd - 1] = BoolGetDatum(createuser);

	if (password)
	{
		if (!encrypt_password || isMD5(password))
			new_record[Anum_pg_shadow_passwd - 1] =
				DirectFunctionCall1(textin, CStringGetDatum(password));
		else
		{
			if (!EncryptMD5(password, stmt->user, strlen(stmt->user),
							encrypted_password))
				elog(ERROR, "CREATE USER: password encryption failed");
			new_record[Anum_pg_shadow_passwd - 1] =
				DirectFunctionCall1(textin, CStringGetDatum(encrypted_password));
		}
	}
	if (validUntil)
		new_record[Anum_pg_shadow_valuntil - 1] =
			DirectFunctionCall1(nabstimein, CStringGetDatum(validUntil));

	new_record_nulls[Anum_pg_shadow_usename - 1] = ' ';
	new_record_nulls[Anum_pg_shadow_usesysid - 1] = ' ';

	new_record_nulls[Anum_pg_shadow_usecreatedb - 1] = ' ';
	new_record_nulls[Anum_pg_shadow_usetrace - 1] = ' ';
	new_record_nulls[Anum_pg_shadow_usesuper - 1] = ' ';
	new_record_nulls[Anum_pg_shadow_usecatupd - 1] = ' ';

	new_record_nulls[Anum_pg_shadow_passwd - 1] = password ? ' ' : 'n';
	new_record_nulls[Anum_pg_shadow_valuntil - 1] = validUntil ? ' ' : 'n';

	tuple = heap_formtuple(pg_shadow_dsc, new_record, new_record_nulls);

	/*
	 * Insert new record in the pg_shadow table
	 */
	heap_insert(pg_shadow_rel, tuple);

	/*
	 * Update indexes
	 */
	if (RelationGetForm(pg_shadow_rel)->relhasindex)
	{
		Relation	idescs[Num_pg_shadow_indices];

		CatalogOpenIndices(Num_pg_shadow_indices,
						   Name_pg_shadow_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_shadow_indices, pg_shadow_rel,
						   tuple);
		CatalogCloseIndices(Num_pg_shadow_indices, idescs);
	}

	/*
	 * Add the user to the groups specified. We'll just call the below
	 * AlterGroup for this.
	 */
	foreach(item, groupElts)
	{
		AlterGroupStmt ags;

		ags.name = strVal(lfirst(item));		/* the group name to add
												 * this in */
		ags.action = +1;
		ags.listUsers = makeList1(makeInteger(sysid));
		AlterGroup(&ags, "CREATE USER");
	}

	/*
	 * Write the updated pg_shadow data to the flat password file.
	 */
	write_password_file(pg_shadow_rel);

	/*
	 * Now we can clean up; but keep lock until commit.
	 */
	heap_close(pg_shadow_rel, NoLock);
}



/*
 * ALTER USER
 */
extern void
AlterUser(AlterUserStmt *stmt)
{
	Datum		new_record[Natts_pg_shadow];
	char		new_record_nulls[Natts_pg_shadow];
	Relation	pg_shadow_rel;
	TupleDesc	pg_shadow_dsc;
	HeapTuple	tuple,
				new_tuple;
	bool		null;
	List	   *option;
	char	   *password = NULL;	/* PostgreSQL user password */
	bool		encrypt_password = Password_encryption; /* encrypt password? */
	char		encrypted_password[MD5_PASSWD_LEN + 1];
	int			createdb = -1;	/* Can the user create databases? */
	int			createuser = -1;	/* Can this user create users? */
	char	   *validUntil = NULL;		/* The time the login is valid
										 * until */
	DefElem    *dpassword = NULL;
	DefElem    *dcreatedb = NULL;
	DefElem    *dcreateuser = NULL;
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
				elog(ERROR, "ALTER USER: conflicting options");
			dpassword = defel;
			if (strcmp(defel->defname, "encryptedPassword") == 0)
				encrypt_password = true;
			else if (strcmp(defel->defname, "unencryptedPassword") == 0)
				encrypt_password = false;
		}
		else if (strcmp(defel->defname, "createdb") == 0)
		{
			if (dcreatedb)
				elog(ERROR, "ALTER USER: conflicting options");
			dcreatedb = defel;
		}
		else if (strcmp(defel->defname, "createuser") == 0)
		{
			if (dcreateuser)
				elog(ERROR, "ALTER USER: conflicting options");
			dcreateuser = defel;
		}
		else if (strcmp(defel->defname, "validUntil") == 0)
		{
			if (dvalidUntil)
				elog(ERROR, "ALTER USER: conflicting options");
			dvalidUntil = defel;
		}
		else
			elog(ERROR, "ALTER USER: option \"%s\" not recognized",
				 defel->defname);
	}

	if (dcreatedb)
		createdb = intVal(dcreatedb->arg);
	if (dcreateuser)
		createuser = intVal(dcreateuser->arg);
	if (dvalidUntil)
		validUntil = strVal(dvalidUntil->arg);
	if (dpassword)
		password = strVal(dpassword->arg);

	if (password)
		CheckPgUserAclNotNull();

	/* must be superuser or just want to change your own password */
	if (!superuser() &&
		!(createdb < 0 &&
		  createuser < 0 &&
		  !validUntil &&
		  password &&
		  strcmp(GetUserName(GetUserId()), stmt->user) == 0))
		elog(ERROR, "ALTER USER: permission denied");

	/* changes to the flat password file cannot be rolled back */
	if (IsTransactionBlock() && password)
		elog(NOTICE, "ALTER USER: password changes cannot be rolled back");

	/*
	 * Scan the pg_shadow relation to be certain the user exists. Note we
	 * secure exclusive lock to protect our update of the flat password
	 * file.
	 */
	pg_shadow_rel = heap_openr(ShadowRelationName, ExclusiveLock);
	pg_shadow_dsc = RelationGetDescr(pg_shadow_rel);

	tuple = SearchSysCache(SHADOWNAME,
						   PointerGetDatum(stmt->user),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "ALTER USER: user \"%s\" does not exist", stmt->user);

	/*
	 * Build a tuple to update, perusing the information just obtained
	 */
	new_record[Anum_pg_shadow_usename - 1] = DirectFunctionCall1(namein,
											CStringGetDatum(stmt->user));
	new_record_nulls[Anum_pg_shadow_usename - 1] = ' ';

	/* sysid - leave as is */
	new_record[Anum_pg_shadow_usesysid - 1] = heap_getattr(tuple, Anum_pg_shadow_usesysid, pg_shadow_dsc, &null);
	new_record_nulls[Anum_pg_shadow_usesysid - 1] = null ? 'n' : ' ';

	/* createdb */
	if (createdb < 0)
	{
		/* don't change */
		new_record[Anum_pg_shadow_usecreatedb - 1] = heap_getattr(tuple, Anum_pg_shadow_usecreatedb, pg_shadow_dsc, &null);
		new_record_nulls[Anum_pg_shadow_usecreatedb - 1] = null ? 'n' : ' ';
	}
	else
	{
		new_record[Anum_pg_shadow_usecreatedb - 1] = BoolGetDatum(createdb > 0);
		new_record_nulls[Anum_pg_shadow_usecreatedb - 1] = ' ';
	}

	/* trace - leave as is */
	new_record[Anum_pg_shadow_usetrace - 1] = heap_getattr(tuple, Anum_pg_shadow_usetrace, pg_shadow_dsc, &null);
	new_record_nulls[Anum_pg_shadow_usetrace - 1] = null ? 'n' : ' ';

	/*
	 * createuser (superuser) and catupd
	 *
	 * XXX It's rather unclear how to handle catupd.  It's probably best to
	 * keep it equal to the superuser status, otherwise you could end up
	 * with a situation where no existing superuser can alter the
	 * catalogs, including pg_shadow!
	 */
	if (createuser < 0)
	{
		/* don't change */
		new_record[Anum_pg_shadow_usesuper - 1] = heap_getattr(tuple, Anum_pg_shadow_usesuper, pg_shadow_dsc, &null);
		new_record_nulls[Anum_pg_shadow_usesuper - 1] = null ? 'n' : ' ';

		new_record[Anum_pg_shadow_usecatupd - 1] = heap_getattr(tuple, Anum_pg_shadow_usecatupd, pg_shadow_dsc, &null);
		new_record_nulls[Anum_pg_shadow_usecatupd - 1] = null ? 'n' : ' ';
	}
	else
	{
		new_record[Anum_pg_shadow_usesuper - 1] = BoolGetDatum(createuser > 0);
		new_record_nulls[Anum_pg_shadow_usesuper - 1] = ' ';

		new_record[Anum_pg_shadow_usecatupd - 1] = BoolGetDatum(createuser > 0);
		new_record_nulls[Anum_pg_shadow_usecatupd - 1] = ' ';
	}

	/* password */
	if (password)
	{
		if (!encrypt_password || isMD5(password))
			new_record[Anum_pg_shadow_passwd - 1] =
				DirectFunctionCall1(textin, CStringGetDatum(password));
		else
		{
			if (!EncryptMD5(password, stmt->user, strlen(stmt->user),
							encrypted_password))
				elog(ERROR, "CREATE USER: password encryption failed");
			new_record[Anum_pg_shadow_passwd - 1] =
				DirectFunctionCall1(textin, CStringGetDatum(encrypted_password));
		}
		new_record_nulls[Anum_pg_shadow_passwd - 1] = ' ';
	}
	else
	{
		/* leave as is */
		new_record[Anum_pg_shadow_passwd - 1] =
			heap_getattr(tuple, Anum_pg_shadow_passwd, pg_shadow_dsc, &null);
		new_record_nulls[Anum_pg_shadow_passwd - 1] = null ? 'n' : ' ';
	}

	/* valid until */
	if (validUntil)
	{
		new_record[Anum_pg_shadow_valuntil - 1] =
			DirectFunctionCall1(nabstimein, CStringGetDatum(validUntil));
		new_record_nulls[Anum_pg_shadow_valuntil - 1] = ' ';
	}
	else
	{
		/* leave as is */
		new_record[Anum_pg_shadow_valuntil - 1] =
			heap_getattr(tuple, Anum_pg_shadow_valuntil, pg_shadow_dsc, &null);
		new_record_nulls[Anum_pg_shadow_valuntil - 1] = null ? 'n' : ' ';
	}

	new_tuple = heap_formtuple(pg_shadow_dsc, new_record, new_record_nulls);
	simple_heap_update(pg_shadow_rel, &tuple->t_self, new_tuple);

	/* Update indexes */
	if (RelationGetForm(pg_shadow_rel)->relhasindex)
	{
		Relation	idescs[Num_pg_shadow_indices];

		CatalogOpenIndices(Num_pg_shadow_indices,
						   Name_pg_shadow_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_shadow_indices, pg_shadow_rel,
						   new_tuple);
		CatalogCloseIndices(Num_pg_shadow_indices, idescs);
	}

	ReleaseSysCache(tuple);
	heap_freetuple(new_tuple);

	/*
	 * Write the updated pg_shadow data to the flat password file.
	 */
	write_password_file(pg_shadow_rel);

	/*
	 * Now we can clean up.
	 */
	heap_close(pg_shadow_rel, NoLock);
}



/*
 * DROP USER
 */
void
DropUser(DropUserStmt *stmt)
{
	Relation	pg_shadow_rel;
	TupleDesc	pg_shadow_dsc;
	List	   *item;

	if (!superuser())
		elog(ERROR, "DROP USER: permission denied");

	if (IsTransactionBlock())
		elog(NOTICE, "DROP USER cannot be rolled back completely");

	/*
	 * Scan the pg_shadow relation to find the usesysid of the user to be
	 * deleted.  Note we secure exclusive lock, because we need to protect
	 * our update of the flat password file.
	 */
	pg_shadow_rel = heap_openr(ShadowRelationName, ExclusiveLock);
	pg_shadow_dsc = RelationGetDescr(pg_shadow_rel);

	foreach(item, stmt->users)
	{
		HeapTuple	tuple,
					tmp_tuple;
		Relation	pg_rel;
		TupleDesc	pg_dsc;
		ScanKeyData scankey;
		HeapScanDesc scan;
		Datum		datum;
		bool		null;
		int32		usesysid;
		const char *user = strVal(lfirst(item));

		tuple = SearchSysCache(SHADOWNAME,
							   PointerGetDatum(user),
							   0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "DROP USER: user \"%s\" does not exist%s", user,
				 (length(stmt->users) > 1) ? " (no users removed)" : "");

		usesysid = DatumGetInt32(heap_getattr(tuple, Anum_pg_shadow_usesysid, pg_shadow_dsc, &null));

		if (usesysid == GetUserId())
			elog(ERROR, "current user cannot be dropped");
		if (usesysid == GetSessionUserId())
			elog(ERROR, "session user cannot be dropped");

		/*
		 * Check if user still owns a database. If so, error out.
		 *
		 * (It used to be that this function would drop the database
		 * automatically. This is not only very dangerous for people that
		 * don't read the manual, it doesn't seem to be the behaviour one
		 * would expect either.) -- petere 2000/01/14)
		 */
		pg_rel = heap_openr(DatabaseRelationName, AccessShareLock);
		pg_dsc = RelationGetDescr(pg_rel);

		ScanKeyEntryInitialize(&scankey, 0x0,
							   Anum_pg_database_datdba, F_INT4EQ,
							   Int32GetDatum(usesysid));

		scan = heap_beginscan(pg_rel, false, SnapshotNow, 1, &scankey);

		if (HeapTupleIsValid(tmp_tuple = heap_getnext(scan, 0)))
		{
			char	   *dbname;

			datum = heap_getattr(tmp_tuple, Anum_pg_database_datname,
								 pg_dsc, &null);
			Assert(!null);
			dbname = DatumGetCString(DirectFunctionCall1(nameout, datum));
			elog(ERROR, "DROP USER: user \"%s\" owns database \"%s\", cannot be removed%s",
				 user, dbname,
				 (length(stmt->users) > 1) ? " (no users removed)" : "");
		}

		heap_endscan(scan);
		heap_close(pg_rel, AccessShareLock);

		/*
		 * Somehow we'd have to check for tables, views, etc. owned by the
		 * user as well, but those could be spread out over all sorts of
		 * databases which we don't have access to (easily).
		 */

		/*
		 * Remove the user from the pg_shadow table
		 */
		simple_heap_delete(pg_shadow_rel, &tuple->t_self);

		ReleaseSysCache(tuple);

		/*
		 * Remove user from groups
		 *
		 * try calling alter group drop user for every group
		 */
		pg_rel = heap_openr(GroupRelationName, ExclusiveLock);
		pg_dsc = RelationGetDescr(pg_rel);
		scan = heap_beginscan(pg_rel, false, SnapshotNow, 0, NULL);
		while (HeapTupleIsValid(tmp_tuple = heap_getnext(scan, 0)))
		{
			AlterGroupStmt ags;

			/* the group name from which to try to drop the user: */
			datum = heap_getattr(tmp_tuple, Anum_pg_group_groname, pg_dsc, &null);
			ags.name = DatumGetCString(DirectFunctionCall1(nameout, datum));
			ags.action = -1;
			ags.listUsers = makeList1(makeInteger(usesysid));
			AlterGroup(&ags, "DROP USER");
		}
		heap_endscan(scan);
		heap_close(pg_rel, ExclusiveLock);

		/*
		 * Advance command counter so that later iterations of this loop
		 * will see the changes already made.  This is essential if, for
		 * example, we are trying to drop two users who are members of the
		 * same group --- the AlterGroup for the second user had better
		 * see the tuple updated from the first one.
		 */
		CommandCounterIncrement();
	}

	/*
	 * Write the updated pg_shadow data to the flat password file.
	 */
	write_password_file(pg_shadow_rel);

	/*
	 * Now we can clean up.
	 */
	heap_close(pg_shadow_rel, NoLock);
}



/*
 * CheckPgUserAclNotNull
 *
 * check to see if there is an ACL on pg_shadow
 */
static void
CheckPgUserAclNotNull()
{
	HeapTuple	htup;

	htup = SearchSysCache(RELNAME,
						  PointerGetDatum(ShadowRelationName),
						  0, 0, 0);
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "CheckPgUserAclNotNull: \"%s\" not found",
			 ShadowRelationName);

	if (heap_attisnull(htup, Anum_pg_class_relacl))
		elog(ERROR,
			 "To use passwords, you have to revoke permissions on %s "
			 "so normal users cannot read the passwords. "
			 "Try 'REVOKE ALL ON \"%s\" FROM PUBLIC'.",
			 ShadowRelationName, ShadowRelationName);

	ReleaseSysCache(htup);
}



/*
 * CREATE GROUP
 */
void
CreateGroup(CreateGroupStmt *stmt)
{
	Relation	pg_group_rel;
	HeapScanDesc scan;
	HeapTuple	tuple;
	TupleDesc	pg_group_dsc;
	bool		group_exists = false,
				sysid_exists = false,
				havesysid = false;
	int			max_id;
	Datum		new_record[Natts_pg_group];
	char		new_record_nulls[Natts_pg_group];
	List	   *item,
			   *option,
			   *newlist = NIL;
	ArrayType  *userarray;
	int			sysid = 0;
	List	   *userElts = NIL;
	DefElem    *dsysid = NULL;
	DefElem    *duserElts = NULL;

	foreach(option, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "sysid") == 0)
		{
			if (dsysid)
				elog(ERROR, "CREATE GROUP: conflicting options");
			dsysid = defel;
		}
		else if (strcmp(defel->defname, "userElts") == 0)
		{
			if (duserElts)
				elog(ERROR, "CREATE GROUP: conflicting options");
			duserElts = defel;
		}
		else
			elog(ERROR, "CREATE GROUP: option \"%s\" not recognized",
				 defel->defname);
	}

	if (dsysid)
	{
		sysid = intVal(dsysid->arg);
		if (sysid <= 0)
			elog(ERROR, "group id must be positive");
		havesysid = true;
	}

	if (duserElts)
		userElts = (List *) duserElts->arg;

	/*
	 * Make sure the user can do this.
	 */
	if (!superuser())
		elog(ERROR, "CREATE GROUP: permission denied");

	pg_group_rel = heap_openr(GroupRelationName, ExclusiveLock);
	pg_group_dsc = RelationGetDescr(pg_group_rel);

	scan = heap_beginscan(pg_group_rel, false, SnapshotNow, 0, NULL);
	max_id = 99;				/* start auto-assigned ids at 100 */
	while (!group_exists && !sysid_exists &&
		   HeapTupleIsValid(tuple = heap_getnext(scan, false)))
	{
		Datum		datum;
		bool		null;

		datum = heap_getattr(tuple, Anum_pg_group_groname,
							 pg_group_dsc, &null);
		Assert(!null);
		group_exists = (strcmp((char *) DatumGetName(datum), stmt->name) == 0);

		datum = heap_getattr(tuple, Anum_pg_group_grosysid,
							 pg_group_dsc, &null);
		Assert(!null);
		if (havesysid)			/* customized id wanted */
			sysid_exists = (DatumGetInt32(datum) == sysid);
		else
		{
			/* pick 1 + max */
			if (DatumGetInt32(datum) > max_id)
				max_id = DatumGetInt32(datum);
		}
	}
	heap_endscan(scan);

	if (group_exists)
		elog(ERROR, "CREATE GROUP: group name \"%s\" already exists",
			 stmt->name);
	if (sysid_exists)
		elog(ERROR, "CREATE GROUP: group sysid %d is already assigned",
			 sysid);

	/*
	 * Translate the given user names to ids
	 */
	foreach(item, userElts)
	{
		const char *groupuser = strVal(lfirst(item));
		Value	   *v;

		v = makeInteger(get_usesysid(groupuser));
		if (!member(v, newlist))
			newlist = lappend(newlist, v);
	}

	/* build an array to insert */
	if (newlist)
	{
		int			i;

		userarray = palloc(ARR_OVERHEAD(1) + length(newlist) * sizeof(int32));
		userarray->size = ARR_OVERHEAD(1) + length(newlist) * sizeof(int32);
		userarray->flags = 0;
		ARR_NDIM(userarray) = 1;	/* one dimensional array */
		ARR_LBOUND(userarray)[0] = 1;	/* axis starts at one */
		ARR_DIMS(userarray)[0] = length(newlist);		/* axis is this long */
		/* fill the array */
		i = 0;
		foreach(item, newlist)
			((int *) ARR_DATA_PTR(userarray))[i++] = intVal(lfirst(item));
	}
	else
		userarray = NULL;

	/*
	 * Form a tuple to insert
	 */
	if (!havesysid)
		sysid = max_id + 1;

	new_record[Anum_pg_group_groname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->name));
	new_record[Anum_pg_group_grosysid - 1] = Int32GetDatum(sysid);
	new_record[Anum_pg_group_grolist - 1] = PointerGetDatum(userarray);

	new_record_nulls[Anum_pg_group_groname - 1] = ' ';
	new_record_nulls[Anum_pg_group_grosysid - 1] = ' ';
	new_record_nulls[Anum_pg_group_grolist - 1] = userarray ? ' ' : 'n';

	tuple = heap_formtuple(pg_group_dsc, new_record, new_record_nulls);

	/*
	 * Insert a new record in the pg_group_table
	 */
	heap_insert(pg_group_rel, tuple);

	/*
	 * Update indexes
	 */
	if (RelationGetForm(pg_group_rel)->relhasindex)
	{
		Relation	idescs[Num_pg_group_indices];

		CatalogOpenIndices(Num_pg_group_indices,
						   Name_pg_group_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_group_indices, pg_group_rel,
						   tuple);
		CatalogCloseIndices(Num_pg_group_indices, idescs);
	}

	heap_close(pg_group_rel, NoLock);
}



/*
 * ALTER GROUP
 */
void
AlterGroup(AlterGroupStmt *stmt, const char *tag)
{
	Relation	pg_group_rel;
	TupleDesc	pg_group_dsc;
	HeapTuple	group_tuple;

	/*
	 * Make sure the user can do this.
	 */
	if (!superuser())
		elog(ERROR, "%s: permission denied", tag);

	pg_group_rel = heap_openr(GroupRelationName, ExclusiveLock);
	pg_group_dsc = RelationGetDescr(pg_group_rel);

	/*
	 * Fetch existing tuple for group.
	 */
	group_tuple = SearchSysCache(GRONAME,
								 PointerGetDatum(stmt->name),
								 0, 0, 0);
	if (!HeapTupleIsValid(group_tuple))
		elog(ERROR, "%s: group \"%s\" does not exist", tag, stmt->name);

	/*
	 * Now decide what to do.
	 */
	AssertState(stmt->action == +1 || stmt->action == -1);

	if (stmt->action == +1)		/* add users, might also be invoked by
								 * create user */
	{
		Datum		new_record[Natts_pg_group];
		char		new_record_nulls[Natts_pg_group];
		ArrayType  *newarray,
				   *oldarray;
		List	   *newlist = NIL,
				   *item;
		HeapTuple	tuple;
		bool		null = false;
		Datum		datum = heap_getattr(group_tuple, Anum_pg_group_grolist, pg_group_dsc, &null);
		int			i;

		oldarray = null ? ((IdList *) NULL) : DatumGetIdListP(datum);
		Assert(null || ARR_NDIM(oldarray) == 1);
		/* first add the old array to the hitherto empty list */
		if (!null)
			for (i = ARR_LBOUND(oldarray)[0]; i < ARR_LBOUND(oldarray)[0] + ARR_DIMS(oldarray)[0]; i++)
			{
				int			index,
							arrval;
				Value	   *v;
				bool		valueNull;

				index = i;
				arrval = DatumGetInt32(array_ref(oldarray, 1, &index, true /* by value */ ,
											sizeof(int), 0, &valueNull));
				v = makeInteger(arrval);
				/* filter out duplicates */
				if (!member(v, newlist))
					newlist = lappend(newlist, v);
			}

		/*
		 * now convert the to be added usernames to sysids and add them to
		 * the list
		 */
		foreach(item, stmt->listUsers)
		{
			Value	   *v;

			if (strcmp(tag, "ALTER GROUP") == 0)
			{
				/* Get the uid of the proposed user to add. */
				v = makeInteger(get_usesysid(strVal(lfirst(item))));
			}
			else if (strcmp(tag, "CREATE USER") == 0)
			{
				/*
				 * in this case we already know the uid and it wouldn't be
				 * in the cache anyway yet
				 */
				v = lfirst(item);
			}
			else
			{
				elog(ERROR, "AlterGroup: unknown tag %s", tag);
				v = NULL;		/* keep compiler quiet */
			}

			if (!member(v, newlist))
				newlist = lappend(newlist, v);
			else

				/*
				 * we silently assume here that this error will only come
				 * up in a ALTER GROUP statement
				 */
				elog(NOTICE, "%s: user \"%s\" is already in group \"%s\"",
					 tag, strVal(lfirst(item)), stmt->name);
		}

		newarray = palloc(ARR_OVERHEAD(1) + length(newlist) * sizeof(int32));
		newarray->size = ARR_OVERHEAD(1) + length(newlist) * sizeof(int32);
		newarray->flags = 0;
		ARR_NDIM(newarray) = 1; /* one dimensional array */
		ARR_LBOUND(newarray)[0] = 1;	/* axis starts at one */
		ARR_DIMS(newarray)[0] = length(newlist);		/* axis is this long */
		/* fill the array */
		i = 0;
		foreach(item, newlist)
			((int *) ARR_DATA_PTR(newarray))[i++] = intVal(lfirst(item));

		/*
		 * Form a tuple with the new array and write it back.
		 */
		new_record[Anum_pg_group_groname - 1] =
			DirectFunctionCall1(namein, CStringGetDatum(stmt->name));
		new_record_nulls[Anum_pg_group_groname - 1] = ' ';
		new_record[Anum_pg_group_grosysid - 1] = heap_getattr(group_tuple, Anum_pg_group_grosysid, pg_group_dsc, &null);
		new_record_nulls[Anum_pg_group_grosysid - 1] = null ? 'n' : ' ';
		new_record[Anum_pg_group_grolist - 1] = PointerGetDatum(newarray);
		new_record_nulls[Anum_pg_group_grolist - 1] = newarray ? ' ' : 'n';

		tuple = heap_formtuple(pg_group_dsc, new_record, new_record_nulls);
		simple_heap_update(pg_group_rel, &group_tuple->t_self, tuple);

		/* Update indexes */
		if (RelationGetForm(pg_group_rel)->relhasindex)
		{
			Relation	idescs[Num_pg_group_indices];

			CatalogOpenIndices(Num_pg_group_indices,
							   Name_pg_group_indices, idescs);
			CatalogIndexInsert(idescs, Num_pg_group_indices, pg_group_rel,
							   tuple);
			CatalogCloseIndices(Num_pg_group_indices, idescs);
		}
	}							/* endif alter group add user */

	else if (stmt->action == -1)	/* drop users from group */
	{
		Datum		datum;
		bool		null;
		bool		is_dropuser = strcmp(tag, "DROP USER") == 0;

		datum = heap_getattr(group_tuple, Anum_pg_group_grolist, pg_group_dsc, &null);
		if (null)
		{
			if (!is_dropuser)
				elog(NOTICE, "ALTER GROUP: group \"%s\" does not have any members", stmt->name);
		}
		else
		{
			HeapTuple	tuple;
			Datum		new_record[Natts_pg_group];
			char		new_record_nulls[Natts_pg_group];
			ArrayType  *oldarray,
					   *newarray;
			List	   *newlist = NIL,
					   *item;
			int			i;

			oldarray = DatumGetIdListP(datum);
			Assert(ARR_NDIM(oldarray) == 1);
			/* first add the old array to the hitherto empty list */
			for (i = ARR_LBOUND(oldarray)[0]; i < ARR_LBOUND(oldarray)[0] + ARR_DIMS(oldarray)[0]; i++)
			{
				int			index,
							arrval;
				Value	   *v;
				bool		valueNull;

				index = i;
				arrval = DatumGetInt32(array_ref(oldarray, 1, &index, true /* by value */ ,
											sizeof(int), 0, &valueNull));
				v = makeInteger(arrval);
				/* filter out duplicates */
				if (!member(v, newlist))
					newlist = lappend(newlist, v);
			}

			/*
			 * now convert the to be dropped usernames to sysids and
			 * remove them from the list
			 */
			foreach(item, stmt->listUsers)
			{
				Value	   *v;

				if (!is_dropuser)
				{
					/* Get the uid of the proposed user to drop. */
					v = makeInteger(get_usesysid(strVal(lfirst(item))));
				}
				else
				{
					/* for dropuser we already know the uid */
					v = lfirst(item);
				}
				if (member(v, newlist))
					newlist = LispRemove(v, newlist);
				else if (!is_dropuser)
					elog(NOTICE, "ALTER GROUP: user \"%s\" is not in group \"%s\"", strVal(lfirst(item)), stmt->name);
			}

			newarray = palloc(ARR_OVERHEAD(1) + length(newlist) * sizeof(int32));
			newarray->size = ARR_OVERHEAD(1) + length(newlist) * sizeof(int32);
			newarray->flags = 0;
			ARR_NDIM(newarray) = 1;		/* one dimensional array */
			ARR_LBOUND(newarray)[0] = 1;		/* axis starts at one */
			ARR_DIMS(newarray)[0] = length(newlist);	/* axis is this long */
			/* fill the array */
			i = 0;
			foreach(item, newlist)
				((int *) ARR_DATA_PTR(newarray))[i++] = intVal(lfirst(item));

			/*
			 * Insert the new tuple with the updated user list
			 */
			new_record[Anum_pg_group_groname - 1] =
				DirectFunctionCall1(namein, CStringGetDatum(stmt->name));
			new_record_nulls[Anum_pg_group_groname - 1] = ' ';
			new_record[Anum_pg_group_grosysid - 1] = heap_getattr(group_tuple, Anum_pg_group_grosysid, pg_group_dsc, &null);
			new_record_nulls[Anum_pg_group_grosysid - 1] = null ? 'n' : ' ';
			new_record[Anum_pg_group_grolist - 1] = PointerGetDatum(newarray);
			new_record_nulls[Anum_pg_group_grolist - 1] = newarray ? ' ' : 'n';

			tuple = heap_formtuple(pg_group_dsc, new_record, new_record_nulls);
			simple_heap_update(pg_group_rel, &group_tuple->t_self, tuple);

			/* Update indexes */
			if (RelationGetForm(pg_group_rel)->relhasindex)
			{
				Relation	idescs[Num_pg_group_indices];

				CatalogOpenIndices(Num_pg_group_indices,
								   Name_pg_group_indices, idescs);
				CatalogIndexInsert(idescs, Num_pg_group_indices, pg_group_rel,
								   tuple);
				CatalogCloseIndices(Num_pg_group_indices, idescs);
			}

		}						/* endif group not null */
	}							/* endif alter group drop user */

	ReleaseSysCache(group_tuple);

	heap_close(pg_group_rel, NoLock);
}



/*
 * DROP GROUP
 */
void
DropGroup(DropGroupStmt *stmt)
{
	Relation	pg_group_rel;
	HeapScanDesc scan;
	HeapTuple	tuple;
	TupleDesc	pg_group_dsc;
	bool		gro_exists = false;

	/*
	 * Make sure the user can do this.
	 */
	if (!superuser())
		elog(ERROR, "DROP GROUP: permission denied");

	/*
	 * Scan the pg_group table and delete all matching groups.
	 */
	pg_group_rel = heap_openr(GroupRelationName, ExclusiveLock);
	pg_group_dsc = RelationGetDescr(pg_group_rel);
	scan = heap_beginscan(pg_group_rel, false, SnapshotNow, 0, NULL);

	while (HeapTupleIsValid(tuple = heap_getnext(scan, false)))
	{
		Datum		datum;
		bool		null;

		datum = heap_getattr(tuple, Anum_pg_group_groname,
							 pg_group_dsc, &null);
		if (!null && strcmp((char *) DatumGetName(datum), stmt->name) == 0)
		{
			gro_exists = true;
			simple_heap_delete(pg_group_rel, &tuple->t_self);
		}
	}

	heap_endscan(scan);

	/*
	 * Did we find any?
	 */
	if (!gro_exists)
		elog(ERROR, "DROP GROUP: group \"%s\" does not exist", stmt->name);

	heap_close(pg_group_rel, NoLock);
}
