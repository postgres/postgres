/*-------------------------------------------------------------------------
 *
 * flatfiles.c
 *	  Routines for maintaining "flat file" images of the shared catalogs.
 *
 * We use flat files so that the postmaster and not-yet-fully-started
 * backends can look at the contents of pg_database, pg_shadow, and pg_group
 * for authentication purposes.  This module is responsible for keeping the
 * flat-file images as nearly in sync with database reality as possible.
 *
 * The tricky part of the write_xxx_file() routines in this module is that
 * they need to be able to operate in the context of the database startup
 * process (which calls BuildFlatFiles()) as well as a normal backend.
 * This means for example that we can't assume a fully functional relcache
 * and we can't use syscaches at all.  The major restriction imposed by
 * all that is that there's no way to read an out-of-line-toasted datum,
 * because the tuptoaster.c code is not prepared to cope with such an
 * environment.  Fortunately we can design the shared catalogs in such
 * a way that this is OK.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/backend/utils/init/flatfiles.c,v 1.2 2005/02/20 04:45:59 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/pg_database.h"
#include "catalog/pg_group.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_tablespace.h"
#include "commands/trigger.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/pmsignal.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/flatfiles.h"
#include "utils/resowner.h"
#include "utils/syscache.h"


#define DATABASE_FLAT_FILE	"pg_database"
#define GROUP_FLAT_FILE		"pg_group"
#define USER_FLAT_FILE		"pg_pwd"


/*
 * The need-to-update-files flags are SubTransactionIds that show
 * what level of the subtransaction tree requested the update. To register
 * an update, the subtransaction saves its own SubTransactionId in the flag,
 * unless the value was already set to a valid SubTransactionId (which implies
 * that it or a parent level has already requested the same).  If it aborts
 * and the value is its SubTransactionId, it resets the flag to
 * InvalidSubTransactionId. If it commits, it changes the value to its
 * parent's SubTransactionId.  This way the value is propagated up to the
 * top-level transaction, which will update the files if a valid
 * SubTransactionId is seen at top-level commit.
 */
static SubTransactionId database_file_update_subid = InvalidSubTransactionId;
static SubTransactionId group_file_update_subid = InvalidSubTransactionId;
static SubTransactionId user_file_update_subid = InvalidSubTransactionId;


/*
 * Mark flat database file as needing an update (because pg_database changed)
 */
void
database_file_update_needed(void)
{
	if (database_file_update_subid == InvalidSubTransactionId)
		database_file_update_subid = GetCurrentSubTransactionId();
}

/*
 * Mark flat group file as needing an update (because pg_group changed)
 */
void
group_file_update_needed(void)
{
	if (group_file_update_subid == InvalidSubTransactionId)
		group_file_update_subid = GetCurrentSubTransactionId();
}

/*
 * Mark flat user file as needing an update (because pg_shadow changed)
 */
void
user_file_update_needed(void)
{
	if (user_file_update_subid == InvalidSubTransactionId)
		user_file_update_subid = GetCurrentSubTransactionId();
}


/*
 * database_getflatfilename --- get full pathname of database file
 *
 * Note that result string is palloc'd, and should be freed by the caller.
 */
char *
database_getflatfilename(void)
{
	int			bufsize;
	char	   *pfnam;

	bufsize = strlen(DataDir) + strlen("/global/") +
		strlen(DATABASE_FLAT_FILE) + 1;
	pfnam = (char *) palloc(bufsize);
	snprintf(pfnam, bufsize, "%s/global/%s", DataDir, DATABASE_FLAT_FILE);

	return pfnam;
}

/*
 * group_getflatfilename --- get full pathname of group file
 *
 * Note that result string is palloc'd, and should be freed by the caller.
 */
