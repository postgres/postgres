/*-------------------------------------------------------------------------
 *
 * user.c
 *	  Commands for manipulating users and groups.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Header: /cvsroot/pgsql/src/backend/commands/user.c,v 1.128 2003/10/02 06:36:37 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_database.h"
#include "catalog/pg_group.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_type.h"
#include "commands/user.h"
#include "libpq/crypt.h"
#include "miscadmin.h"
#include "storage/pmsignal.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


#define PWD_FILE		"pg_pwd"
#define USER_GROUP_FILE "pg_group"


extern bool Password_encryption;

static bool user_file_update_needed = false;
static bool group_file_update_needed = false;


static void CheckPgUserAclNotNull(void);
static void UpdateGroupMembership(Relation group_rel, HeapTuple group_tuple,
					  List *members);
static IdList *IdListToArray(List *members);
static List *IdArrayToList(IdList *oldarray);


/*
 *	fputs_quote
 *
 *	Outputs string in quotes, with double-quotes duplicated.
 *	We could use quote_ident(), but that expects a TEXT argument.
 */
static void
fputs_quote(char *str, FILE *fp)
{
	fputc('"', fp);
	while (*str)
	{
		fputc(*str, fp);
		if (*str == '"')
			fputc('"', fp);
		str++;
	}
	fputc('"', fp);
}


/*
 * group_getfilename --- get full pathname of group file
 *
 * Note that result string is palloc'd, and should be freed by the caller.
 */
char *
group_getfilename(void)
{
	int			bufsize;
	char	   *pfnam;

	bufsize = strlen(DataDir) + strlen("/global/") +
		strlen(USER_GROUP_FILE) + 1;
	pfnam = (char *) palloc(bufsize);
	snprintf(pfnam, bufsize, "%s/global/%s", DataDir, USER_GROUP_FILE);

	return pfnam;
}


/*
 * Get full pathname of password file.
 *
 * Note that result string is palloc'd, and should be freed by the caller.
 */
char *
user_getfilename(void)
{
	int			bufsize;
	char	   *pfnam;

	bufsize = strlen(DataDir) + strlen("/global/") +
		strlen(PWD_FILE) + 1;
	pfnam = (char *) palloc(bufsize);
	snprintf(pfnam, bufsize, "%s/global/%s", DataDir, PWD_FILE);

	return pfnam;
}


/*
 * write_group_file: update the flat group file
 */
static void
write_group_file(Relation grel)
{
	char	   *filename,
			   *tempname;
	int			bufsize;
	FILE	   *fp;
	mode_t		oumask;
	HeapScanDesc scan;
	HeapTuple	tuple;
	TupleDesc	dsc = RelationGetDescr(grel);

	/*
	 * Create a temporary filename to be renamed later.  This prevents the
	 * backend from clobbering the pg_group file while the postmaster
	 * might be reading from it.
	 */
	filename = group_getfilename();
	bufsize = strlen(filename) + 12;
	tempname = (char *) palloc(bufsize);
	snprintf(tempname, bufsize, "%s.%d", filename, MyProcPid);

	oumask = umask((mode_t) 077);
	fp = AllocateFile(tempname, "w");
	umask(oumask);
	if (fp == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
			  errmsg("could not write to temporary file \"%s\": %m", tempname)));

	/*
	 * Read pg_group and write the file.  Note we use SnapshotSelf to
	 * ensure we see all effects of current transaction.  (Perhaps could
	 * do a CommandCounterIncrement beforehand, instead?)
	 */
	scan = heap_beginscan(grel, SnapshotSelf, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Datum		datum,
					grolist_datum;
		bool		isnull;
		char	   *groname;
		IdList	   *grolist_p;
		AclId	   *aidp;
		int			i,
					j,
					num;
		char	   *usename;
		bool		first_user = true;

		datum = heap_getattr(tuple, Anum_pg_group_groname, dsc, &isnull);
		/* ignore NULL groupnames --- shouldn't happen */
		if (isnull)
			continue;
		groname = NameStr(*DatumGetName(datum));

		/*
		 * Check for invalid characters in the group name.
		 */
		i = strcspn(groname, "\n");
		if (groname[i] != '\0')
		{
			ereport(LOG,
					(errmsg("invalid group name \"%s\"", groname)));
			continue;
		}

		grolist_datum = heap_getattr(tuple, Anum_pg_group_grolist, dsc, &isnull);
		/* Ignore NULL group lists */
		if (isnull)
			continue;

		/* be sure the IdList is not toasted */
		grolist_p = DatumGetIdListP(grolist_datum);

		/* scan grolist */
		num = IDLIST_NUM(grolist_p);
		aidp = IDLIST_DAT(grolist_p);
		for (i = 0; i < num; ++i)
		{
			tuple = SearchSysCache(SHADOWSYSID,
								   PointerGetDatum(aidp[i]),
								   0, 0, 0);
			if (HeapTupleIsValid(tuple))
			{
				usename = NameStr(((Form_pg_shadow) GETSTRUCT(tuple))->usename);

				/*
				 * Check for illegal characters in the user name.
				 */
				j = strcspn(usename, "\n");
				if (usename[j] != '\0')
				{
					ereport(LOG,
						  (errmsg("invalid user name \"%s\"", usename)));
					continue;
				}

				/*
				 * File format is: "dbname"    "user1" "user2" "user3"
				 */
				if (first_user)
				{
					fputs_quote(groname, fp);
					fputs("\t", fp);
				}
				else
					fputs(" ", fp);

				first_user = false;
				fputs_quote(usename, fp);

				ReleaseSysCache(tuple);
			}
		}
		if (!first_user)
			fputs("\n", fp);
		/* if IdList was toasted, free detoasted copy */
		if ((Pointer) grolist_p != DatumGetPointer(grolist_datum))
			pfree(grolist_p);
	}
	heap_endscan(scan);

	fflush(fp);
	if (ferror(fp))
		ereport(ERROR,
				(errcode_for_file_access(),
			  errmsg("could not write to temporary file \"%s\": %m", tempname)));
	FreeFile(fp);

	/*
	 * Rename the temp file to its final name, deleting the old pg_pwd. We
	 * expect that rename(2) is an atomic action.
	 */
	if (rename(tempname, filename))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						tempname, filename)));

	pfree((void *) tempname);
	pfree((void *) filename);
}


