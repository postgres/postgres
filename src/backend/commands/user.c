/*-------------------------------------------------------------------------
 *
 * user.c
 *	  use pg_exec_query to create a new user in the catalog
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: user.c,v 1.27 1999/04/02 06:16:36 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>				
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <postgres.h>

#include <miscadmin.h>
#include <catalog/catname.h>
#include <catalog/pg_database.h>
#include <catalog/pg_shadow.h>
#include <libpq/crypt.h>
#include <access/heapam.h>
#include <access/xact.h>
#include <storage/bufmgr.h>
#include <storage/lmgr.h>
#include <tcop/tcopprot.h>
#include <utils/acl.h>
#include <utils/rel.h>
#include <utils/syscache.h>
#include <commands/user.h>

static void CheckPgUserAclNotNull(void);

#define	SQL_LENGTH	512

/*---------------------------------------------------------------------
 * UpdatePgPwdFile
 *
 * copy the modified contents of pg_shadow to a file used by the postmaster
 * for user authentication.  The file is stored as $PGDATA/pg_pwd.
 *---------------------------------------------------------------------
 */
static
void
UpdatePgPwdFile(char *sql, CommandDest dest)
{

	char	*filename,
				*tempname;
	int		bufsize;

	/*
	 * Create a temporary filename to be renamed later.  This prevents the
	 * backend from clobbering the pg_pwd file while the postmaster might
	 * be reading from it.
	 */
	filename = crypt_getpwdfilename();
	bufsize = strlen(filename) + 12;
	tempname = (char *) palloc(bufsize);
	snprintf(tempname, bufsize, "%s.%d", filename, MyProcPid);

	/*
	 * Copy the contents of pg_shadow to the pg_pwd ASCII file using a the
	 * SEPCHAR character as the delimiter between fields.  Then rename the
	 * file to its final name.
	 */
	snprintf(sql, SQL_LENGTH, 
			"copy %s to '%s' using delimiters %s", 
			ShadowRelationName, tempname, CRYPT_PWD_FILE_SEPCHAR);
	pg_exec_query_dest(sql, dest, false);
	rename(tempname, filename);
	pfree((void *) tempname);

	/*
	 * Create a flag file the postmaster will detect the next time it
	 * tries to authenticate a user.  The postmaster will know to reload
	 * the pg_pwd file contents.
	 */
	filename = crypt_getpwdreloadfilename();
	creat(filename, S_IRUSR | S_IWUSR);
}

/*---------------------------------------------------------------------
 * DefineUser
 *
 * Add the user to the pg_shadow relation, and if specified make sure the
 * user is specified in the desired groups of defined in pg_group.
 *---------------------------------------------------------------------
 */