char *
group_getflatfilename(void)
{
	int			bufsize;
	char	   *pfnam;

	bufsize = strlen(DataDir) + strlen("/global/") +
		strlen(GROUP_FLAT_FILE) + 1;
	pfnam = (char *) palloc(bufsize);
	snprintf(pfnam, bufsize, "%s/global/%s", DataDir, GROUP_FLAT_FILE);

	return pfnam;
}

/*
 * Get full pathname of password file.
 *
 * Note that result string is palloc'd, and should be freed by the caller.
 */
char *
user_getflatfilename(void)
{
	int			bufsize;
	char	   *pfnam;

	bufsize = strlen(DataDir) + strlen("/global/") +
		strlen(USER_FLAT_FILE) + 1;
	pfnam = (char *) palloc(bufsize);
	snprintf(pfnam, bufsize, "%s/global/%s", DataDir, USER_FLAT_FILE);

	return pfnam;
}


/*
 *	fputs_quote
 *
 *	Outputs string in quotes, with double-quotes duplicated.
 *	We could use quote_ident(), but that expects a TEXT argument.
 */
static void
fputs_quote(const char *str, FILE *fp)
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
 * name_okay
 *
 * We must disallow newlines in user and group names because
 * hba.c's parser won't handle fields split across lines, even if quoted.
 */
static bool
name_okay(const char *str)
{
	int			i;

	i = strcspn(str, "\r\n");
	return (str[i] == '\0');
}


/*
 * write_database_file: update the flat database file
 *
 * A side effect is to determine the oldest database's datfrozenxid
 * so we can set or update the XID wrap limit.
 */