/*
 * write_user_file: update the flat password file
 */
static void
write_user_file(Relation urel)
{
	char	   *filename,
			   *tempname;
	int			bufsize;
	FILE	   *fp;
	mode_t		oumask;
	HeapScanDesc scan;
	HeapTuple	tuple;
	TupleDesc	dsc = RelationGetDescr(urel);

	/*
	 * Create a temporary filename to be renamed later.  This prevents the
	 * backend from clobbering the pg_pwd file while the postmaster might
	 * be reading from it.
	 */
	filename = user_getfilename();
	bufsize = strlen(filename) + 12;
	tempname = (char *) palloc(bufsize);
	snprintf(tempname, bufsize, "%s.%d", filename, MyProcPid);

	oumask = umask((mode_t) 077);
	fp = AllocateFile(tempname, "w");
	umask(oumask);
	if (fp == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
			  errmsg("could not write to temporary file \"%s\": %m", tempname)));

	/*
	 * Read pg_shadow and write the file.  Note we use SnapshotSelf to
	 * ensure we see all effects of current transaction.  (Perhaps could
	 * do a CommandCounterIncrement beforehand, instead?)
	 */
	scan = heap_beginscan(urel, SnapshotSelf, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Datum		datum;
		bool		isnull;
		char	   *usename,
				   *passwd,
				   *valuntil;
		int			i;

		datum = heap_getattr(tuple, Anum_pg_shadow_usename, dsc, &isnull);
		/* ignore NULL usernames (shouldn't happen) */
		if (isnull)
			continue;
		usename = NameStr(*DatumGetName(datum));

		datum = heap_getattr(tuple, Anum_pg_shadow_passwd, dsc, &isnull);

		/*
		 * It can be argued that people having a null password shouldn't
		 * be allowed to connect under password authentication, because
		 * they need to have a password set up first. If you think
		 * assuming an empty password in that case is better, change this
		 * logic to look something like the code for valuntil.
		 */
		if (isnull)
			continue;

		passwd = DatumGetCString(DirectFunctionCall1(textout, datum));

		datum = heap_getattr(tuple, Anum_pg_shadow_valuntil, dsc, &isnull);
		if (isnull)
			valuntil = pstrdup("");
		else
			valuntil = DatumGetCString(DirectFunctionCall1(abstimeout, datum));

		/*
		 * Check for illegal characters in the username and password.
		 */
		i = strcspn(usename, "\n");
		if (usename[i] != '\0')
		{
			ereport(LOG,
					(errmsg("invalid user name \"%s\"", usename)));
			continue;
		}
		i = strcspn(passwd, "\n");
		if (passwd[i] != '\0')
		{
			ereport(LOG,
					(errmsg("invalid user password \"%s\"", passwd)));
			continue;
		}

		/*
		 * The extra columns we emit here are not really necessary. To
		 * remove them, the parser in backend/libpq/crypt.c would need to
		 * be adjusted.
		 */
		fputs_quote(usename, fp);
		fputs(" ", fp);
		fputs_quote(passwd, fp);
		fputs(" ", fp);
		fputs_quote(valuntil, fp);
		fputs("\n", fp);

		pfree(passwd);
		pfree(valuntil);
	}
	heap_endscan(scan);

	fflush(fp);
	if (ferror(fp))
		ereport(ERROR,
				(errcode_for_file_access(),
			  errmsg("could not write to temporary file \"%s\": %m", tempname)));
	FreeFile(fp);

	/*
	 * Rename the temp file to its final name, deleting the old pg_pwd. We
	 * expect that rename(2) is an atomic action.
	 */
	if (rename(tempname, filename))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						tempname, filename)));

	pfree((void *) tempname);
	pfree((void *) filename);
}