void
DefineUser(CreateUserStmt *stmt, CommandDest dest)
{
	char			   *pg_shadow,
						sql[SQL_LENGTH];
	Relation			pg_shadow_rel;
	TupleDesc			pg_shadow_dsc;
	HeapScanDesc		scan;
	HeapTuple			tuple;
	Datum				datum;
	bool				exists = false,
						n,
						inblock,
						havepassword,
						havevaluntil;
	int					max_id = -1;

	havepassword = stmt->password && stmt->password[0];
	havevaluntil = stmt->validUntil && stmt->validUntil[0];

	if (havepassword)
		CheckPgUserAclNotNull();
	if (!(inblock = IsTransactionBlock()))
		BeginTransactionBlock();

	/*
	 * Make sure the user attempting to create a user can insert into the
	 * pg_shadow relation.
	 */
	pg_shadow = GetPgUserName();
	if (pg_aclcheck(ShadowRelationName, pg_shadow, ACL_RD | ACL_WR | ACL_AP) != ACLCHECK_OK)
	{
		UserAbortTransactionBlock();
		elog(ERROR, "defineUser: user \"%s\" does not have SELECT and INSERT privilege for \"%s\"",
			 pg_shadow, ShadowRelationName);
		return;
	}

	/*
	 * Scan the pg_shadow relation to be certain the user doesn't already
	 * exist.
	 */
	pg_shadow_rel = heap_openr(ShadowRelationName);
	pg_shadow_dsc = RelationGetDescr(pg_shadow_rel);

	/*
	 * Secure a write lock on pg_shadow so we can be sure of what the next
	 * usesysid should be.
	 */
	LockRelation(pg_shadow_rel, AccessExclusiveLock);

	scan = heap_beginscan(pg_shadow_rel, false, SnapshotNow, 0, NULL);
	while (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
	{
		datum = heap_getattr(tuple, Anum_pg_shadow_usename, pg_shadow_dsc, &n);

		if (!exists && !strncmp((char *) datum, stmt->user, strlen(stmt->user)))
			exists = true;

		datum = heap_getattr(tuple, Anum_pg_shadow_usesysid, pg_shadow_dsc, &n);
		if ((int) datum > max_id)
			max_id = (int) datum;
	}
	heap_endscan(scan);

	if (exists)
	{
		UnlockRelation(pg_shadow_rel, AccessExclusiveLock);
		heap_close(pg_shadow_rel);
		UserAbortTransactionBlock();
		elog(ERROR, 
				"defineUser: user \"%s\" has already been created", stmt->user);
		return;
	}

	/*
	 * Build the insert statement to be executed.
	 *
	 * XXX Ugly as this code is, it still fails to cope with ' or \
	 * in any of the provided strings.
	 */
	snprintf(sql, SQL_LENGTH, 
			 "insert into %s (usename,usesysid,usecreatedb,usetrace,"
			 "usesuper,usecatupd,passwd,valuntil) "
			 "values('%s',%d,'%c','t','%c','t',%s%s%s,%s%s%s)", 
			 ShadowRelationName,
			 stmt->user,
			 max_id + 1,
			 (stmt->createdb && *stmt->createdb) ? 't' : 'f',
			 (stmt->createuser && *stmt->createuser) ? 't' : 'f',
			 havepassword ? "'" : "",
			 havepassword ? stmt->password : "NULL",
			 havepassword ? "'" : "",
			 havevaluntil ? "'" : "",
			 havevaluntil ? stmt->validUntil : "NULL",
			 havevaluntil ? "'" : "");

	/*
	 * XXX If insert fails, say because a bogus valuntil date is given,
	 * need to catch the resulting error and undo our transaction.
	 */
	pg_exec_query_dest(sql, dest, false);

	/*
	 * Add the stuff here for groups.
	 */

	UpdatePgPwdFile(sql, dest);

	/*
	 * This goes after the UpdatePgPwdFile to be certain that two backends
	 * to not attempt to write to the pg_pwd file at the same time.
	 */
	UnlockRelation(pg_shadow_rel, AccessExclusiveLock);
	heap_close(pg_shadow_rel);

	if (IsTransactionBlock() && !inblock)
		EndTransactionBlock();
}


extern void
AlterUser(AlterUserStmt *stmt, CommandDest dest)
{

	char			*pg_shadow,
						sql[SQL_LENGTH];
	Relation	pg_shadow_rel;
	TupleDesc	pg_shadow_dsc;
	HeapTuple	tuple;
	bool			inblock;

	if (stmt->password)
		CheckPgUserAclNotNull();
	if (!(inblock = IsTransactionBlock()))
		BeginTransactionBlock();

	/*
	 * Make sure the user attempting to create a user can insert into the
	 * pg_shadow relation.
	 */
	pg_shadow = GetPgUserName();
	if (pg_aclcheck(ShadowRelationName, pg_shadow, ACL_RD | ACL_WR) != ACLCHECK_OK)
	{
		UserAbortTransactionBlock();
		elog(ERROR, "alterUser: user \"%s\" does not have SELECT and UPDATE privilege for \"%s\"",
			 pg_shadow, ShadowRelationName);
		return;
	}

	/*
	 * Scan the pg_shadow relation to be certain the user exists.
	 */
	pg_shadow_rel = heap_openr(ShadowRelationName);
	pg_shadow_dsc = RelationGetDescr(pg_shadow_rel);

	/*
	 * Secure a write lock on pg_shadow so we can be sure that when the
	 * dump of the pg_pwd file is done, there is not another backend doing
	 * the same.
	 */
	LockRelation(pg_shadow_rel, AccessExclusiveLock);

	tuple = SearchSysCacheTuple(USENAME,
								PointerGetDatum(stmt->user),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
	{
		UnlockRelation(pg_shadow_rel, AccessExclusiveLock);
		heap_close(pg_shadow_rel);
		UserAbortTransactionBlock();	/* needed? */
		elog(ERROR, "alterUser: user \"%s\" does not exist", stmt->user);
		return;
	}

	/*
	 * Create the update statement to modify the user.
	 */
	snprintf(sql, SQL_LENGTH, "update %s set", ShadowRelationName);

	if (stmt->password)
	{
		snprintf(sql, SQL_LENGTH, "%s passwd = '%s'", pstrdup(sql), stmt->password);
	}

	if (stmt->createdb)
	{
		snprintf(sql, SQL_LENGTH, "%s %susecreatedb='%s'",
				pstrdup(sql), stmt->password ? "," : "",
				*stmt->createdb ? "t" : "f");
	}

	if (stmt->createuser)
	{
		snprintf(sql, SQL_LENGTH, "%s %susesuper='%s'",
				pstrdup(sql), (stmt->password || stmt->createdb) ? "," : "",
				*stmt->createuser ? "t" : "f");
	}

	if (stmt->validUntil)
	{
		snprintf(sql, SQL_LENGTH, "%s %svaluntil='%s'",
				pstrdup(sql),
				(stmt->password || stmt->createdb || stmt->createuser) ? "," : "",
				stmt->validUntil);
	}

	snprintf(sql, SQL_LENGTH, "%s where usename = '%s'",
				pstrdup(sql), stmt->user);

	pg_exec_query_dest(sql, dest, false);

	/* do the pg_group stuff here */

	UpdatePgPwdFile(sql, dest);

	UnlockRelation(pg_shadow_rel, AccessExclusiveLock);
	heap_close(pg_shadow_rel);

	if (IsTransactionBlock() && !inblock)
		EndTransactionBlock();
}


extern void
RemoveUser(char *user, CommandDest dest)
{

	char	   *pg_shadow;
	Relation	pg_shadow_rel,
				pg_rel;
	TupleDesc	pg_dsc;
	HeapScanDesc scan;
	HeapTuple	tuple;
	Datum		datum;
	char		sql[512];
	bool		n,
				inblock;
	int32		usesysid;
	int			ndbase = 0;
	char	  **dbase = NULL;

	if (!(inblock = IsTransactionBlock()))
		BeginTransactionBlock();

	/*
	 * Make sure the user attempting to create a user can delete from the
	 * pg_shadow relation.
	 */
	pg_shadow = GetPgUserName();
	if (pg_aclcheck(ShadowRelationName, pg_shadow, ACL_RD | ACL_WR) != ACLCHECK_OK)
	{
		UserAbortTransactionBlock();
		elog(ERROR, "removeUser: user \"%s\" does not have SELECT and DELETE privilege for \"%s\"",
			 pg_shadow, ShadowRelationName);
	}

	/*
	 * Perform a scan of the pg_shadow relation to find the usesysid of
	 * the user to be deleted.	If it is not found, then return a warning
	 * message.
	 */
	pg_shadow_rel = heap_openr(ShadowRelationName);
	pg_dsc = RelationGetDescr(pg_shadow_rel);

	/*
	 * Secure a write lock on pg_shadow so we can be sure that when the
	 * dump of the pg_pwd file is done, there is not another backend doing
	 * the same.
	 */
	LockRelation(pg_shadow_rel, AccessExclusiveLock);

	tuple = SearchSysCacheTuple(USENAME,
								PointerGetDatum(user),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
	{
		UnlockRelation(pg_shadow_rel, AccessExclusiveLock);
		heap_close(pg_shadow_rel);
		UserAbortTransactionBlock();
		elog(ERROR, "removeUser: user \"%s\" does not exist", user);
	}

	usesysid = (int32) heap_getattr(tuple, Anum_pg_shadow_usesysid, pg_dsc, &n);

	/*
	 * Perform a scan of the pg_database relation to find the databases
	 * owned by usesysid.  Then drop them.
	 */
	pg_rel = heap_openr(DatabaseRelationName);
	pg_dsc = RelationGetDescr(pg_rel);

	scan = heap_beginscan(pg_rel, false, SnapshotNow, 0, NULL);
	while (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
	{
		datum = heap_getattr(tuple, Anum_pg_database_datdba, pg_dsc, &n);

		if ((int) datum == usesysid)
		{
			datum = heap_getattr(tuple, Anum_pg_database_datname, pg_dsc, &n);
			if (memcmp((void *) datum, "template1", 9))
			{
				dbase = 
						(char **) repalloc((void *) dbase, sizeof(char *) * (ndbase + 1));
				dbase[ndbase] = (char *) palloc(NAMEDATALEN + 1);
				memcpy((void *) dbase[ndbase], (void *) datum, NAMEDATALEN);
				dbase[ndbase++][NAMEDATALEN] = '\0';
			}
		}
	}
	heap_endscan(scan);
	heap_close(pg_rel);

	while (ndbase--)
	{
		elog(NOTICE, "Dropping database %s", dbase[ndbase]);
		snprintf(sql, SQL_LENGTH, "drop database %s", dbase[ndbase]);
		pfree((void *) dbase[ndbase]);
		pg_exec_query_dest(sql, dest, false);
	}
	if (dbase)
		pfree((void *) dbase);

	/*
	 * Since pg_shadow is global over all databases, one of two things
	 * must be done to insure complete consistency.  First, pg_shadow
	 * could be made non-global. This would elminate the code above for
	 * deleting database and would require the addition of code to delete
	 * tables, views, etc owned by the user.
	 *
	 * The second option would be to create a means of deleting tables, view,
	 * etc. owned by the user from other databases.  pg_shadow is global
	 * and so this must be done at some point.
	 *
	 * Let us not forget that the user should be removed from the pg_groups
	 * also.
	 *
	 * Todd A. Brandys 11/18/1997
	 *
	 */

	/*
	 * Remove the user from the pg_shadow table
	 */
	snprintf(sql, SQL_LENGTH, 
			"delete from %s where usename = '%s'", ShadowRelationName, user);
	pg_exec_query_dest(sql, dest, false);

	UpdatePgPwdFile(sql, dest);

	UnlockRelation(pg_shadow_rel, AccessExclusiveLock);
	heap_close(pg_shadow_rel);

	if (IsTransactionBlock() && !inblock)
		EndTransactionBlock();
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

	htup = SearchSysCacheTuple(RELNAME,
							   PointerGetDatum(ShadowRelationName),
							   0, 0, 0);
	if (!HeapTupleIsValid(htup))
	{
		elog(ERROR, "IsPgUserAclNull: class \"%s\" not found",
			 ShadowRelationName);
	}

	if (heap_attisnull(htup, Anum_pg_class_relacl))
	{
		elog(NOTICE, "To use passwords, you have to revoke permissions on pg_shadow");
		elog(NOTICE, "so normal users can not read the passwords.");
		elog(ERROR, "Try 'REVOKE ALL ON pg_shadow FROM PUBLIC'");
	}

	return;
}