static void
write_database_file(Relation drel)
{
	char	   *filename,
			   *tempname;
	int			bufsize;
	FILE	   *fp;
	mode_t		oumask;
	HeapScanDesc scan;
	HeapTuple	tuple;
	NameData	oldest_datname;
	TransactionId oldest_datfrozenxid = InvalidTransactionId;

	/*
	 * Create a temporary filename to be renamed later.  This prevents the
	 * backend from clobbering the flat file while the postmaster
	 * might be reading from it.
	 */
	filename = database_getflatfilename();
	bufsize = strlen(filename) + 12;
	tempname = (char *) palloc(bufsize);
	snprintf(tempname, bufsize, "%s.%d", filename, MyProcPid);

	oumask = umask((mode_t) 077);
	fp = AllocateFile(tempname, "w");
	umask(oumask);
	if (fp == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to temporary file \"%s\": %m",
						tempname)));

	/*
	 * Read pg_database and write the file.  Note we use SnapshotSelf to
	 * ensure we see all effects of current transaction.  (Perhaps could
	 * do a CommandCounterIncrement beforehand, instead?)
	 */
	scan = heap_beginscan(drel, SnapshotSelf, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_database dbform = (Form_pg_database) GETSTRUCT(tuple);
		char	   *datname;
		Oid			datoid;
		TransactionId datfrozenxid;

		datname = NameStr(dbform->datname);
		datoid = HeapTupleGetOid(tuple);
		datfrozenxid = dbform->datfrozenxid;

		/*
		 * Identify the oldest datfrozenxid, ignoring databases that are not
		 * connectable (we assume they are safely frozen).  This must match
		 * the logic in vac_truncate_clog() in vacuum.c.
		 */
		if (dbform->datallowconn &&
			TransactionIdIsNormal(datfrozenxid))
		{
			if (oldest_datfrozenxid == InvalidTransactionId ||
				TransactionIdPrecedes(datfrozenxid, oldest_datfrozenxid))
			{
				oldest_datfrozenxid = datfrozenxid;
				namestrcpy(&oldest_datname, datname);
			}
		}

		/*
		 * Check for illegal characters in the database name.
		 */
		if (!name_okay(datname))
		{
			ereport(LOG,
					(errmsg("invalid database name \"%s\"", datname)));
			continue;
		}

		/*
		 * The file format is: "dbname" oid frozenxid
		 *
		 * The xid is not needed for backend startup, but may be of use
		 * for forensic purposes.
		 */
		fputs_quote(datname, fp);
		fprintf(fp, " %u %u\n", datoid, datfrozenxid);
	}
	heap_endscan(scan);

	if (FreeFile(fp))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to temporary file \"%s\": %m",
						tempname)));

	/*
	 * Rename the temp file to its final name, deleting the old flat file.
	 * We expect that rename(2) is an atomic action.
	 */
	if (rename(tempname, filename))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						tempname, filename)));

	pfree(tempname);
	pfree(filename);

	/*
	 * Set the transaction ID wrap limit using the oldest datfrozenxid
	 */
	if (oldest_datfrozenxid != InvalidTransactionId)
		SetTransactionIdLimit(oldest_datfrozenxid, &oldest_datname);
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

	/*
	 * Create a temporary filename to be renamed later.  This prevents the
	 * backend from clobbering the flat file while the postmaster
	 * might be reading from it.
	 */
	filename = group_getflatfilename();
	bufsize = strlen(filename) + 12;
	tempname = (char *) palloc(bufsize);
	snprintf(tempname, bufsize, "%s.%d", filename, MyProcPid);

	oumask = umask((mode_t) 077);
	fp = AllocateFile(tempname, "w");
	umask(oumask);
	if (fp == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to temporary file \"%s\": %m",
						tempname)));

	/*
	 * Read pg_group and write the file.  Note we use SnapshotSelf to
	 * ensure we see all effects of current transaction.  (Perhaps could
	 * do a CommandCounterIncrement beforehand, instead?)
	 */
	scan = heap_beginscan(grel, SnapshotSelf, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_group grpform = (Form_pg_group) GETSTRUCT(tuple);
		HeapTupleHeader tup = tuple->t_data;
		char	   *tp;				/* ptr to tuple data */
		long		off;			/* offset in tuple data */
		bits8	   *bp = tup->t_bits;	/* ptr to null bitmask in tuple */
		Datum		datum;
		char	   *groname;
		IdList	   *grolist_p;
		AclId	   *aidp;
		int			i,
					num;

		groname = NameStr(grpform->groname);

		/*
		 * Check for illegal characters in the group name.
		 */
		if (!name_okay(groname))
		{
			ereport(LOG,
					(errmsg("invalid group name \"%s\"", groname)));
			continue;
		}

		/*
		 * We can't use heap_getattr() here because during startup we will
		 * not have any tupdesc for pg_group.  Fortunately it's not too
		 * hard to work around this.  grolist is the first possibly-null
		 * field so we can compute its offset directly.
		 */
		tp = (char *) tup + tup->t_hoff;
		off = offsetof(FormData_pg_group, grolist);

		if (HeapTupleHasNulls(tuple) &&
			att_isnull(Anum_pg_group_grolist - 1, bp))
		{
			/* grolist is null, so we can ignore this group */
			continue;
		}

		/* assume grolist is pass-by-ref */
		datum = PointerGetDatum(tp + off);

		/*
		 * We can't currently support out-of-line toasted group lists in
		 * startup mode (the tuptoaster won't work).  This sucks, but it
		 * should be something of a corner case.  Live with it until we
		 * can redesign pg_group.
		 *
		 * Detect startup mode by noting whether we got a tupdesc.
		 */
		if (VARATT_IS_EXTERNAL(DatumGetPointer(datum)) &&
			RelationGetDescr(grel) == NULL)
			continue;

		/* be sure the IdList is not toasted */
		grolist_p = DatumGetIdListP(datum);

		/*
		 * The file format is: "groupname"    usesysid1 usesysid2 ...
		 *
		 * We ignore groups that have no members.
		 */
		aidp = IDLIST_DAT(grolist_p);
		num = IDLIST_NUM(grolist_p);
		if (num > 0)
		{
			fputs_quote(groname, fp);
			fprintf(fp, "\t%u", aidp[0]);
			for (i = 1; i < num; ++i)
				fprintf(fp, " %u", aidp[i]);
			fputs("\n", fp);
		}

		/* if IdList was toasted, free detoasted copy */
		if ((Pointer) grolist_p != DatumGetPointer(datum))
			pfree(grolist_p);
	}
	heap_endscan(scan);

	if (FreeFile(fp))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to temporary file \"%s\": %m",
						tempname)));

	/*
	 * Rename the temp file to its final name, deleting the old flat file.
	 * We expect that rename(2) is an atomic action.
	 */
	if (rename(tempname, filename))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						tempname, filename)));

	pfree(tempname);
	pfree(filename);
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

	/*
	 * Create a temporary filename to be renamed later.  This prevents the
	 * backend from clobbering the flat file while the postmaster might
	 * be reading from it.
	 */
	filename = user_getflatfilename();
	bufsize = strlen(filename) + 12;
	tempname = (char *) palloc(bufsize);
	snprintf(tempname, bufsize, "%s.%d", filename, MyProcPid);

	oumask = umask((mode_t) 077);
	fp = AllocateFile(tempname, "w");
	umask(oumask);
	if (fp == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to temporary file \"%s\": %m",
						tempname)));

	/*
	 * Read pg_shadow and write the file.  Note we use SnapshotSelf to
	 * ensure we see all effects of current transaction.  (Perhaps could
	 * do a CommandCounterIncrement beforehand, instead?)
	 */
	scan = heap_beginscan(urel, SnapshotSelf, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_shadow pwform = (Form_pg_shadow) GETSTRUCT(tuple);
		HeapTupleHeader tup = tuple->t_data;
		char	   *tp;				/* ptr to tuple data */
		long		off;			/* offset in tuple data */
		bits8	   *bp = tup->t_bits;	/* ptr to null bitmask in tuple */
		Datum		datum;
		char	   *usename,
				   *passwd,
				   *valuntil;
		AclId		usesysid;

		usename = NameStr(pwform->usename);
		usesysid = pwform->usesysid;

		/*
		 * We can't use heap_getattr() here because during startup we will
		 * not have any tupdesc for pg_shadow.  Fortunately it's not too
		 * hard to work around this.  passwd is the first possibly-null
		 * field so we can compute its offset directly.
		 */
		tp = (char *) tup + tup->t_hoff;
		off = offsetof(FormData_pg_shadow, passwd);

		if (HeapTupleHasNulls(tuple) &&
			att_isnull(Anum_pg_shadow_passwd - 1, bp))
		{
			/* passwd is null, emit as an empty string */
			passwd = pstrdup("");
		}
		else
		{
			/* assume passwd is pass-by-ref */
			datum = PointerGetDatum(tp + off);

			/*
			 * The password probably shouldn't ever be out-of-line toasted;
			 * if it is, ignore it, since we can't handle that in startup mode.
			 */
			if (VARATT_IS_EXTERNAL(DatumGetPointer(datum)))
				passwd = pstrdup("");
			else
				passwd = DatumGetCString(DirectFunctionCall1(textout, datum));

			/* assume passwd has attlen -1 */
			off = att_addlength(off, -1, tp + off);
		}

		if (HeapTupleHasNulls(tuple) &&
			att_isnull(Anum_pg_shadow_valuntil - 1, bp))
		{
			/* valuntil is null, emit as an empty string */
			valuntil = pstrdup("");
		}
		else
		{
			/* assume valuntil has attalign 'i' */
			off = att_align(off, 'i');
			/* assume valuntil is pass-by-value, integer size */
			datum = Int32GetDatum(*((int32 *) (tp + off)));
			valuntil = DatumGetCString(DirectFunctionCall1(abstimeout, datum));
		}

		/*
		 * Check for illegal characters in the user name and password.
		 */
		if (!name_okay(usename))
		{
			ereport(LOG,
					(errmsg("invalid user name \"%s\"", usename)));
			continue;
		}
		if (!name_okay(passwd))
		{
			ereport(LOG,
					(errmsg("invalid user password \"%s\"", passwd)));
			continue;
		}

		/*
		 * The file format is: "usename" usesysid "passwd" "valuntil"
		 */
		fputs_quote(usename, fp);
		fprintf(fp, " %u ", usesysid);
		fputs_quote(passwd, fp);
		fputs(" ", fp);
		fputs_quote(valuntil, fp);
		fputs("\n", fp);

		pfree(passwd);
		pfree(valuntil);
	}
	heap_endscan(scan);

	if (FreeFile(fp))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to temporary file \"%s\": %m",
						tempname)));

	/*
	 * Rename the temp file to its final name, deleting the old flat file.
	 * We expect that rename(2) is an atomic action.
	 */
	if (rename(tempname, filename))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						tempname, filename)));

	pfree(tempname);
	pfree(filename);
}