/*
 * This trigger is fired whenever someone modifies pg_shadow or pg_group
 * via general-purpose INSERT/UPDATE/DELETE commands.
 *
 * XXX should probably have two separate triggers.
 */
Datum
update_pg_pwd_and_pg_group(PG_FUNCTION_ARGS)
{
	user_file_update_needed = true;
	group_file_update_needed = true;

	return PointerGetDatum(NULL);
}


/*
 * This routine is called during transaction commit or abort.
 *
 * On commit, if we've written pg_shadow or pg_group during the current
 * transaction, update the flat files and signal the postmaster.
 *
 * On abort, just reset the static flags so we don't try to do it on the
 * next successful commit.
 *
 * NB: this should be the last step before actual transaction commit.
 * If any error aborts the transaction after we run this code, the postmaster
 * will still have received and cached the changed data; so minimize the
 * window for such problems.
 */
void
AtEOXact_UpdatePasswordFile(bool isCommit)
{
	Relation	urel = NULL;
	Relation	grel = NULL;

	if (!(user_file_update_needed || group_file_update_needed))
		return;

	if (!isCommit)
	{
		user_file_update_needed = false;
		group_file_update_needed = false;
		return;
	}

	/*
	 * We use ExclusiveLock to ensure that only one backend writes the
	 * flat file(s) at a time.	That's sufficient because it's okay to
	 * allow plain reads of the tables in parallel.  There is some chance
	 * of a deadlock here (if we were triggered by a user update of
	 * pg_shadow or pg_group, which likely won't have gotten a strong
	 * enough lock), so get the locks we need before writing anything.
	 */
	if (user_file_update_needed)
		urel = heap_openr(ShadowRelationName, ExclusiveLock);
	if (group_file_update_needed)
		grel = heap_openr(GroupRelationName, ExclusiveLock);

	/* Okay to write the files */
	if (user_file_update_needed)
	{
		user_file_update_needed = false;
		write_user_file(urel);
		heap_close(urel, NoLock);
	}

	if (group_file_update_needed)
	{
		group_file_update_needed = false;
		write_group_file(grel);
		heap_close(grel, NoLock);
	}

	/*
	 * Signal the postmaster to reload its password & group-file cache.
	 */
	SendPostmasterSignal(PMSIGNAL_PASSWORD_CHANGE);
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
			if (dsysid)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dsysid = defel;
		}
		else if (strcmp(defel->defname, "createdb") == 0)
		{
			if (dcreatedb)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dcreatedb = defel;
		}
		else if (strcmp(defel->defname, "createuser") == 0)
		{
			if (dcreateuser)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dcreateuser = defel;
		}
		else if (strcmp(defel->defname, "groupElts") == 0)
		{
			if (dgroupElts)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dgroupElts = defel;
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
	if (dcreateuser)
		createuser = intVal(dcreateuser->arg) != 0;
	if (dsysid)
	{
		sysid = intVal(dsysid->arg);
		if (sysid <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("user ID must be positive")));
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
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create users")));

	if (strcmp(stmt->user, "public") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("user name \"%s\" is reserved",
						stmt->user)));

	/*
	 * Scan the pg_shadow relation to be certain the user or id doesn't
	 * already exist.  Note we secure exclusive lock, because we also need
	 * to be sure of what the next usesysid should be, and we need to
	 * protect our eventual update of the flat password file.
	 */
	pg_shadow_rel = heap_openr(ShadowRelationName, ExclusiveLock);
	pg_shadow_dsc = RelationGetDescr(pg_shadow_rel);

	scan = heap_beginscan(pg_shadow_rel, SnapshotNow, 0, NULL);
	max_id = 99;				/* start auto-assigned ids at 100 */
	while (!user_exists && !sysid_exists &&
		   (tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_shadow shadow_form = (Form_pg_shadow) GETSTRUCT(tuple);
		int32		this_sysid;

		user_exists = (strcmp(NameStr(shadow_form->usename), stmt->user) == 0);

		this_sysid = shadow_form->usesysid;
		if (havesysid)			/* customized id wanted */
			sysid_exists = (this_sysid == sysid);
		else
		{
			/* pick 1 + max */
			if (this_sysid > max_id)
				max_id = this_sysid;
		}
	}
	heap_endscan(scan);

	if (user_exists)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("user \"%s\" already exists",
						stmt->user)));
	if (sysid_exists)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("user ID %d is already assigned", sysid)));

	/* If no sysid given, use max existing id + 1 */
	if (!havesysid)
		sysid = max_id + 1;

	/*
	 * Build a tuple to insert
	 */
	MemSet(new_record, 0, sizeof(new_record));
	MemSet(new_record_nulls, ' ', sizeof(new_record_nulls));

	new_record[Anum_pg_shadow_usename - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->user));
	new_record[Anum_pg_shadow_usesysid - 1] = Int32GetDatum(sysid);
	AssertState(BoolIsValid(createdb));
	new_record[Anum_pg_shadow_usecreatedb - 1] = BoolGetDatum(createdb);
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
				elog(ERROR, "password encryption failed");
			new_record[Anum_pg_shadow_passwd - 1] =
				DirectFunctionCall1(textin, CStringGetDatum(encrypted_password));
		}
	}
	else
		new_record_nulls[Anum_pg_shadow_passwd - 1] = 'n';

	if (validUntil)
		new_record[Anum_pg_shadow_valuntil - 1] =
			DirectFunctionCall1(abstimein, CStringGetDatum(validUntil));
	else
		new_record_nulls[Anum_pg_shadow_valuntil - 1] = 'n';

	new_record_nulls[Anum_pg_shadow_useconfig - 1] = 'n';

	tuple = heap_formtuple(pg_shadow_dsc, new_record, new_record_nulls);

	/*
	 * Insert new record in the pg_shadow table
	 */
	simple_heap_insert(pg_shadow_rel, tuple);

	/* Update indexes */
	CatalogUpdateIndexes(pg_shadow_rel, tuple);

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
	 * Now we can clean up; but keep lock until commit (to avoid possible
	 * deadlock when commit code tries to acquire lock).
	 */
	heap_close(pg_shadow_rel, NoLock);

	/*
	 * Set flag to update flat password file at commit.
	 */
	user_file_update_needed = true;
}



