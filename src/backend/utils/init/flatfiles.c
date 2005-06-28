/*-------------------------------------------------------------------------
 *
 * flatfiles.c
 *	  Routines for maintaining "flat file" images of the shared catalogs.
 *
 * We use flat files so that the postmaster and not-yet-fully-started
 * backends can look at the contents of pg_database, pg_authid, and 
 * pg_auth_members for authentication purposes.  This module is 
 * responsible for keeping the flat-file images as nearly in sync with 
 * database reality as possible.
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
 * $PostgreSQL: pgsql/src/backend/utils/init/flatfiles.c,v 1.9 2005/06/28 05:09:02 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/twophase_rmgr.h"
#include "catalog/indexing.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_tablespace.h"
#include "commands/trigger.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/pmsignal.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/flatfiles.h"
#include "utils/fmgroids.h"
#include "utils/resowner.h"
#include "utils/syscache.h"


/* Actual names of the flat files (within $PGDATA/global/) */
#define DATABASE_FLAT_FILE	"pg_database"
#define AUTH_FLAT_FILE		"pg_auth"

/* Info bits in a flatfiles 2PC record */
#define FF_BIT_DATABASE	1
#define FF_BIT_AUTH		2


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
static SubTransactionId auth_file_update_subid = InvalidSubTransactionId;


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
 * Mark flat auth file as needing an update (because pg_auth changed)
 */
