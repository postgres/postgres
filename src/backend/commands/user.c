/*-------------------------------------------------------------------------
 *
 * user.c--
 *	  use pg_exec_query to create a new user in the catalog
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>				/* for sprintf() */
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

/*---------------------------------------------------------------------
 * UpdatePgPwdFile
 *
 * copy the modified contents of pg_shadow to a file used by the postmaster
 * for user authentication.  The file is stored as $PGDATA/pg_pwd.
 *---------------------------------------------------------------------
 */
static
void
UpdatePgPwdFile(char *sql)
{

	char	   *filename;
	char	   *tempname;

	/*
	 * Create a temporary filename to be renamed later.  This prevents the
	 * backend from clobbering the pg_pwd file while the postmaster might
	 * be reading from it.
	 */
	filename = crypt_getpwdfilename();
	tempname = (char *) malloc(strlen(filename) + 12);
	sprintf(tempname, "%s.%d", filename, MyProcPid);

	/*
	 * Copy the contents of pg_shadow to the pg_pwd ASCII file using a the
	 * SEPCHAR character as the delimiter between fields.  Then rename the
	 * file to its final name.
	 */
	sprintf(sql, "copy %s to '%s' using delimiters %s", ShadowRelationName, tempname, CRYPT_PWD_FILE_SEPCHAR);
	pg_exec_query(sql, (char **) NULL, (Oid *) NULL, 0);
	rename(tempname, filename);
	free((void *) tempname);

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
DefineUser(CreateUserStmt *stmt)
{

	char	   *pg_user;
	Relation	pg_shadow_rel;
	TupleDesc	pg_shadow_dsc;
	HeapScanDesc scan;
	HeapTuple	tuple;
	Datum		datum;
	Buffer		buffer;
	char		sql[512];
	char	   *sql_end;
	bool		exists = false,
				n,
				inblock;
	int			max_id = -1;

	if (stmt->password)
		CheckPgUserAclNotNull();
	if (!(inblock = IsTransactionBlock()))
		BeginTransactionBlock();

	/*
	 * Make sure the user attempting to create a user can insert into the
	 * pg_shadow relation.
	 */
	pg_user = GetPgUserName();
	if (pg_aclcheck(ShadowRelationName, pg_user, ACL_RD | ACL_WR | ACL_AP) != ACLCHECK_OK)
	{
		UserAbortTransactionBlock();
		elog(ERROR, "defineUser: user \"%s\" does not have SELECT and INSERT privilege for \"%s\"",
			 pg_user, ShadowRelationName);
		return;
	}

	/*
	 * Scan the pg_shadow relation to be certain the user doesn't already
	 * exist.
	 */
	pg_shadow_rel = heap_openr(ShadowRelationName);
	pg_shadow_dsc = RelationGetTupleDescriptor(pg_shadow_rel);

	/*
	 * Secure a write lock on pg_shadow so we can be sure of what the next
	 * usesysid should be.
	 */
	RelationSetLockForWrite(pg_shadow_rel);

	scan = heap_beginscan(pg_shadow_rel, false, false, 0, NULL);
	while (HeapTupleIsValid(tuple = heap_getnext(scan, 0, &buffer)))
	{
		datum = heap_getattr(tuple, Anum_pg_shadow_usename, pg_shadow_dsc, &n);

		if (!exists && !strncmp((char *) datum, stmt->user, strlen(stmt->user)))
			exists = true;

		datum = heap_getattr(tuple, Anum_pg_shadow_usesysid, pg_shadow_dsc, &n);
		if ((int) datum > max_id)
			max_id = (int) datum;

		ReleaseBuffer(buffer);
	}
	heap_endscan(scan);

	if (exists)
	{
		RelationUnsetLockForWrite(pg_shadow_rel);
		heap_close(pg_shadow_rel);
		UserAbortTransactionBlock();
		elog(ERROR, "defineUser: user \"%s\" has already been created", stmt->user);
		return;
	}

	/*
	 * Build the insert statment to be executed.
	 */
	sprintf(sql, "insert into %s(usename,usesysid,usecreatedb,usetrace,usesuper,usecatupd,passwd", ShadowRelationName);
/*	if (stmt->password)
	strcat(sql, ",passwd"); -- removed so that insert empty string when no password */
	if (stmt->validUntil)
		strcat(sql, ",valuntil");

	sql_end = sql + strlen(sql);
	sprintf(sql_end, ") values('%s',%d", stmt->user, max_id + 1);
	if (stmt->createdb && *stmt->createdb)
		strcat(sql_end, ",'t','t'");
	else
		strcat(sql_end, ",'f','t'");
	if (stmt->createuser && *stmt->createuser)
		strcat(sql_end, ",'t','t'");
	else
		strcat(sql_end, ",'f','t'");
	sql_end += strlen(sql_end);
	if (stmt->password)
	{
		sprintf(sql_end, ",'%s'", stmt->password);
		sql_end += strlen(sql_end);
	}
	else
	{
		strcpy(sql_end, ",''");
		sql_end += strlen(sql_end);
	}
	if (stmt->validUntil)
	{
		sprintf(sql_end, ",'%s'", stmt->validUntil);
		sql_end += strlen(sql_end);
	}
	strcat(sql_end, ")");

	pg_exec_query(sql, (char **) NULL, (Oid *) NULL, 0);

	/*
	 * Add the stuff here for groups.
	 */

	UpdatePgPwdFile(sql);

	/*
	 * This goes after the UpdatePgPwdFile to be certain that two backends
	 * to not attempt to write to the pg_pwd file at the same time.
	 */
	RelationUnsetLockForWrite(pg_shadow_rel);
	heap_close(pg_shadow_rel);

	if (IsTransactionBlock() && !inblock)
		EndTransactionBlock();
}


extern void
AlterUser(AlterUserStmt *stmt)
{

	char	   *pg_user;
	Relation	pg_shadow_rel;
	TupleDesc	pg_shadow_dsc;
	HeapScanDesc scan;
	HeapTuple	tuple;
	Datum		datum;
	Buffer		buffer;
	char		sql[512];
	char	   *sql_end;
	bool		exists = false,
				n,
				inblock;

	if (stmt->password)
		CheckPgUserAclNotNull();
	if (!(inblock = IsTransactionBlock()))
		BeginTransactionBlock();

	/*
	 * Make sure the user attempting to create a user can insert into the
	 * pg_shadow relation.
	 */
	pg_user = GetPgUserName();
	if (pg_aclcheck(ShadowRelationName, pg_user, ACL_RD | ACL_WR) != ACLCHECK_OK)
	{
		UserAbortTransactionBlock();
		elog(ERROR, "alterUser: user \"%s\" does not have SELECT and UPDATE privilege for \"%s\"",
			 pg_user, ShadowRelationName);
		return;
	}

	/*
	 * Scan the pg_shadow relation to be certain the user exists.
	 */
	pg_shadow_rel = heap_openr(ShadowRelationName);
	pg_shadow_dsc = RelationGetTupleDescriptor(pg_shadow_rel);

	/*
	 * Secure a write lock on pg_shadow so we can be sure that when the
	 * dump of the pg_pwd file is done, there is not another backend doing
	 * the same.
	 */
	RelationSetLockForWrite(pg_shadow_rel);

	scan = heap_beginscan(pg_shadow_rel, false, false, 0, NULL);
	while (HeapTupleIsValid(tuple = heap_getnext(scan, 0, &buffer)))
	{
		datum = heap_getattr(tuple, Anum_pg_shadow_usename, pg_shadow_dsc, &n);

		if (!strncmp((char *) datum, stmt->user, strlen(stmt->user)))
		{
			exists = true;
			ReleaseBuffer(buffer);
			break;
		}
	}
	heap_endscan(scan);

	if (!exists)
	{
		RelationUnsetLockForWrite(pg_shadow_rel);
		heap_close(pg_shadow_rel);
		UserAbortTransactionBlock();
		elog(ERROR, "alterUser: user \"%s\" does not exist", stmt->user);
		return;
	}

	/*
	 * Create the update statement to modify the user.
	 */
	sprintf(sql, "update %s set", ShadowRelationName);
	sql_end = sql;
	if (stmt->password)
	{
		sql_end += strlen(sql_end);
		sprintf(sql_end, " passwd = '%s'", stmt->password);
	}
	if (stmt->createdb)
	{
		if (sql_end != sql)
			strcat(sql_end, ",");
		sql_end += strlen(sql_end);
		if (*stmt->createdb)
			strcat(sql_end, " usecreatedb = 't'");
		else
			strcat(sql_end, " usecreatedb = 'f'");
	}
	if (stmt->createuser)
	{
		if (sql_end != sql)
			strcat(sql_end, ",");
		sql_end += strlen(sql_end);
		if (*stmt->createuser)
			strcat(sql_end, " usesuper = 't'");
		else
			strcat(sql_end, " usesuper = 'f'");
	}
	if (stmt->validUntil)
	{
		if (sql_end != sql)
			strcat(sql_end, ",");
		sql_end += strlen(sql_end);
		sprintf(sql_end, " valuntil = '%s'", stmt->validUntil);
	}
	if (sql_end != sql)
	{
		sql_end += strlen(sql_end);
		sprintf(sql_end, " where usename = '%s'", stmt->user);
		pg_exec_query(sql, (char **) NULL, (Oid *) NULL, 0);
	}

	/* do the pg_group stuff here */

	UpdatePgPwdFile(sql);

	RelationUnsetLockForWrite(pg_shadow_rel);
	heap_close(pg_shadow_rel);

	if (IsTransactionBlock() && !inblock)
		EndTransactionBlock();
}


extern void
RemoveUser(char *user)
{

	char	   *pg_user;
	Relation	pg_shadow_rel,
				pg_rel;
	TupleDesc	pg_dsc;
	HeapScanDesc scan;
	HeapTuple	tuple;
	Datum		datum;
	Buffer		buffer;
	char		sql[512];
	bool		n,
				inblock;
	int			usesysid = -1,
				ndbase = 0;
	char	  **dbase = NULL;

	if (!(inblock = IsTransactionBlock()))
		BeginTransactionBlock();

	/*
	 * Make sure the user attempting to create a user can delete from the
	 * pg_shadow relation.
	 */
	pg_user = GetPgUserName();
	if (pg_aclcheck(ShadowRelationName, pg_user, ACL_RD | ACL_WR) != ACLCHECK_OK)
	{
		UserAbortTransactionBlock();
		elog(ERROR, "removeUser: user \"%s\" does not have SELECT and DELETE privilege for \"%s\"",
			 pg_user, ShadowRelationName);
		return;
	}

	/*
	 * Perform a scan of the pg_shadow relation to find the usesysid of
	 * the user to be deleted.	If it is not found, then return a warning
	 * message.
	 */
	pg_shadow_rel = heap_openr(ShadowRelationName);
	pg_dsc = RelationGetTupleDescriptor(pg_shadow_rel);

	/*
	 * Secure a write lock on pg_shadow so we can be sure that when the
	 * dump of the pg_pwd file is done, there is not another backend doing
	 * the same.
	 */
	RelationSetLockForWrite(pg_shadow_rel);

	scan = heap_beginscan(pg_shadow_rel, false, false, 0, NULL);
	while (HeapTupleIsValid(tuple = heap_getnext(scan, 0, &buffer)))
	{
		datum = heap_getattr(tuple, Anum_pg_shadow_usename, pg_dsc, &n);

		if (!strncmp((char *) datum, user, strlen(user)))
		{
			usesysid = (int) heap_getattr(tuple, Anum_pg_shadow_usesysid, pg_dsc, &n);
			ReleaseBuffer(buffer);
			break;
		}
		ReleaseBuffer(buffer);
	}
	heap_endscan(scan);

	if (usesysid == -1)
	{
		RelationUnsetLockForWrite(pg_shadow_rel);
		heap_close(pg_shadow_rel);
		UserAbortTransactionBlock();
		elog(ERROR, "removeUser: user \"%s\" does not exist", user);
		return;
	}

	/*
	 * Perform a scan of the pg_database relation to find the databases
	 * owned by usesysid.  Then drop them.
	 */
	pg_rel = heap_openr(DatabaseRelationName);
	pg_dsc = RelationGetTupleDescriptor(pg_rel);

	scan = heap_beginscan(pg_rel, false, false, 0, NULL);
	while (HeapTupleIsValid(tuple = heap_getnext(scan, 0, &buffer)))
	{
		datum = heap_getattr(tuple, Anum_pg_database_datdba, pg_dsc, &n);

		if ((int) datum == usesysid)
		{
			datum = heap_getattr(tuple, Anum_pg_database_datname, pg_dsc, &n);
			if (memcmp((void *) datum, "template1", 9))
			{
				dbase = (char **) realloc((void *) dbase, sizeof(char *) * (ndbase + 1));
				dbase[ndbase] = (char *) malloc(NAMEDATALEN + 1);
				memcpy((void *) dbase[ndbase], (void *) datum, NAMEDATALEN);
				dbase[ndbase++][NAMEDATALEN] = '\0';
			}
		}
		ReleaseBuffer(buffer);
	}
	heap_endscan(scan);
	heap_close(pg_rel);

	while (ndbase--)
	{
		elog(NOTICE, "Dropping database %s", dbase[ndbase]);
		sprintf(sql, "drop database %s", dbase[ndbase]);
		free((void *) dbase[ndbase]);
		pg_exec_query(sql, (char **) NULL, (Oid *) NULL, 0);
	}
	if (dbase)
		free((void *) dbase);

	/*
	 * Since pg_shadow is global over all databases, one of two things
	 * must be done to insure complete consistency.  First, pg_shadow
	 * could be made non-global. This would elminate the code above for
	 * deleting database and would require the addition of code to delete
	 * tables, views, etc owned by the user.
	 *
	 * The second option would be to create a means of deleting tables, view,
	 * etc. owned by the user from other databases.  Pg_user is global and
	 * so this must be done at some point.
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
	sprintf(sql, "delete from %s where usename = '%s'", ShadowRelationName, user);
	pg_exec_query(sql, (char **) NULL, (Oid *) NULL, 0);

	UpdatePgPwdFile(sql);

	RelationUnsetLockForWrite(pg_shadow_rel);
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
	HeapTuple	htp;

	htp = SearchSysCacheTuple(RELNAME, PointerGetDatum(ShadowRelationName),
							  0, 0, 0);
	if (!HeapTupleIsValid(htp))
	{
		elog(ERROR, "IsPgUserAclNull: class \"%s\" not found",
			 ShadowRelationName);
	}

	if (heap_attisnull(htp, Anum_pg_class_relacl))
	{
		elog(NOTICE, "To use passwords, you have to revoke permissions on pg_shadow");
		elog(NOTICE, "so normal users can not read the passwords.");
		elog(ERROR, "Try 'REVOKE ALL ON pg_shadow FROM PUBLIC'");
	}

	return;
}