/*
 * ALTER USER
 */
void
AlterUser(AlterUserStmt *stmt)
{
	Datum		new_record[Natts_pg_shadow];
	char		new_record_nulls[Natts_pg_shadow];
	char		new_record_repl[Natts_pg_shadow];
	Relation	pg_shadow_rel;
	TupleDesc	pg_shadow_dsc;
	HeapTuple	tuple,
				new_tuple;
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
		else if (strcmp(defel->defname, "createuser") == 0)
		{
			if (dcreateuser)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dcreateuser = defel;
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
		  strcmp(GetUserNameFromId(GetUserId()), stmt->user) == 0))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied")));

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
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("user \"%s\" does not exist", stmt->user)));

	/*
	 * Build an updated tuple, perusing the information just obtained
	 */
	MemSet(new_record, 0, sizeof(new_record));
	MemSet(new_record_nulls, ' ', sizeof(new_record_nulls));
	MemSet(new_record_repl, ' ', sizeof(new_record_repl));

	new_record[Anum_pg_shadow_usename - 1] = DirectFunctionCall1(namein,
											CStringGetDatum(stmt->user));
	new_record_repl[Anum_pg_shadow_usename - 1] = 'r';

	/* createdb */
	if (createdb >= 0)
	{
		new_record[Anum_pg_shadow_usecreatedb - 1] = BoolGetDatum(createdb > 0);
		new_record_repl[Anum_pg_shadow_usecreatedb - 1] = 'r';
	}

	/*
	 * createuser (superuser) and catupd
	 *
	 * XXX It's rather unclear how to handle catupd.  It's probably best to
	 * keep it equal to the superuser status, otherwise you could end up
	 * with a situation where no existing superuser can alter the
	 * catalogs, including pg_shadow!
	 */
	if (createuser >= 0)
	{
		new_record[Anum_pg_shadow_usesuper - 1] = BoolGetDatum(createuser > 0);
		new_record_repl[Anum_pg_shadow_usesuper - 1] = 'r';

		new_record[Anum_pg_shadow_usecatupd - 1] = BoolGetDatum(createuser > 0);
		new_record_repl[Anum_pg_shadow_usecatupd - 1] = 'r';
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
				elog(ERROR, "password encryption failed");
			new_record[Anum_pg_shadow_passwd - 1] =
				DirectFunctionCall1(textin, CStringGetDatum(encrypted_password));
		}
		new_record_repl[Anum_pg_shadow_passwd - 1] = 'r';
	}

	/* valid until */
	if (validUntil)
	{
		new_record[Anum_pg_shadow_valuntil - 1] =
			DirectFunctionCall1(abstimein, CStringGetDatum(validUntil));
		new_record_repl[Anum_pg_shadow_valuntil - 1] = 'r';
	}

	new_tuple = heap_modifytuple(tuple, pg_shadow_rel, new_record,
								 new_record_nulls, new_record_repl);
	simple_heap_update(pg_shadow_rel, &tuple->t_self, new_tuple);

	/* Update indexes */
	CatalogUpdateIndexes(pg_shadow_rel, new_tuple);

	ReleaseSysCache(tuple);
	heap_freetuple(new_tuple);

	/*
	 * Now we can clean up; but keep lock until commit (to avoid possible
	 * deadlock when commit code tries to acquire lock).
	 */
	heap_close(pg_shadow_rel, NoLock);

	/*
	 * Set flag to update flat password file at commit.
	 */
	user_file_update_needed = true;
}


