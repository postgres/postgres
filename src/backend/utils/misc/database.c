/*-------------------------------------------------------------------------
 *
 * database.c
 *	  miscellaneous initialization support stuff
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/misc/database.c,v 1.63 2004/12/31 22:02:45 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>

#include "access/xact.h"
#include "catalog/catname.h"
#include "catalog/catalog.h"
#include "catalog/pg_database.h"
#include "catalog/pg_tablespace.h"
#include "miscadmin.h"
#include "utils/syscache.h"


static bool PhonyHeapTupleSatisfiesNow(HeapTupleHeader tuple);


/* --------------------------------
 *	GetRawDatabaseInfo() -- Find the OID and tablespace of the database.
 *
 *		We need both the OID and the default tablespace in order to find
 *		the database's system catalogs.  Moreover the database's OID forms
 *		half of the unique key for the system caches and lock tables, so
 *		we must have it before we can use any of the cache mechanisms.
 *		To get around these problems, this code opens and scans the
 *		pg_database relation by hand.
 *
 *		This code knows way more than it should about the layout of
 *		tuples on disk, but there seems to be no help for that.
 *		We're pulling ourselves up by the bootstraps here...
 * --------------------------------
 */
void
GetRawDatabaseInfo(const char *name, Oid *db_id, Oid *db_tablespace)
{
	int			dbfd;
	int			nbytes;
	HeapTupleData tup;
	Form_pg_database tup_db;
	Page		pg;
	char	   *dbfname;
	RelFileNode rnode;

	/* hard-wired path to pg_database */
	rnode.spcNode = GLOBALTABLESPACE_OID;
	rnode.dbNode = 0;
	rnode.relNode = RelOid_pg_database;

	dbfname = relpath(rnode);

	if ((dbfd = open(dbfname, O_RDONLY | PG_BINARY, 0)) < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", dbfname)));

	pfree(dbfname);

	/*
	 * read and examine every page in pg_database
	 *
	 * Raw I/O! Read those tuples the hard way! Yow!
	 *
	 * Why don't we use the access methods or move this code someplace else?
	 * This is really pg_database schema dependent code.  Perhaps it
	 * should go in lib/catalog/pg_database? -cim 10/3/90
	 *
	 * mao replies 4 apr 91:  yeah, maybe this should be moved to
	 * lib/catalog.  however, we CANNOT use the access methods since those
	 * use the buffer cache, which uses the relation cache, which requires
	 * that the dbid be set, which is what we're trying to do here.
	 *
	 */
	pg = (Page) palloc(BLCKSZ);

	while ((nbytes = read(dbfd, pg, BLCKSZ)) == BLCKSZ)
	{
		OffsetNumber max = PageGetMaxOffsetNumber(pg);
		OffsetNumber lineoff;

		/* look at each tuple on the page */
		for (lineoff = FirstOffsetNumber; lineoff <= max; lineoff++)
		{
			ItemId		lpp = PageGetItemId(pg, lineoff);

			/* if it's a freed tuple, ignore it */
			if (!ItemIdIsUsed(lpp))
				continue;

			/* get a pointer to the tuple itself */
			tup.t_datamcxt = NULL;
			tup.t_data = (HeapTupleHeader) PageGetItem(pg, lpp);

			/*
			 * Check to see if tuple is valid (committed).
			 *
			 * XXX warning, will robinson: violation of transaction semantics
			 * happens right here.	We cannot really determine if the
			 * tuple is valid without checking transaction commit status,
			 * and the only way to do that at init time is to paw over
			 * pg_clog by hand, too.  Instead of checking, we assume that
			 * the inserting transaction committed, and that any deleting
			 * transaction did also, unless shown otherwise by on-row
			 * commit status bits.
			 *
			 * All in all, this code is pretty shaky.  We will cross-check
			 * our result in ReverifyMyDatabase() in postinit.c.
			 *
			 * NOTE: if a bogus tuple in pg_database prevents connection to a
			 * valid database, a fix is to connect to another database and
			 * do "select * from pg_database".	That should cause
			 * committed and dead tuples to be marked with correct states.
			 *
			 * XXX wouldn't it be better to let new backends read the
			 * database info from a flat file, handled the same way we
			 * handle the password relation?
			 */
			if (!PhonyHeapTupleSatisfiesNow(tup.t_data))
				continue;

			/*
			 * Okay, see if this is the one we want.
			 */
			tup_db = (Form_pg_database) GETSTRUCT(&tup);

			if (strcmp(name, NameStr(tup_db->datname)) == 0)
			{
				/* Found it; extract the db's OID and tablespace. */
				*db_id = HeapTupleGetOid(&tup);
				*db_tablespace = tup_db->dattablespace;
				goto done;
			}
		}
	}

	/* failed to find it... */
	*db_id = InvalidOid;
	*db_tablespace = InvalidOid;

done:
	close(dbfd);
	pfree(pg);
}

/*
 * PhonyHeapTupleSatisfiesNow --- cut-down tuple time qual test
 *
 * This is a simplified version of HeapTupleSatisfiesNow() that does not
 * depend on having transaction commit info available.	Any transaction
 * that touched the tuple is assumed committed unless later marked invalid.
 * (While we could think about more complex rules, this seems appropriate
 * for examining pg_database, since both CREATE DATABASE and DROP DATABASE
 * are non-roll-back-able.)
 */
static bool
PhonyHeapTupleSatisfiesNow(HeapTupleHeader tuple)
{
	if (!(tuple->t_infomask & HEAP_XMIN_COMMITTED))
	{
		if (tuple->t_infomask & HEAP_XMIN_INVALID)
			return false;

		if (tuple->t_infomask & HEAP_MOVED_OFF)
			return false;
		/* else assume committed */
	}

	if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid or aborted */
		return true;

	/* assume xmax transaction committed */
	if (tuple->t_infomask & HEAP_MARKED_FOR_UPDATE)
		return true;

	return false;
}