/*
 * This routine is called once during database startup, after completing
 * WAL replay if needed.  Its purpose is to sync the flat files with the
 * current state of the database tables.  This is particularly important
 * during PITR operation, since the flat files will come from the
 * base backup which may be far out of sync with the current state.
 *
 * In theory we could skip rebuilding the flat files if no WAL replay
 * occurred, but it seems safest to just do it always.  We have to
 * scan pg_database to compute the XID wrap limit anyway.
 *
 * In a standalone backend we pass database_only = true to skip processing
 * the user and group files.  We won't need them, and building them could
 * fail if there's something corrupt in those catalogs.
 */
void
BuildFlatFiles(bool database_only)
{
	ResourceOwner owner;
	RelFileNode rnode;
	Relation	rel;

	/*
	 * We don't have any hope of running a real relcache, but we can use
	 * the same fake-relcache facility that WAL replay uses.
	 */
	XLogInitRelationCache();

	/* Need a resowner to keep the heapam and buffer code happy */
	owner = ResourceOwnerCreate(NULL, "BuildFlatFiles");
	CurrentResourceOwner = owner;

	/* hard-wired path to pg_database */
	rnode.spcNode = GLOBALTABLESPACE_OID;
	rnode.dbNode = 0;
	rnode.relNode = RelOid_pg_database;

	/* No locking is needed because no one else is alive yet */
	rel = XLogOpenRelation(true, 0, rnode);
	write_database_file(rel);

	if (!database_only)
	{
		/* hard-wired path to pg_group */
		rnode.spcNode = GLOBALTABLESPACE_OID;
		rnode.dbNode = 0;
		rnode.relNode = RelOid_pg_group;

		rel = XLogOpenRelation(true, 0, rnode);
		write_group_file(rel);

		/* hard-wired path to pg_shadow */
		rnode.spcNode = GLOBALTABLESPACE_OID;
		rnode.dbNode = 0;
		rnode.relNode = RelOid_pg_shadow;

		rel = XLogOpenRelation(true, 0, rnode);
		write_user_file(rel);
	}

	CurrentResourceOwner = NULL;
	ResourceOwnerDelete(owner);

	XLogCloseRelationCache();
}