/*
 * ALTER USER ... SET
 */
void
AlterUserSet(AlterUserSetStmt *stmt)
{
	char	   *valuestr;
	HeapTuple	oldtuple,
				newtuple;
	Relation	rel;
	Datum		repl_val[Natts_pg_shadow];
	char		repl_null[Natts_pg_shadow];
	char		repl_repl[Natts_pg_shadow];
	int			i;

	valuestr = flatten_set_variable_args(stmt->variable, stmt->value);

	/*
	 * RowExclusiveLock is sufficient, because we don't need to update the
	 * flat password file.
	 */
	rel = heap_openr(ShadowRelationName, RowExclusiveLock);
	oldtuple = SearchSysCache(SHADOWNAME,
							  PointerGetDatum(stmt->user),
							  0, 0, 0);
	if (!HeapTupleIsValid(oldtuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("user \"%s\" does not exist", stmt->user)));

	if (!(superuser()
	 || ((Form_pg_shadow) GETSTRUCT(oldtuple))->usesysid == GetUserId()))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied")));

	for (i = 0; i < Natts_pg_shadow; i++)
		repl_repl[i] = ' ';

	repl_repl[Anum_pg_shadow_useconfig - 1] = 'r';
	if (strcmp(stmt->variable, "all") == 0 && valuestr == NULL)
	{
		/* RESET ALL */
		repl_null[Anum_pg_shadow_useconfig - 1] = 'n';
	}
	else
	{
		Datum		datum;
		bool		isnull;
		ArrayType  *array;

		repl_null[Anum_pg_shadow_useconfig - 1] = ' ';

		datum = SysCacheGetAttr(SHADOWNAME, oldtuple,
								Anum_pg_shadow_useconfig, &isnull);

		array = isnull ? ((ArrayType *) NULL) : DatumGetArrayTypeP(datum);

		if (valuestr)
			array = GUCArrayAdd(array, stmt->variable, valuestr);
		else
			array = GUCArrayDelete(array, stmt->variable);

		if (array)
			repl_val[Anum_pg_shadow_useconfig - 1] = PointerGetDatum(array);
		else
			repl_null[Anum_pg_shadow_useconfig - 1] = 'n';
	}

	newtuple = heap_modifytuple(oldtuple, rel, repl_val, repl_null, repl_repl);
	simple_heap_update(rel, &oldtuple->t_self, newtuple);

	CatalogUpdateIndexes(rel, newtuple);

	ReleaseSysCache(oldtuple);
	heap_close(rel, RowExclusiveLock);
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
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to drop users")));

	/*
	 * Scan the pg_shadow relation to find the usesysid of the user to be
	 * deleted.  Note we secure exclusive lock, because we need to protect
	 * our update of the flat password file.
	 */
	pg_shadow_rel = heap_openr(ShadowRelationName, ExclusiveLock);
	pg_shadow_dsc = RelationGetDescr(pg_shadow_rel);

	foreach(item, stmt->users)
	{
		const char *user = strVal(lfirst(item));
		HeapTuple	tuple,
					tmp_tuple;
		Relation	pg_rel;
		TupleDesc	pg_dsc;
		ScanKeyData scankey;
		HeapScanDesc scan;
		AclId		usesysid;

		tuple = SearchSysCache(SHADOWNAME,
							   PointerGetDatum(user),
							   0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("user \"%s\" does not exist", user)));

		usesysid = ((Form_pg_shadow) GETSTRUCT(tuple))->usesysid;

		if (usesysid == GetUserId())
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("current user cannot be dropped")));
		if (usesysid == GetSessionUserId())
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("session user cannot be dropped")));

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

		scan = heap_beginscan(pg_rel, SnapshotNow, 1, &scankey);

		if ((tmp_tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
		{
			char	   *dbname;

			dbname = NameStr(((Form_pg_database) GETSTRUCT(tmp_tuple))->datname);
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("user \"%s\" cannot be dropped", user),
				   errdetail("The user owns database \"%s\".", dbname)));
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
		scan = heap_beginscan(pg_rel, SnapshotNow, 0, NULL);
		while ((tmp_tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
		{
			AlterGroupStmt ags;

			/* the group name from which to try to drop the user: */
			ags.name = pstrdup(NameStr(((Form_pg_group) GETSTRUCT(tmp_tuple))->groname));
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
	 * Now we can clean up; but keep lock until commit (to avoid possible
	 * deadlock when commit code tries to acquire lock).
	 */
	heap_close(pg_shadow_rel, NoLock);

	/*
	 * Set flag to update flat password file at commit.
	 */
	user_file_update_needed = true;
}


/*
 * Rename user
 */
void
RenameUser(const char *oldname, const char *newname)
{
	HeapTuple	tup;
	Relation	rel;

	/* ExclusiveLock because we need to update the password file */
	rel = heap_openr(ShadowRelationName, ExclusiveLock);

	tup = SearchSysCacheCopy(SHADOWNAME,
							 CStringGetDatum(oldname),
							 0, 0, 0);
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("user \"%s\" does not exist", oldname)));

	/*
	 * XXX Client applications probably store the session user somewhere,
	 * so renaming it could cause confusion.  On the other hand, there may
	 * not be an actual problem besides a little confusion, so think about
	 * this and decide.
	 */
	if (((Form_pg_shadow) GETSTRUCT(tup))->usesysid == GetSessionUserId())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("session user may not be renamed")));

	/* make sure the new name doesn't exist */
	if (SearchSysCacheExists(SHADOWNAME,
							 CStringGetDatum(newname),
							 0, 0, 0))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("user \"%s\" already exists", newname)));

	/* must be superuser */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to rename users")));

	/* rename */
	namestrcpy(&(((Form_pg_shadow) GETSTRUCT(tup))->usename), newname);
	simple_heap_update(rel, &tup->t_self, tup);
	CatalogUpdateIndexes(rel, tup);

	heap_close(rel, NoLock);
	heap_freetuple(tup);

	user_file_update_needed = true;
}


