/*-------------------------------------------------------------------------
 *
 * user.c
 *	  use pg_exec_query to create a new user in the catalog
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: user.c,v 1.51 2000/03/15 07:02:56 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/pg_database.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_group.h"
#include "catalog/indexing.h"
#include "commands/copy.h"
#include "commands/user.h"
#include "commands/trigger.h"
#include "libpq/crypt.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

static void CheckPgUserAclNotNull(void);

#define SQL_LENGTH	512

/*---------------------------------------------------------------------
 * write_password_file / update_pg_pwd
 *
 * copy the modified contents of pg_shadow to a file used by the postmaster
 * for user authentication.  The file is stored as $PGDATA/pg_pwd.
 *
 * This function set is both a trigger function for direct updates to pg_shadow
 * as well as being called directly from create/alter/drop user.
 *---------------------------------------------------------------------
 */
static void
write_password_file(Relation rel)
{
	char	   *filename,
			   *tempname;
	int			bufsize;
    FILE       *fp;
    mode_t      oumask;
    HeapScanDesc scan;
    HeapTuple   tuple;
	TupleDesc dsc = RelationGetDescr(rel);

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
        elog(ERROR, "%s: %s", tempname, strerror(errno));

    /* read table */
    scan = heap_beginscan(rel, false, SnapshotSelf, 0, NULL);
    while (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
    {
        Datum		datum_n, datum_p, datum_v;
        bool        null_n, null_p, null_v;

		datum_n = heap_getattr(tuple, Anum_pg_shadow_usename, dsc, &null_n);
        if (null_n)
            continue; /* don't allow empty users */
		datum_p = heap_getattr(tuple, Anum_pg_shadow_passwd, dsc, &null_p);
        /* It could be argued that people having a null password
           shouldn't be allowed to connect, because they need
           to have a password set up first. If you think assuming
           an empty password in that case is better, erase the following line. */
        if (null_p)
            continue;
		datum_v = heap_getattr(tuple, Anum_pg_shadow_valuntil, dsc, &null_v);

        /* These fake entries are not really necessary. To remove them, the parser
           in backend/libpq/crypt.c would need to be adjusted. Initdb might also
           need adjustments. */
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
                nameout(DatumGetName(datum_n)),
                null_p ? "" : textout((text*)datum_p),
                null_v ? "\\N" : nabstimeout((AbsoluteTime)datum_v) /* this is how the parser wants it */
                );
        if (ferror(fp))
            elog(ERROR, "%s: %s", tempname, strerror(errno));
        fflush(fp);
    }
    heap_endscan(scan);
    FreeFile(fp);

    /*
     * And rename the temp file to its final name, deleting the old pg_pwd.
     */
    rename(tempname, filename);

    /*
	 * Create a flag file the postmaster will detect the next time it
	 * tries to authenticate a user.  The postmaster will know to reload
	 * the pg_pwd file contents.
	 */
	filename = crypt_getpwdreloadfilename();
	if (creat(filename, S_IRUSR | S_IWUSR) == -1)
        elog(ERROR, "%s: %s", filename, strerror(errno));

	pfree((void *) tempname);
}