/*
 * This routine is called during transaction commit or abort.
 *
 * On commit, if we've written any of the critical database tables during
 * the current transaction, update the flat files and signal the postmaster.
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
AtEOXact_UpdateFlatFiles(bool isCommit)
{
	Relation	drel = NULL;
	Relation	grel = NULL;
	Relation	urel = NULL;

	if (database_file_update_subid == InvalidSubTransactionId &&
		group_file_update_subid == InvalidSubTransactionId &&
		user_file_update_subid == InvalidSubTransactionId)
		return;					/* nothing to do */

	if (!isCommit)
	{
		database_file_update_subid = InvalidSubTransactionId;
		group_file_update_subid = InvalidSubTransactionId;
		user_file_update_subid = InvalidSubTransactionId;
		return;
	}

	/*
	 * We use ExclusiveLock to ensure that only one backend writes the
	 * flat file(s) at a time.	That's sufficient because it's okay to
	 * allow plain reads of the tables in parallel.  There is some chance
	 * of a deadlock here (if we were triggered by a user update of one
	 * of the tables, which likely won't have gotten a strong enough lock),
	 * so get the locks we need before writing anything.
	 */
	if (database_file_update_subid != InvalidSubTransactionId)
		drel = heap_openr(DatabaseRelationName, ExclusiveLock);
	if (group_file_update_subid != InvalidSubTransactionId)
		grel = heap_openr(GroupRelationName, ExclusiveLock);
	if (user_file_update_subid != InvalidSubTransactionId)
		urel = heap_openr(ShadowRelationName, ExclusiveLock);

	/* Okay to write the files */
	if (database_file_update_subid != InvalidSubTransactionId)
	{
		database_file_update_subid = InvalidSubTransactionId;
		write_database_file(drel);
		heap_close(drel, NoLock);
	}

	if (group_file_update_subid != InvalidSubTransactionId)
	{
		group_file_update_subid = InvalidSubTransactionId;
		write_group_file(grel);
		heap_close(grel, NoLock);
	}

	if (user_file_update_subid != InvalidSubTransactionId)
	{
		user_file_update_subid = InvalidSubTransactionId;
		write_user_file(urel);
		heap_close(urel, NoLock);
	}

	/*
	 * Signal the postmaster to reload its caches.
	 */
	SendPostmasterSignal(PMSIGNAL_PASSWORD_CHANGE);
}