/*
 * CheckPgUserAclNotNull
 *
 * check to see if there is an ACL on pg_shadow
 */
static void
CheckPgUserAclNotNull(void)
{
	HeapTuple	htup;

	htup = SearchSysCache(RELOID,
						  ObjectIdGetDatum(RelOid_pg_shadow),
						  0, 0, 0);
	if (!HeapTupleIsValid(htup))	/* should not happen, we hope */
		elog(ERROR, "cache lookup failed for relation %u", RelOid_pg_shadow);

	if (heap_attisnull(htup, Anum_pg_class_relacl))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
		errmsg("before using passwords you must revoke privileges on %s",
			   ShadowRelationName),
				 errdetail("This restriction is to prevent unprivileged users from reading the passwords."),
				 errhint("Try REVOKE ALL ON \"%s\" FROM PUBLIC.",
						 ShadowRelationName)));

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
	IdList	   *grolist;
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
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dsysid = defel;
		}
		else if (strcmp(defel->defname, "userElts") == 0)
		{
			if (duserElts)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			duserElts = defel;
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	if (dsysid)
	{
		sysid = intVal(dsysid->arg);
		if (sysid <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("group ID must be positive")));
		havesysid = true;
	}

	if (duserElts)
		userElts = (List *) duserElts->arg;

	/*
	 * Make sure the user can do this.
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create groups")));

	if (strcmp(stmt->name, "public") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("group name \"%s\" is reserved",
						stmt->name)));

	/*
	 * Scan the pg_group relation to be certain the group or id doesn't
	 * already exist.  Note we secure exclusive lock, because we also need
	 * to be sure of what the next grosysid should be, and we need to
	 * protect our eventual update of the flat group file.
	 */
	pg_group_rel = heap_openr(GroupRelationName, ExclusiveLock);
	pg_group_dsc = RelationGetDescr(pg_group_rel);

	scan = heap_beginscan(pg_group_rel, SnapshotNow, 0, NULL);
	max_id = 99;				/* start auto-assigned ids at 100 */
	while (!group_exists && !sysid_exists &&
		   (tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_group group_form = (Form_pg_group) GETSTRUCT(tuple);
		int32		this_sysid;

		group_exists = (strcmp(NameStr(group_form->groname), stmt->name) == 0);

		this_sysid = group_form->grosysid;
		if (havesysid)			/* customized id wanted */
			sysid_exists = (this_sysid == sysid);
		else
		{
			/* pick 1 + max */
			if (this_sysid > max_id)
				max_id = this_sysid;
		}
	}
	heap_endscan(scan);

	if (group_exists)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("group \"%s\" already exists",
						stmt->name)));
	if (sysid_exists)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("group ID %d is already assigned", sysid)));

	/* If no sysid given, use max existing id + 1 */
	if (!havesysid)
		sysid = max_id + 1;

	/*
	 * Translate the given user names to ids
	 */
	foreach(item, userElts)
	{
		const char *groupuser = strVal(lfirst(item));
		int32		userid = get_usesysid(groupuser);

		if (!intMember(userid, newlist))
			newlist = lappendi(newlist, userid);
	}

	/* build an array to insert */
	if (newlist)
		grolist = IdListToArray(newlist);
	else
		grolist = NULL;

	/*
	 * Form a tuple to insert
	 */
	new_record[Anum_pg_group_groname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->name));
	new_record[Anum_pg_group_grosysid - 1] = Int32GetDatum(sysid);
	new_record[Anum_pg_group_grolist - 1] = PointerGetDatum(grolist);

	new_record_nulls[Anum_pg_group_groname - 1] = ' ';
	new_record_nulls[Anum_pg_group_grosysid - 1] = ' ';
	new_record_nulls[Anum_pg_group_grolist - 1] = grolist ? ' ' : 'n';

	tuple = heap_formtuple(pg_group_dsc, new_record, new_record_nulls);

	/*
	 * Insert a new record in the pg_group table
	 */
	simple_heap_insert(pg_group_rel, tuple);

	/* Update indexes */
	CatalogUpdateIndexes(pg_group_rel, tuple);

	/*
	 * Now we can clean up; but keep lock until commit (to avoid possible
	 * deadlock when commit code tries to acquire lock).
	 */
	heap_close(pg_group_rel, NoLock);

	/*
	 * Set flag to update flat group file at commit.
	 */
	group_file_update_needed = true;
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
	IdList	   *oldarray;
	Datum		datum;
	bool		null;
	List	   *newlist,
			   *item;

	/*
	 * Make sure the user can do this.
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to alter groups")));

	/*
	 * Secure exclusive lock to protect our update of the flat group file.
	 */
	pg_group_rel = heap_openr(GroupRelationName, ExclusiveLock);
	pg_group_dsc = RelationGetDescr(pg_group_rel);

	/*
	 * Fetch existing tuple for group.
	 */
	group_tuple = SearchSysCache(GRONAME,
								 PointerGetDatum(stmt->name),
								 0, 0, 0);
	if (!HeapTupleIsValid(group_tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("group \"%s\" does not exist", stmt->name)));

	/* Fetch old group membership. */
	datum = heap_getattr(group_tuple, Anum_pg_group_grolist,
						 pg_group_dsc, &null);
	oldarray = null ? ((IdList *) NULL) : DatumGetIdListP(datum);

	/* initialize list with old array contents */
	newlist = IdArrayToList(oldarray);

	/*
	 * Now decide what to do.
	 */
	AssertState(stmt->action == +1 || stmt->action == -1);

	if (stmt->action == +1)		/* add users, might also be invoked by
								 * create user */
	{
		/*
		 * convert the to be added usernames to sysids and add them to the
		 * list
		 */
		foreach(item, stmt->listUsers)
		{
			int32		sysid;

			if (strcmp(tag, "ALTER GROUP") == 0)
			{
				/* Get the uid of the proposed user to add. */
				sysid = get_usesysid(strVal(lfirst(item)));
			}
			else if (strcmp(tag, "CREATE USER") == 0)
			{
				/*
				 * in this case we already know the uid and it wouldn't be
				 * in the cache anyway yet
				 */
				sysid = intVal(lfirst(item));
			}
			else
			{
				elog(ERROR, "unexpected tag: \"%s\"", tag);
				sysid = 0;		/* keep compiler quiet */
			}

			if (!intMember(sysid, newlist))
				newlist = lappendi(newlist, sysid);
		}

		/* Do the update */
		UpdateGroupMembership(pg_group_rel, group_tuple, newlist);
	}							/* endif alter group add user */

	else if (stmt->action == -1)	/* drop users from group */
	{
		bool		is_dropuser = strcmp(tag, "DROP USER") == 0;

		if (newlist == NIL)
		{
			if (!is_dropuser)
				ereport(WARNING,
						(errcode(ERRCODE_WARNING),
						 errmsg("group \"%s\" does not have any members",
								stmt->name)));
		}
		else
		{
			/*
			 * convert the to be dropped usernames to sysids and remove
			 * them from the list
			 */
			foreach(item, stmt->listUsers)
			{
				int32		sysid;

				if (!is_dropuser)
				{
					/* Get the uid of the proposed user to drop. */
					sysid = get_usesysid(strVal(lfirst(item)));
				}
				else
				{
					/* for dropuser we already know the uid */
					sysid = intVal(lfirst(item));
				}
				if (intMember(sysid, newlist))
					newlist = lremovei(sysid, newlist);
				else if (!is_dropuser)
					ereport(WARNING,
							(errcode(ERRCODE_WARNING),
							 errmsg("user \"%s\" is not in group \"%s\"",
									strVal(lfirst(item)), stmt->name)));
			}

			/* Do the update */
			UpdateGroupMembership(pg_group_rel, group_tuple, newlist);
		}						/* endif group not null */
	}							/* endif alter group drop user */

	ReleaseSysCache(group_tuple);

	/*
	 * Now we can clean up; but keep lock until commit (to avoid possible
	 * deadlock when commit code tries to acquire lock).
	 */
	heap_close(pg_group_rel, NoLock);

	/*
	 * Set flag to update flat group file at commit.
	 */
	group_file_update_needed = true;
}