void
auth_file_update_needed(void)
{
	if (auth_file_update_subid == InvalidSubTransactionId)
		auth_file_update_subid = GetCurrentSubTransactionId();
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
 * Get full pathname of auth file.
 *
 * Note that result string is palloc'd, and should be freed by the caller.
 */
char *
auth_getflatfilename(void)
{
	int			bufsize;
	char	   *pfnam;

	bufsize = strlen(DataDir) + strlen("/global/") +
		strlen(AUTH_FLAT_FILE) + 1;
	pfnam = (char *) palloc(bufsize);
	snprintf(pfnam, bufsize, "%s/global/%s", DataDir, AUTH_FLAT_FILE);

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
 * We must disallow newlines in role names because
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
	 * Read pg_database and write the file.
	 */
	scan = heap_beginscan(drel, SnapshotNow, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_database dbform = (Form_pg_database) GETSTRUCT(tuple);
		char	   *datname;
		Oid			datoid;
		Oid			dattablespace;
		TransactionId datfrozenxid;

		datname = NameStr(dbform->datname);
		datoid = HeapTupleGetOid(tuple);
		dattablespace = dbform->dattablespace;
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
		 * The file format is: "dbname" oid tablespace frozenxid
		 *
		 * The xid is not needed for backend startup, but may be of use
		 * for forensic purposes.
		 */
		fputs_quote(datname, fp);
		fprintf(fp, " %u %u %u\n", datoid, dattablespace, datfrozenxid);
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
 * Support for write_auth_file
 */

typedef struct {
	Oid			roleid;
	char*		rolname;
	char*		rolpassword;
	char*		rolvaliduntil;
	List*		roles_names;
} auth_entry;

typedef struct {
	Oid			roleid;
	Oid			memberid;
} authmem_entry;

static int
oid_compar(const void *a, const void *b)
{
	const auth_entry *a_auth = (const auth_entry*) a;
	const auth_entry *b_auth = (const auth_entry*) b;

	if (a_auth->roleid < b_auth->roleid) return -1;
	if (a_auth->roleid > b_auth->roleid) return 1;
	return 0;
}

static int
name_compar(const void *a, const void *b)
{
	const auth_entry *a_auth = (const auth_entry*) a;
	const auth_entry *b_auth = (const auth_entry*) b;

	return strcmp(a_auth->rolname,b_auth->rolname);
}

static int
mem_compar(const void *a, const void *b)
{
	const authmem_entry *a_auth = (const authmem_entry*) a;
	const authmem_entry *b_auth = (const authmem_entry*) b;

	if (a_auth->memberid < b_auth->memberid) return -1;
	if (a_auth->memberid > b_auth->memberid) return 1;
	return 0;
}

/*
 * write_auth_file: update the flat auth file
 */
static void
write_auth_file(Relation rel_auth, Relation rel_authmem, bool startup)
{
	char	   *filename,
			   *tempname;
	int			bufsize;
	BlockNumber	totalblocks;
	FILE	   *fp;
	mode_t		oumask;
	HeapScanDesc scan;
	HeapTuple	tuple;
	int			curr_role = 0;
	int			total_roles = 0;
	int			curr_mem = 0;
	int			total_mem = 0;
	int			est_rows;
	auth_entry  *auth_info;
	authmem_entry  *authmem_info = NULL;

	/*
	 * Create a temporary filename to be renamed later.  This prevents the
	 * backend from clobbering the pg_auth file while the postmaster might
	 * be reading from it.
	 */
	filename = auth_getflatfilename();
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
	 * Read pg_authid and fill temporary data structures.
	 */
	totalblocks = RelationGetNumberOfBlocks(rel_auth);
	totalblocks = totalblocks ? totalblocks : 1;
	est_rows = totalblocks * (BLCKSZ / (sizeof(HeapTupleHeaderData)+sizeof(FormData_pg_authid)));
	auth_info = (auth_entry*) palloc(est_rows*sizeof(auth_entry));

	scan = heap_beginscan(rel_auth, SnapshotNow, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_authid pwform = (Form_pg_authid) GETSTRUCT(tuple);
		HeapTupleHeader tup = tuple->t_data;
		char	   *tp;				/* ptr to tuple data */
		long		off;			/* offset in tuple data */
		bits8	   *bp = tup->t_bits;	/* ptr to null bitmask in tuple */
		Datum		datum;

		auth_info[curr_role].roleid = HeapTupleGetOid(tuple);
		auth_info[curr_role].rolname = pstrdup(NameStr(pwform->rolname));
		auth_info[curr_role].roles_names = NIL;

		/*
		 * We can't use heap_getattr() here because during startup we will
		 * not have any tupdesc for pg_authid.  Fortunately it's not too
		 * hard to work around this.  rolpassword is the first possibly-null
		 * field so we can compute its offset directly.
		 */
		tp = (char *) tup + tup->t_hoff;
		off = offsetof(FormData_pg_authid, rolpassword);

		if (HeapTupleHasNulls(tuple) &&
			att_isnull(Anum_pg_authid_rolpassword - 1, bp))
		{
			/* passwd is null, emit as an empty string */
			auth_info[curr_role].rolpassword = pstrdup("");
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
				auth_info[curr_role].rolpassword = pstrdup("");
			else
				auth_info[curr_role].rolpassword = DatumGetCString(DirectFunctionCall1(textout, datum));

			/* assume passwd has attlen -1 */
			off = att_addlength(off, -1, tp + off);
		}

		if (HeapTupleHasNulls(tuple) &&
			att_isnull(Anum_pg_authid_rolvaliduntil - 1, bp))
		{
			/* rolvaliduntil is null, emit as an empty string */
			auth_info[curr_role].rolvaliduntil = pstrdup("");
		}
		else
		{
			/*
			 * rolvaliduntil is timestamptz, which we assume is double
			 * alignment and pass-by-reference.
			 */
			off = att_align(off, 'd');
			datum = PointerGetDatum(tp + off);
			auth_info[curr_role].rolvaliduntil = DatumGetCString(DirectFunctionCall1(timestamptz_out, datum));
		}

		/*
		 * Check for illegal characters in the user name and password.
		 */
		if (!name_okay(auth_info[curr_role].rolname))
		{
			ereport(LOG,
					(errmsg("invalid role name \"%s\"",
							auth_info[curr_role].rolname)));
			pfree(auth_info[curr_role].rolname);
			pfree(auth_info[curr_role].rolpassword);
			pfree(auth_info[curr_role].rolvaliduntil);
			continue;
		}
		if (!name_okay(auth_info[curr_role].rolpassword))
		{
			ereport(LOG,
					(errmsg("invalid role password \"%s\"",
							auth_info[curr_role].rolpassword)));
			pfree(auth_info[curr_role].rolname);
			pfree(auth_info[curr_role].rolpassword);
			pfree(auth_info[curr_role].rolvaliduntil);
			continue;
		}

		curr_role++;
		total_roles++;
	}
	heap_endscan(scan);

	Assert(total_roles <= est_rows);

	qsort(auth_info, total_roles, sizeof(auth_entry), oid_compar);

	/*
	 * Read pg_auth_members into temporary data structure, too
	 */
	totalblocks = RelationGetNumberOfBlocks(rel_authmem);
	totalblocks = totalblocks ? totalblocks : 1;
	est_rows = totalblocks * (BLCKSZ / (sizeof(HeapTupleHeaderData)+sizeof(FormData_pg_auth_members)));
	authmem_info = (authmem_entry*) palloc(est_rows*sizeof(authmem_entry));

	scan = heap_beginscan(rel_authmem, SnapshotNow, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_auth_members memform = (Form_pg_auth_members) GETSTRUCT(tuple);

		authmem_info[curr_mem].roleid = memform->roleid;
		authmem_info[curr_mem].memberid = memform->member;
		curr_mem++;
		total_mem++;
	}
	heap_endscan(scan);

	Assert(total_mem <= est_rows);

	qsort(authmem_info, total_mem, sizeof(authmem_entry), mem_compar);

	for (curr_role = 0; curr_role < total_roles; curr_role++)
	{
		int		first_found, last_found, curr_mem;
		List	*roles_list_hunt = NIL;
		List	*roles_list = NIL;
		ListCell *mem = NULL;
		auth_entry *found_role = NULL, key_auth;
		authmem_entry key;
		authmem_entry *found_mem = NULL;

		roles_list_hunt = lappend_oid(roles_list_hunt,
									  auth_info[curr_role].roleid);

		while (roles_list_hunt)
		{
			key.memberid = linitial_oid(roles_list_hunt);
			roles_list_hunt = list_delete_first(roles_list_hunt);
			if (total_mem)
				found_mem = bsearch(&key, authmem_info, total_mem,
									sizeof(authmem_entry), mem_compar);
			if (found_mem)
			{
				/*
				 * bsearch found a match for us; but if there were multiple
				 * matches it could have found any one of them.
				 */
				first_found = last_found = (found_mem - authmem_info);
				while (first_found > 0 &&
					   mem_compar(&key, &authmem_info[first_found - 1]) == 0)
					first_found--;
				while (last_found + 1 < total_mem &&
					   mem_compar(&key, &authmem_info[last_found + 1]) == 0)
					last_found++;

				for (curr_mem = first_found; curr_mem <= last_found; curr_mem++)
				{
					Oid	otherrole = authmem_info[curr_mem].roleid;

					if (!list_member_oid(roles_list, otherrole))
					{
						roles_list = lappend_oid(roles_list,
												 otherrole);
						roles_list_hunt = lappend_oid(roles_list_hunt,
													  otherrole);
					}
				}
			}
		}

		foreach(mem, roles_list)
		{
			key_auth.roleid = lfirst_oid(mem);
			found_role = bsearch(&key_auth, auth_info, total_roles, sizeof(auth_entry), oid_compar);
			auth_info[curr_role].roles_names = lappend(auth_info[curr_role].roles_names,found_role->rolname);
		}
	}

	qsort(auth_info, total_roles, sizeof(auth_entry), name_compar);

	for (curr_role = 0; curr_role < total_roles; curr_role++)
	{
		ListCell *mem = NULL;

		/*----------
		 * The file format is:
		 *	"rolename" "password" "validuntil" "member" "member" ...
		 * where lines are expected to be in order by rolename
		 *----------
		 */
		fputs_quote(auth_info[curr_role].rolname, fp);
		fputs(" ", fp);
		fputs_quote(auth_info[curr_role].rolpassword, fp);
		fputs(" ", fp);
		fputs_quote(auth_info[curr_role].rolvaliduntil, fp);

		foreach(mem, auth_info[curr_role].roles_names)
		{
			fputs(" ", fp);
			fputs_quote(lfirst(mem), fp);
		}

		fputs("\n", fp);

		pfree(auth_info[curr_role].rolname);
		pfree(auth_info[curr_role].rolpassword);
		pfree(auth_info[curr_role].rolvaliduntil);
	}

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

	pfree(auth_info);
	pfree(authmem_info);
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
	Relation	rel, rel_auth, rel_authmem;

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
	rnode.relNode = DatabaseRelationId;

	/* No locking is needed because no one else is alive yet */
	rel = XLogOpenRelation(rnode);
	write_database_file(rel);

	if (!database_only)
	{
		/* hard-wired path to pg_auth */
		rnode.spcNode = GLOBALTABLESPACE_OID;
		rnode.dbNode = 0;
		rnode.relNode = AuthIdRelationId;
		rel_auth = XLogOpenRelation(rnode);

		rnode.spcNode = GLOBALTABLESPACE_OID;
		rnode.dbNode = 0;
		rnode.relNode = AuthMemRelationId;
		rel_authmem = XLogOpenRelation(rnode);
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
	Relation	arel = NULL;
	Relation	mrel = NULL;

	if (database_file_update_subid == InvalidSubTransactionId &&
		auth_file_update_subid == InvalidSubTransactionId)
		return;					/* nothing to do */

	if (!isCommit)
	{
		database_file_update_subid = InvalidSubTransactionId;
		auth_file_update_subid = InvalidSubTransactionId;
		return;
	}

	/*
	 * Advance command counter to be certain we see all effects of the
	 * current transaction.
	 */
	CommandCounterIncrement();

	/*
	 * We use ExclusiveLock to ensure that only one backend writes the
	 * flat file(s) at a time.	That's sufficient because it's okay to
	 * allow plain reads of the tables in parallel.  There is some chance
	 * of a deadlock here (if we were triggered by a user update of one
	 * of the tables, which likely won't have gotten a strong enough lock),
	 * so get the locks we need before writing anything.
	 */
	if (database_file_update_subid != InvalidSubTransactionId)
		drel = heap_open(DatabaseRelationId, ExclusiveLock);
	if (auth_file_update_subid != InvalidSubTransactionId) {
		arel = heap_open(AuthIdRelationId, ExclusiveLock);
		mrel = heap_open(AuthMemRelationId, ExclusiveLock);
	}

	/* Okay to write the files */
	if (database_file_update_subid != InvalidSubTransactionId)
	{
		database_file_update_subid = InvalidSubTransactionId;
		write_database_file(drel);
		heap_close(drel, NoLock);
	}

	if (auth_file_update_subid != InvalidSubTransactionId)
	{
		auth_file_update_subid = InvalidSubTransactionId;
		write_auth_file(arel, mrel, false);
		heap_close(arel, NoLock);
		heap_close(mrel, NoLock);
	}

	/*
	 * Signal the postmaster to reload its caches.
	 */
	SendPostmasterSignal(PMSIGNAL_PASSWORD_CHANGE);
}


/*
 * This routine is called during transaction prepare.
 *
 * Record which files need to be refreshed if this transaction later
 * commits.
 *
 * Note: it's OK to clear the flags immediately, since if the PREPARE fails
 * further on, we'd only reset the flags anyway. So there's no need for a
 * separate PostPrepare call.
 */
void
AtPrepare_UpdateFlatFiles(void)
{
	uint16		info = 0;

	if (database_file_update_subid != InvalidSubTransactionId)
	{
		database_file_update_subid = InvalidSubTransactionId;
		info |= FF_BIT_DATABASE;
	}
	if (auth_file_update_subid != InvalidSubTransactionId)
	{
		auth_file_update_subid = InvalidSubTransactionId;
		info |= FF_BIT_AUTH;
	}
	if (info != 0)
		RegisterTwoPhaseRecord(TWOPHASE_RM_FLATFILES_ID, info,
							   NULL, 0);
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

		if (auth_file_update_subid == mySubid)
			auth_file_update_subid = parentSubid;
	}
	else
	{
		if (database_file_update_subid == mySubid)
			database_file_update_subid = InvalidSubTransactionId;

		if (auth_file_update_subid == mySubid)
			auth_file_update_subid = InvalidSubTransactionId;
	}
}


/*
 * This trigger is fired whenever someone modifies pg_database, pg_authid
 * or pg_auth_members via general-purpose INSERT/UPDATE/DELETE commands.
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
		case DatabaseRelationId:
			database_file_update_needed();
			break;
		case AuthIdRelationId:
			auth_file_update_needed();
			break;
		case AuthMemRelationId:
			auth_file_update_needed();
			break;
		default:
			elog(ERROR, "flatfile_update_trigger was called for wrong table");
			break;
	}

	return PointerGetDatum(NULL);
}


/*
 * 2PC processing routine for COMMIT PREPARED case.
 *
 * (We don't have to do anything for ROLLBACK PREPARED.)
 */
void
flatfile_twophase_postcommit(TransactionId xid, uint16 info,
							 void *recdata, uint32 len)
{
	/*
	 * Set flags to do the needed file updates at the end of my own
	 * current transaction.  (XXX this has some issues if my own
	 * transaction later rolls back, or if there is any significant
	 * delay before I commit.  OK for now because we disallow
	 * COMMIT PREPARED inside a transaction block.)
	 */
	if (info & FF_BIT_DATABASE)
		database_file_update_needed();
	if (info & FF_BIT_AUTH)
		auth_file_update_needed();
}