/* This is the wrapper for triggers. */
HeapTuple
update_pg_pwd(void)
{
    Relation rel = heap_openr(ShadowRelationName,  AccessExclusiveLock);
    write_password_file(rel);
    heap_close(rel,  AccessExclusiveLock);

	/*
	 * This is a trigger, so clean out the information provided by
	 * the trigger manager.
	 */
	CurrentTriggerData = NULL;
    return NULL;
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
    Datum       new_record[Natts_pg_shadow];
    char        new_record_nulls[Natts_pg_shadow];
	bool		user_exists = false,
                sysid_exists = false,
                havesysid;
	int			max_id = -1;
    List       *item;

    havesysid    = stmt->sysid > 0;

    /* Check some permissions first */
	if (stmt->password)
		CheckPgUserAclNotNull();

    if (!superuser())
        elog(ERROR, "CREATE USER: permission denied");

    /* The reason for the following is this:
     * If you start a transaction block, create a user, then roll back the
     * transaction, the pg_pwd won't get rolled back due to a bug in the
     * Unix file system ( :}). Hence this is in the interest of security.
     */
	if (IsTransactionBlock())
        elog(ERROR, "CREATE USER: may not be called in a transaction block");

	/*
	 * Scan the pg_shadow relation to be certain the user or id doesn't already
	 * exist.  Note we secure exclusive lock, because we also need to be
	 * sure of what the next usesysid should be, and we need to protect
	 * our update of the flat password file.
	 */
	pg_shadow_rel = heap_openr(ShadowRelationName, AccessExclusiveLock);
	pg_shadow_dsc = RelationGetDescr(pg_shadow_rel);

	scan = heap_beginscan(pg_shadow_rel, false, SnapshotNow, 0, NULL);
	while (!user_exists && !sysid_exists && HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
	{
        Datum		datum;
        bool        null;

		datum = heap_getattr(tuple, Anum_pg_shadow_usename, pg_shadow_dsc, &null);
		user_exists = datum && !null && (strcmp((char *) datum, stmt->user) == 0);

		datum = heap_getattr(tuple, Anum_pg_shadow_usesysid, pg_shadow_dsc, &null);
        if (havesysid) /* customized id wanted */
            sysid_exists = datum && !null && ((int)datum == stmt->sysid);
        else /* pick 1 + max */
        {
            if ((int) datum > max_id)
                max_id = (int) datum;
        }
	}
	heap_endscan(scan);

	if (user_exists || sysid_exists)
	{
		heap_close(pg_shadow_rel, AccessExclusiveLock);
        if (user_exists)
            elog(ERROR, "CREATE USER: user name \"%s\" already exists", stmt->user);
        else
            elog(ERROR, "CREATE USER: sysid %d is already assigned", stmt->sysid);
		return;
	}

    /*
     * Build a tuple to insert
     */
    new_record[Anum_pg_shadow_usename-1] = PointerGetDatum(namein(stmt->user)); /* this truncated properly */
    new_record[Anum_pg_shadow_usesysid-1] = Int32GetDatum(havesysid ? stmt->sysid : max_id + 1);

    AssertState(BoolIsValid(stmt->createdb));
    new_record[Anum_pg_shadow_usecreatedb-1] = (Datum)(stmt->createdb);
    new_record[Anum_pg_shadow_usetrace-1] = (Datum)(false);
    AssertState(BoolIsValid(stmt->createuser));
    new_record[Anum_pg_shadow_usesuper-1] = (Datum)(stmt->createuser);
    /* superuser gets catupd right by default */
    new_record[Anum_pg_shadow_usecatupd-1] = (Datum)(stmt->createuser);

    if (stmt->password)
        new_record[Anum_pg_shadow_passwd-1] = PointerGetDatum(textin(stmt->password));
    if (stmt->validUntil)
        new_record[Anum_pg_shadow_valuntil-1] = PointerGetDatum(nabstimein(stmt->validUntil));

    new_record_nulls[Anum_pg_shadow_usename-1] = ' ';
    new_record_nulls[Anum_pg_shadow_usesysid-1] = ' ';

    new_record_nulls[Anum_pg_shadow_usecreatedb-1] = ' ';
    new_record_nulls[Anum_pg_shadow_usetrace-1] = ' ';
    new_record_nulls[Anum_pg_shadow_usesuper-1] = ' ';
    new_record_nulls[Anum_pg_shadow_usecatupd-1] = ' ';

    new_record_nulls[Anum_pg_shadow_passwd-1] = stmt->password ? ' ' : 'n';
    new_record_nulls[Anum_pg_shadow_valuntil-1] = stmt->validUntil ? ' ' : 'n';

    tuple = heap_formtuple(pg_shadow_dsc, new_record, new_record_nulls);
    Assert(tuple);

    /*
     * Insert a new record in the pg_shadow table
     */
    if (heap_insert(pg_shadow_rel, tuple) == InvalidOid)
        elog(ERROR, "CREATE USER: heap_insert failed");

    /*
     * Update indexes
     */
    if (RelationGetForm(pg_shadow_rel)->relhasindex) {
        Relation idescs[Num_pg_shadow_indices];
      
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
    foreach(item, stmt->groupElts)
    {
        AlterGroupStmt ags;

        ags.name = strVal(lfirst(item)); /* the group name to add this in */
        ags.action = +1;
        ags.listUsers = lcons((void*)makeInteger(havesysid ? stmt->sysid : max_id + 1), NIL);
        AlterGroup(&ags, "CREATE USER");
    }

	/*
	 * Write the updated pg_shadow data to the flat password file.
	 */
    write_password_file(pg_shadow_rel);
	/*
	 * Now we can clean up.
	 */
	heap_close(pg_shadow_rel, AccessExclusiveLock);
}



/*
 * ALTER USER
 */
extern void
AlterUser(AlterUserStmt *stmt)
{
    Datum       new_record[Natts_pg_shadow];
    char        new_record_nulls[Natts_pg_shadow];
	Relation	pg_shadow_rel;
	TupleDesc	pg_shadow_dsc;
	HeapTuple	tuple, new_tuple;
    bool        null;

	if (stmt->password)
		CheckPgUserAclNotNull();

    /* must be superuser or just want to change your own password */
    if (!superuser() &&
        !(stmt->createdb==0 && stmt->createuser==0 && !stmt->validUntil
          && stmt->password && strcmp(GetPgUserName(), stmt->user)==0))
        elog(ERROR, "ALTER USER: permission denied");

    /* see comments in create user */
	if (IsTransactionBlock())
        elog(ERROR, "ALTER USER: may not be called in a transaction block");

	/*
	 * Scan the pg_shadow relation to be certain the user exists.
	 * Note we secure exclusive lock to protect our update of the
	 * flat password file.
	 */
	pg_shadow_rel = heap_openr(ShadowRelationName, AccessExclusiveLock);
	pg_shadow_dsc = RelationGetDescr(pg_shadow_rel);

	tuple = SearchSysCacheTuple(SHADOWNAME,
								PointerGetDatum(stmt->user),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
	{
		heap_close(pg_shadow_rel, AccessExclusiveLock);
		elog(ERROR, "ALTER USER: user \"%s\" does not exist", stmt->user);
	}

    /*
     * Build a tuple to update, perusing the information just obtained
     */
    new_record[Anum_pg_shadow_usename-1] = PointerGetDatum(namein(stmt->user));
    new_record_nulls[Anum_pg_shadow_usename-1] = ' ';

    /* sysid - leave as is */
    new_record[Anum_pg_shadow_usesysid-1] = heap_getattr(tuple, Anum_pg_shadow_usesysid, pg_shadow_dsc, &null);
    new_record_nulls[Anum_pg_shadow_usesysid-1] = null ? 'n' : ' ';

    /* createdb */
    if (stmt->createdb == 0)
    {
        /* don't change */
        new_record[Anum_pg_shadow_usecreatedb-1] = heap_getattr(tuple, Anum_pg_shadow_usecreatedb, pg_shadow_dsc, &null);
        new_record_nulls[Anum_pg_shadow_usecreatedb-1] = null ? 'n' : ' ';
    }
    else
    {
        new_record[Anum_pg_shadow_usecreatedb-1] = (Datum)(stmt->createdb > 0 ? true : false);
        new_record_nulls[Anum_pg_shadow_usecreatedb-1] = ' ';
    }

    /* trace - leave as is */
    new_record[Anum_pg_shadow_usetrace-1] = heap_getattr(tuple, Anum_pg_shadow_usetrace, pg_shadow_dsc, &null);
    new_record_nulls[Anum_pg_shadow_usetrace-1] = null ? 'n' : ' ';

    /* createuser (superuser) */
    if (stmt->createuser == 0)
    {
        /* don't change */
        new_record[Anum_pg_shadow_usesuper-1] = heap_getattr(tuple, Anum_pg_shadow_usesuper, pg_shadow_dsc, &null);
        new_record_nulls[Anum_pg_shadow_usesuper-1] = null ? 'n' : ' ';
    }
    else
    {
        new_record[Anum_pg_shadow_usesuper-1] = (Datum)(stmt->createuser > 0 ? true : false);
        new_record_nulls[Anum_pg_shadow_usesuper-1] = ' ';
    }

    /* catupd - set to false if someone's superuser priv is being yanked */
    if (stmt->createuser < 0)
    {
        new_record[Anum_pg_shadow_usecatupd-1] = (Datum)(false);
        new_record_nulls[Anum_pg_shadow_usecatupd-1] = ' ';
    }
    else
    {
        /* leave alone */
        new_record[Anum_pg_shadow_usecatupd-1] = heap_getattr(tuple, Anum_pg_shadow_usecatupd, pg_shadow_dsc, &null);
        new_record_nulls[Anum_pg_shadow_usecatupd-1] = null ? 'n' : ' ';
    }

    /* password */
    if (stmt->password)
    {
        new_record[Anum_pg_shadow_passwd-1] = PointerGetDatum(textin(stmt->password));
        new_record_nulls[Anum_pg_shadow_passwd-1] = ' ';
    }
    else
    {
        /* leave as is */
        new_record[Anum_pg_shadow_passwd-1] = heap_getattr(tuple, Anum_pg_shadow_passwd, pg_shadow_dsc, &null);
        new_record_nulls[Anum_pg_shadow_passwd-1] = null ? 'n' : ' ';
    }

    /* valid until */
    if (stmt->validUntil)
    {    
        new_record[Anum_pg_shadow_valuntil-1] = PointerGetDatum(nabstimein(stmt->validUntil));
        new_record_nulls[Anum_pg_shadow_valuntil-1] = ' ';
    }
    else
    {
        /* leave as is */
        new_record[Anum_pg_shadow_valuntil-1] = heap_getattr(tuple, Anum_pg_shadow_valuntil, pg_shadow_dsc, &null);
        new_record_nulls[Anum_pg_shadow_valuntil-1] = null ? 'n' : ' ';
    }

    new_tuple = heap_formtuple(pg_shadow_dsc, new_record, new_record_nulls);
    Assert(new_tuple);
    /* XXX check return value of this? */
    heap_update(pg_shadow_rel, &tuple->t_self, new_tuple, NULL);


    /* Update indexes */
    if (RelationGetForm(pg_shadow_rel)->relhasindex)
    {
        Relation idescs[Num_pg_shadow_indices];
      
        CatalogOpenIndices(Num_pg_shadow_indices, 
                           Name_pg_shadow_indices, idescs);
        CatalogIndexInsert(idescs, Num_pg_shadow_indices, pg_shadow_rel, 
                           tuple);
        CatalogCloseIndices(Num_pg_shadow_indices, idescs);
    }

	/*
	 * Write the updated pg_shadow data to the flat password file.
	 */
    write_password_file(pg_shadow_rel);

	/*
	 * Now we can clean up.
	 */
	heap_close(pg_shadow_rel, AccessExclusiveLock);

}



/*
 * DROP USER
 */
void
DropUser(DropUserStmt *stmt)
{
	Relation	pg_shadow_rel;
	TupleDesc	pg_shadow_dsc;
    List       *item;

    if (!superuser())
        elog(ERROR, "DROP USER: permission denied");

	if (IsTransactionBlock())
        elog(ERROR, "DROP USER: may not be called in a transaction block");

	/*
	 * Scan the pg_shadow relation to find the usesysid of the user to be
	 * deleted.  Note we secure exclusive lock, because we need to protect
	 * our update of the flat password file.
	 */
	pg_shadow_rel = heap_openr(ShadowRelationName, AccessExclusiveLock);
	pg_shadow_dsc = RelationGetDescr(pg_shadow_rel);

    foreach(item, stmt->users)
    {
        HeapTuple	tuple,
            tmp_tuple;
        Relation    pg_rel;
        TupleDesc   pg_dsc;
        ScanKeyData scankey;
        HeapScanDesc scan;
        Datum		datum;
        bool		null;
        int32		usesysid;
        const char *user = strVal(lfirst(item));

        tuple = SearchSysCacheTuple(SHADOWNAME,
                                    PointerGetDatum(user),
                                    0, 0, 0);
        if (!HeapTupleIsValid(tuple))
        {
            heap_close(pg_shadow_rel, AccessExclusiveLock);
            elog(ERROR, "DROP USER: user \"%s\" does not exist%s", user,
                 (length(stmt->users) > 1) ? " (no users removed)" : "");
        }

        usesysid = DatumGetInt32(heap_getattr(tuple, Anum_pg_shadow_usesysid, pg_shadow_dsc, &null));

        /*-------------------
         * Check if user still owns a database. If so, error out.
         *
         * (It used to be that this function would drop the database automatically.
         *  This is not only very dangerous for people that don't read the manual,
         *  it doesn't seem to be the behaviour one would expect either.)
         *                                                   -- petere 2000/01/14)
         *-------------------*/
        pg_rel = heap_openr(DatabaseRelationName, AccessExclusiveLock);
        pg_dsc = RelationGetDescr(pg_rel);

        ScanKeyEntryInitialize(&scankey, 0x0, Anum_pg_database_datdba, F_INT4EQ,
                               Int32GetDatum(usesysid));

        scan = heap_beginscan(pg_rel, false, SnapshotNow, 1, &scankey);

        if (HeapTupleIsValid(tmp_tuple = heap_getnext(scan, 0)))
        {
            datum = heap_getattr(tmp_tuple, Anum_pg_database_datname, pg_dsc, &null);
            heap_close(pg_shadow_rel, AccessExclusiveLock);
            elog(ERROR, "DROP USER: user \"%s\" owns database \"%s\", cannot be removed%s",
                 user, nameout(DatumGetName(datum)),
                 (length(stmt->users) > 1) ? " (no users removed)" : ""
                );
        }
            
        heap_endscan(scan);
        heap_close(pg_rel, AccessExclusiveLock);

        /*
         * Somehow we'd have to check for tables, views, etc. owned by the user
         * as well, but those could be spread out over all sorts of databases
         * which we don't have access to (easily).
         */

        /*
         * Remove the user from the pg_shadow table
         */
        heap_delete(pg_shadow_rel, &tuple->t_self, NULL);

        /*
         * Remove user from groups
         *
         * try calling alter group drop user for every group
         */
        pg_rel = heap_openr(GroupRelationName, AccessExclusiveLock);
        pg_dsc = RelationGetDescr(pg_rel);
        scan = heap_beginscan(pg_rel, false, SnapshotNow, 0, NULL);
        while (HeapTupleIsValid(tmp_tuple = heap_getnext(scan, 0)))
        {
            AlterGroupStmt ags;

            datum = heap_getattr(tmp_tuple, Anum_pg_group_groname, pg_dsc, &null);

            ags.name = nameout(DatumGetName(datum)); /* the group name from which to try to drop the user */
            ags.action = -1;
            ags.listUsers = lcons((void*)makeInteger(usesysid), NIL);
            AlterGroup(&ags, "DROP USER");
        }
        heap_endscan(scan);
        heap_close(pg_rel, AccessExclusiveLock);        
    }

	/*
	 * Write the updated pg_shadow data to the flat password file.
	 */
    write_password_file(pg_shadow_rel);

    /*
     * Now we can clean up.
     */
    heap_close(pg_shadow_rel, AccessExclusiveLock);
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
        /* BIG problem */
		elog(ERROR, "IsPgUserAclNull: \"%s\" not found",
			 ShadowRelationName);
	}

	if (heap_attisnull(htup, Anum_pg_class_relacl))
	{
		elog(ERROR,
             "To use passwords, you have to revoke permissions on %s "
             "so normal users cannot read the passwords. "
             "Try 'REVOKE ALL ON \"%s\" FROM PUBLIC'.",
             ShadowRelationName, ShadowRelationName);
	}

	return;
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
    TupleDesc   pg_group_dsc;
    bool        group_exists = false,
                sysid_exists = false;
    int         max_id = 0;
    Datum       new_record[Natts_pg_group];
    char        new_record_nulls[Natts_pg_group];
    List       *item, *newlist=NULL;
    ArrayType  *userarray;

	/*
	 * Make sure the user can do this.
	 */
	if (!superuser())
		elog(ERROR, "CREATE GROUP: permission denied");

    /*
     * There is not real reason for this, but it makes it consistent
     * with create user, and it seems like a good idea anyway.
     */
	if (IsTransactionBlock())
        elog(ERROR, "CREATE GROUP: may not be called in a transaction block");


	pg_group_rel = heap_openr(GroupRelationName, AccessExclusiveLock);
	pg_group_dsc = RelationGetDescr(pg_group_rel);

	scan = heap_beginscan(pg_group_rel, false, SnapshotNow, 0, NULL);
	while (!group_exists && !sysid_exists && HeapTupleIsValid(tuple = heap_getnext(scan, false)))
	{
        Datum		datum;
        bool        null;

		datum = heap_getattr(tuple, Anum_pg_group_groname, pg_group_dsc, &null);
		group_exists = datum && !null && (strcmp((char *) datum, stmt->name) == 0);

		datum = heap_getattr(tuple, Anum_pg_group_grosysid, pg_group_dsc, &null);
        if (stmt->sysid >= 0) /* customized id wanted */
            sysid_exists = datum && !null && ((int)datum == stmt->sysid);
        else /* pick 1 + max */
        {
            if ((int) datum > max_id)
                max_id = (int) datum;
        }
	}
	heap_endscan(scan);

	if (group_exists || sysid_exists)
	{
		heap_close(pg_group_rel, AccessExclusiveLock);
        if (group_exists)
            elog(ERROR, "CREATE GROUP: group name \"%s\" already exists", stmt->name);
        else
            elog(ERROR, "CREATE GROUP: group sysid %d is already assigned", stmt->sysid);
	}

    /*
     * Translate the given user names to ids
     */

    foreach(item, stmt->initUsers)
    {
        const char * groupuser = strVal(lfirst(item));
        Value *v;

        tuple = SearchSysCacheTuple(SHADOWNAME,
                                    PointerGetDatum(groupuser),
                                    0, 0, 0);
        if (!HeapTupleIsValid(tuple))
        {
            heap_close(pg_group_rel, AccessExclusiveLock);
            elog(ERROR, "CREATE GROUP: user \"%s\" does not exist", groupuser);
        }

        v = makeInteger(((Form_pg_shadow) GETSTRUCT(tuple))->usesysid);
        if (!member(v, newlist))
            newlist = lcons(v, newlist);
    }

    /* build an array to insert */
    if (newlist)
    {
        int i;

        userarray = palloc(ARR_OVERHEAD(1) + length(newlist) * sizeof(int32));
        ARR_SIZE(userarray) = ARR_OVERHEAD(1) + length(newlist) * sizeof(int32);
        ARR_FLAGS(userarray) = 0x0;
        ARR_NDIM(userarray) = 1; /* one dimensional array */
        ARR_LBOUND(userarray)[0] = 1; /* axis starts at one */
        ARR_DIMS(userarray)[0] = length(newlist); /* axis is this long */
        /* fill the array */
        i = 0;
        foreach(item, newlist)
        {
            ((int*)ARR_DATA_PTR(userarray))[i++] = intVal(lfirst(item));
        }
    }
    else
        userarray = NULL;

    /*
     * Form a tuple to insert
     */
    if (stmt->sysid >=0)
        max_id = stmt->sysid;
    else 
        max_id++;

    new_record[Anum_pg_group_groname-1] = (Datum)(stmt->name);
    new_record[Anum_pg_group_grosysid-1] = (Datum)(max_id);
    new_record[Anum_pg_group_grolist-1] = (Datum)userarray;

    new_record_nulls[Anum_pg_group_groname-1] = ' ';
    new_record_nulls[Anum_pg_group_grosysid-1] = ' ';
    new_record_nulls[Anum_pg_group_grolist-1] = userarray ? ' ' : 'n';

    tuple = heap_formtuple(pg_group_dsc, new_record, new_record_nulls);

    /*
     * Insert a new record in the pg_group_table
     */
    heap_insert(pg_group_rel, tuple);

    /*
     * Update indexes
     */
    if (RelationGetForm(pg_group_rel)->relhasindex) {
        Relation idescs[Num_pg_group_indices];
      
        CatalogOpenIndices(Num_pg_group_indices, 
                           Name_pg_group_indices, idescs);
        CatalogIndexInsert(idescs, Num_pg_group_indices, pg_group_rel, 
                           tuple);
        CatalogCloseIndices(Num_pg_group_indices, idescs);
    }

	heap_close(pg_group_rel, AccessExclusiveLock);
}



/*
 * ALTER GROUP
 */
void
AlterGroup(AlterGroupStmt *stmt, const char * tag)
{
	Relation	pg_group_rel;
    TupleDesc   pg_group_dsc;
    HeapTuple   group_tuple;

    /*
	 * Make sure the user can do this.
	 */
	if (!superuser())
		elog(ERROR, "%s: permission denied", tag);

    /*
     * There is not real reason for this, but it makes it consistent
     * with alter user, and it seems like a good idea anyway.
     */
	if (IsTransactionBlock())
        elog(ERROR, "%s: may not be called in a transaction block", tag);


    pg_group_rel = heap_openr(GroupRelationName, AccessExclusiveLock);
    pg_group_dsc = RelationGetDescr(pg_group_rel);

    /*
     * Verify that group exists.
     * If we find a tuple, will take that the rest of the way and make our
     * modifications on it.
     */
    if (!HeapTupleIsValid(group_tuple = SearchSysCacheTupleCopy(GRONAME, PointerGetDatum(stmt->name), 0, 0, 0)))
	{
        heap_close(pg_group_rel, AccessExclusiveLock);
		elog(ERROR, "%s: group \"%s\" does not exist", tag, stmt->name);
	}

    AssertState(stmt->action == +1 || stmt->action == -1);
    /*
     * Now decide what to do.
     */
    if (stmt->action == +1) /* add users, might also be invoked by create user */
    {
        Datum       new_record[Natts_pg_group];
        char        new_record_nulls[Natts_pg_group] = { ' ', ' ', ' '};
        ArrayType *newarray, *oldarray;
        List * newlist = NULL, *item;
        HeapTuple tuple;
        bool null = false;
        Datum datum = heap_getattr(group_tuple, Anum_pg_group_grolist, pg_group_dsc, &null);
        int i;
        
        oldarray = (ArrayType*)datum;
        Assert(null || ARR_NDIM(oldarray) == 1);
        /* first add the old array to the hitherto empty list */
        if (!null)
            for (i = ARR_LBOUND(oldarray)[0]; i < ARR_LBOUND(oldarray)[0] + ARR_DIMS(oldarray)[0]; i++)
            {
                int index, arrval;
                Value *v;
                bool valueNull;
                index = i;
                arrval = DatumGetInt32(array_ref(oldarray, 1, &index, true/*by value*/,
                                                 sizeof(int), 0, &valueNull));
                v = makeInteger(arrval);
                /* filter out duplicates */
                if (!member(v, newlist))
                    newlist = lcons(v, newlist);
            }

        /* 
         * now convert the to be added usernames to sysids and add them
         * to the list
         */
        foreach(item, stmt->listUsers)
        {
            Value *v;

            if (strcmp(tag, "ALTER GROUP")==0)
            {
                /* Get the uid of the proposed user to add. */
                tuple = SearchSysCacheTuple(SHADOWNAME,
                                            PointerGetDatum(strVal(lfirst(item))),
                                            0, 0, 0);
                if (!HeapTupleIsValid(tuple))
                {
                    heap_close(pg_group_rel, AccessExclusiveLock);
                    elog(ERROR, "%s: user \"%s\" does not exist", tag, strVal(lfirst(item)));
                }
                v = makeInteger(((Form_pg_shadow) GETSTRUCT(tuple))->usesysid);
            }
            else if (strcmp(tag, "CREATE USER")==0)
            {
                /* in this case we already know the uid and it wouldn't
                   be in the cache anyway yet */
                v = lfirst(item);
            }
            else
			{
                elog(ERROR, "AlterGroup: unknown tag %s", tag);
				v = NULL;		/* keep compiler quiet */
			}

            if (!member(v, newlist))
                newlist = lcons(v, newlist);
            else
                /* we silently assume here that this error will only come up
                   in a ALTER GROUP statement */
                elog(NOTICE, "%s: user \"%s\" is already in group \"%s\"", tag, strVal(lfirst(item)), stmt->name);
        }
             
        newarray = palloc(ARR_OVERHEAD(1) + length(newlist) * sizeof(int32));
        ARR_SIZE(newarray) = ARR_OVERHEAD(1) + length(newlist) * sizeof(int32);
        ARR_FLAGS(newarray) = 0x0;
        ARR_NDIM(newarray) = 1; /* one dimensional array */
        ARR_LBOUND(newarray)[0] = 1; /* axis starts at one */
        ARR_DIMS(newarray)[0] = length(newlist); /* axis is this long */
        /* fill the array */
        i = 0;
        foreach(item, newlist)
        {
            ((int*)ARR_DATA_PTR(newarray))[i++] = intVal(lfirst(item));
        }
        
        /*
         * Form a tuple with the new array and write it back.
         */
        new_record[Anum_pg_group_groname-1] = (Datum)(stmt->name);
        new_record[Anum_pg_group_grosysid-1] = heap_getattr(group_tuple, Anum_pg_group_grosysid, pg_group_dsc, &null);
        new_record[Anum_pg_group_grolist-1] = PointerGetDatum(newarray);

        tuple = heap_formtuple(pg_group_dsc, new_record, new_record_nulls);
        heap_update(pg_group_rel, &group_tuple->t_self, tuple, NULL);

        /* Update indexes */
        if (RelationGetForm(pg_group_rel)->relhasindex) {
            Relation idescs[Num_pg_group_indices];
      
            CatalogOpenIndices(Num_pg_group_indices, 
                               Name_pg_group_indices, idescs);
            CatalogIndexInsert(idescs, Num_pg_group_indices, pg_group_rel, 
                               tuple);
            CatalogCloseIndices(Num_pg_group_indices, idescs);
        }
    } /* endif alter group add user */

    else if (stmt->action == -1) /*drop users from group */
    {
        Datum         datum;
        bool          null;
        bool          is_dropuser = strcmp(tag, "DROP USER")==0;
        
        datum = heap_getattr(group_tuple, Anum_pg_group_grolist, pg_group_dsc, &null);
        if (null)
        {
            if (!is_dropuser)
                elog(NOTICE, "ALTER GROUP: group \"%s\" does not have any members", stmt->name);
        }
        else
        {
            HeapTuple	  tuple;
            Datum       new_record[Natts_pg_group];
            char        new_record_nulls[Natts_pg_group] = { ' ', ' ', ' '};
            ArrayType    *oldarray, *newarray;
            List * newlist = NULL, *item;
            int i;

            oldarray = (ArrayType*)datum;
            Assert(ARR_NDIM(oldarray) == 1);
            /* first add the old array to the hitherto empty list */
            for (i = ARR_LBOUND(oldarray)[0]; i < ARR_LBOUND(oldarray)[0] + ARR_DIMS(oldarray)[0]; i++)
            {
                int index, arrval;
                Value *v;
                bool valueNull;
                index = i;
                arrval = DatumGetInt32(array_ref(oldarray, 1, &index, true/*by value*/,
                                                 sizeof(int), 0, &valueNull));
                v = makeInteger(arrval);
                /* filter out duplicates */
                if (!member(v, newlist))
                    newlist = lcons(v, newlist);
            }

            /* 
             * now convert the to be dropped usernames to sysids and remove
             * them from the list
             */
            foreach(item, stmt->listUsers)
            {
                Value *v;
                if (!is_dropuser)
                {
                    /* Get the uid of the proposed user to drop. */
                    tuple = SearchSysCacheTuple(SHADOWNAME,
                                                PointerGetDatum(strVal(lfirst(item))),
                                                0, 0, 0);
                    if (!HeapTupleIsValid(tuple))
                    {
                        heap_close(pg_group_rel, AccessExclusiveLock);
                        elog(ERROR, "ALTER GROUP: user \"%s\" does not exist", strVal(lfirst(item)));
                    }
                    v = makeInteger(((Form_pg_shadow) GETSTRUCT(tuple))->usesysid);
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
            ARR_SIZE(newarray) = ARR_OVERHEAD(1) + length(newlist) * sizeof(int32);
            ARR_FLAGS(newarray) = 0x0;
            ARR_NDIM(newarray) = 1; /* one dimensional array */
            ARR_LBOUND(newarray)[0] = 1; /* axis starts at one */
            ARR_DIMS(newarray)[0] = length(newlist); /* axis is this long */
            /* fill the array */
            i = 0;
            foreach(item, newlist)
            {
                ((int*)ARR_DATA_PTR(newarray))[i++] = intVal(lfirst(item));
            }
        
            /*
             * Insert the new tuple with the updated user list
             */
            new_record[Anum_pg_group_groname-1] = (Datum)(stmt->name);
            new_record[Anum_pg_group_grosysid-1] = heap_getattr(group_tuple, Anum_pg_group_grosysid, pg_group_dsc, &null);
            new_record[Anum_pg_group_grolist-1] = PointerGetDatum(newarray);

            tuple = heap_formtuple(pg_group_dsc, new_record, new_record_nulls);
            heap_update(pg_group_rel, &group_tuple->t_self, tuple, NULL);

            /* Update indexes */
            if (RelationGetForm(pg_group_rel)->relhasindex) {
                Relation idescs[Num_pg_group_indices];
      
                CatalogOpenIndices(Num_pg_group_indices, 
                                   Name_pg_group_indices, idescs);
                CatalogIndexInsert(idescs, Num_pg_group_indices, pg_group_rel, 
                                   tuple);
                CatalogCloseIndices(Num_pg_group_indices, idescs);
            }

        } /* endif group not null */
    } /* endif alter group drop user */

    heap_close(pg_group_rel, AccessExclusiveLock);

    pfree(group_tuple);
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
    TupleDesc   pg_group_dsc;
    bool        gro_exists = false;

    /*
	 * Make sure the user can do this.
	 */
	if (!superuser())
		elog(ERROR, "DROP GROUP: permission denied");

    /*
     * There is not real reason for this, but it makes it consistent
     * with drop user, and it seems like a good idea anyway.
     */
	if (IsTransactionBlock())
        elog(ERROR, "DROP GROUP: may not be called in a transaction block");

    /*
     * Scan the pg_group table and delete all matching groups.
     */
	pg_group_rel = heap_openr(GroupRelationName, AccessExclusiveLock);
	pg_group_dsc = RelationGetDescr(pg_group_rel);
	scan = heap_beginscan(pg_group_rel, false, SnapshotNow, 0, NULL);

	while (HeapTupleIsValid(tuple = heap_getnext(scan, false)))
	{
        Datum datum;
        bool null;

        datum = heap_getattr(tuple, Anum_pg_group_groname, pg_group_dsc, &null);
        if (datum && !null && strcmp((char*)datum, stmt->name)==0)
        {
            gro_exists = true;
            heap_delete(pg_group_rel, &tuple->t_self, NULL);
        }
	}

	heap_endscan(scan);

    /*
     * Did we find any?
     */
    if (!gro_exists)
    {
        heap_close(pg_group_rel, AccessExclusiveLock);
		elog(ERROR, "DROP GROUP: group \"%s\" does not exist", stmt->name);
    }

	heap_close(pg_group_rel, AccessExclusiveLock);
}