/*
 * Subroutine for AlterGroup: given a pg_group tuple and a desired new
 * membership (expressed as an integer list), form and write an updated tuple.
 * The pg_group relation must be open and locked already.
 */
static void
UpdateGroupMembership(Relation group_rel, HeapTuple group_tuple,
					  List *members)
{
	IdList	   *newarray;
	Datum		new_record[Natts_pg_group];
	char		new_record_nulls[Natts_pg_group];
	char		new_record_repl[Natts_pg_group];
	HeapTuple	tuple;

	newarray = IdListToArray(members);

	/*
	 * Form an updated tuple with the new array and write it back.
	 */
	MemSet(new_record, 0, sizeof(new_record));
	MemSet(new_record_nulls, ' ', sizeof(new_record_nulls));
	MemSet(new_record_repl, ' ', sizeof(new_record_repl));

	new_record[Anum_pg_group_grolist - 1] = PointerGetDatum(newarray);
	new_record_repl[Anum_pg_group_grolist - 1] = 'r';

	tuple = heap_modifytuple(group_tuple, group_rel,
						  new_record, new_record_nulls, new_record_repl);

	simple_heap_update(group_rel, &group_tuple->t_self, tuple);

	/* Update indexes */
	CatalogUpdateIndexes(group_rel, tuple);
}