/*
 * AtEOSubXact_UpdateFlatFiles
 *
 * Called at subtransaction end, this routine resets or updates the
 * need-to-update-files flags.
 */
void
AtEOSubXact_UpdateFlatFiles(bool isCommit,
							SubTransactionId mySubid,
							SubTransactionId parentSubid)
{
	if (isCommit)
	{
		if (database_file_update_subid == mySubid)
			database_file_update_subid = parentSubid;

		if (group_file_update_subid == mySubid)
			group_file_update_subid = parentSubid;

		if (user_file_update_subid == mySubid)
			user_file_update_subid = parentSubid;
	}
	else
	{
		if (database_file_update_subid == mySubid)
			database_file_update_subid = InvalidSubTransactionId;

		if (group_file_update_subid == mySubid)
			group_file_update_subid = InvalidSubTransactionId;

		if (user_file_update_subid == mySubid)
			user_file_update_subid = InvalidSubTransactionId;
	}
}


/*
 * This trigger is fired whenever someone modifies pg_database, pg_shadow
 * or pg_group via general-purpose INSERT/UPDATE/DELETE commands.
 *
 * It is sufficient for this to be a STATEMENT trigger since we don't
 * care which individual rows changed.  It doesn't much matter whether
 * it's a BEFORE or AFTER trigger.
 */
Datum
flatfile_update_trigger(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;

	if (!CALLED_AS_TRIGGER(fcinfo))
		elog(ERROR,
			 "flatfile_update_trigger was not called by trigger manager");

	if (RelationGetNamespace(trigdata->tg_relation) != PG_CATALOG_NAMESPACE)
		elog(ERROR, "flatfile_update_trigger was called for wrong table");

	switch (RelationGetRelid(trigdata->tg_relation))
	{
		case RelOid_pg_database:
			database_file_update_needed();
			break;
		case RelOid_pg_group:
			group_file_update_needed();
			break;
		case RelOid_pg_shadow:
			user_file_update_needed();
			break;
		default:
			elog(ERROR, "flatfile_update_trigger was called for wrong table");
			break;
	}

	return PointerGetDatum(NULL);
}


/*
 * Old version of trigger --- remove after we can force an initdb
 */
extern Datum update_pg_pwd_and_pg_group(PG_FUNCTION_ARGS);

Datum
update_pg_pwd_and_pg_group(PG_FUNCTION_ARGS)
{
	return flatfile_update_trigger(fcinfo);
}