/*
 * Convert an integer list of sysids to an array.
 */
static IdList *
IdListToArray(List *members)
{
	int			nmembers = length(members);
	IdList	   *newarray;
	List	   *item;
	int			i;

	newarray = palloc(ARR_OVERHEAD(1) + nmembers * sizeof(int32));
	newarray->size = ARR_OVERHEAD(1) + nmembers * sizeof(int32);
	newarray->flags = 0;
	newarray->elemtype = INT4OID;
	ARR_NDIM(newarray) = 1;		/* one dimensional array */
	ARR_LBOUND(newarray)[0] = 1;	/* axis starts at one */
	ARR_DIMS(newarray)[0] = nmembers;	/* axis is this long */
	i = 0;
	foreach(item, members)
		((int *) ARR_DATA_PTR(newarray))[i++] = lfirsti(item);

	return newarray;
}

/*
 * Convert an array of sysids to an integer list.
 */
static List *
IdArrayToList(IdList *oldarray)
{
	List	   *newlist = NIL;
	int			hibound,
				i;

	if (oldarray == NULL)
		return NIL;

	Assert(ARR_NDIM(oldarray) == 1);
	Assert(ARR_ELEMTYPE(oldarray) == INT4OID);

	hibound = ARR_DIMS(oldarray)[0];

	for (i = 0; i < hibound; i++)
	{
		int32		sysid;

		sysid = ((int32 *) ARR_DATA_PTR(oldarray))[i];
		/* filter out any duplicates --- probably a waste of time */
		if (!intMember(sysid, newlist))
			newlist = lappendi(newlist, sysid);
	}

	return newlist;
}


/*
 * DROP GROUP
 */
void
DropGroup(DropGroupStmt *stmt)
{
	Relation	pg_group_rel;
	HeapTuple	tuple;

	/*
	 * Make sure the user can do this.
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to drop groups")));

	/*
	 * Secure exclusive lock to protect our update of the flat group file.
	 */
	pg_group_rel = heap_openr(GroupRelationName, ExclusiveLock);

	/* Find and delete the group. */

	tuple = SearchSysCacheCopy(GRONAME,
							   PointerGetDatum(stmt->name),
							   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("group \"%s\" does not exist", stmt->name)));

	simple_heap_delete(pg_group_rel, &tuple->t_self);

	/*
	 * Now we can clean up; but keep lock until commit (to avoid possible
	 * deadlock when commit code tries to acquire lock).
	 */
	heap_close(pg_group_rel, NoLock);

	/*
	 * Set flag to update flat group file at commit.
	 */
	group_file_update_needed = true;
}


/*
 * Rename group
 */
void
RenameGroup(const char *oldname, const char *newname)
{
	HeapTuple	tup;
	Relation	rel;

	/* ExclusiveLock because we need to update the flat group file */
	rel = heap_openr(GroupRelationName, ExclusiveLock);

	tup = SearchSysCacheCopy(GRONAME,
							 CStringGetDatum(oldname),
							 0, 0, 0);
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("group \"%s\" does not exist", oldname)));

	/* make sure the new name doesn't exist */
	if (SearchSysCacheExists(GRONAME,
							 CStringGetDatum(newname),
							 0, 0, 0))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("group \"%s\" already exists", newname)));

	/* must be superuser */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to rename groups")));

	/* rename */
	namestrcpy(&(((Form_pg_group) GETSTRUCT(tup))->groname), newname);
	simple_heap_update(rel, &tup->t_self, tup);
	CatalogUpdateIndexes(rel, tup);

	heap_close(rel, NoLock);
	heap_freetuple(tup);

	group_file_update_needed = true;
}
