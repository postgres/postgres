/*-------------------------------------------------------------------------
 *
 * database.c
 *	  miscellaneous initialization support stuff
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/misc/Attic/database.c,v 1.33 1999/12/16 22:19:55 wieck Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <unistd.h>
#include <fcntl.h>

#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_database.h"
#include "miscadmin.h"
#include "utils/syscache.h"

#ifdef NOT_USED
/* GetDatabaseInfo()
 * Pull database information from pg_database.
 */
int
GetDatabaseInfo(char *name, int4 *owner, char *path)
{
	Oid			dbowner,
				dbid;
	char		dbpath[MAXPGPATH];
	text	   *dbtext;

	Relation	dbrel;
	HeapTuple	dbtup;
	HeapTuple	tup;
	HeapScanDesc scan;
	ScanKeyData scanKey;

	dbrel = heap_openr(DatabaseRelationName, AccessShareLock);

	ScanKeyEntryInitialize(&scanKey, 0, Anum_pg_database_datname,
						   F_NAMEEQ, NameGetDatum(name));

	scan = heap_beginscan(dbrel, 0, SnapshotNow, 1, &scanKey);
	if (!HeapScanIsValid(scan))
		elog(ERROR, "GetDatabaseInfo: cannot begin scan of %s", DatabaseRelationName);

	/*
	 * Since we're going to close the relation, copy the tuple.
	 */
	tup = heap_getnext(scan, 0);

	if (HeapTupleIsValid(tup))
		dbtup = heap_copytuple(tup);
	else
		dbtup = tup;

	heap_endscan(scan);

	if (!HeapTupleIsValid(dbtup))
	{
		elog(NOTICE, "GetDatabaseInfo: %s entry not found %s",
			 DatabaseRelationName, name);
		heap_close(dbrel, AccessShareLock);
		return TRUE;
	}

	dbowner = (Oid) heap_getattr(dbtup,
								 Anum_pg_database_datdba,
								 RelationGetDescr(dbrel),
								 (char *) NULL);
	dbid = dbtup->t_oid;

	dbtext = (text *) heap_getattr(dbtup,
								   Anum_pg_database_datpath,
								   RelationGetDescr(dbrel),
								   (char *) NULL);

	memcpy(dbpath, VARDATA(dbtext), (VARSIZE(dbtext) - VARHDRSZ));
	*(dbpath + (VARSIZE(dbtext) - VARHDRSZ)) = '\0';

	heap_close(dbrel, AccessShareLock);

	owner = palloc(sizeof(Oid));
	*owner = dbowner;
	path = pstrdup(dbpath);		/* doesn't do the right thing! */

	return FALSE;
}	/* GetDatabaseInfo() */

#endif

char *
ExpandDatabasePath(char *dbpath)
{
	char		buf[MAXPGPATH];
	char	   *cp;
	char	   *envvar;
	int			len;

	if (strlen(dbpath) >= MAXPGPATH)
		return NULL;			/* ain't gonna fit nohow */

	/* leading path delimiter? then already absolute path */
	if (*dbpath == SEP_CHAR)
	{
#ifdef ALLOW_ABSOLUTE_DBPATHS
		cp = strrchr(dbpath, SEP_CHAR);
		len = cp - dbpath;
		strncpy(buf, dbpath, len);
		snprintf(&buf[len], MAXPGPATH-len, "%cbase%c%s",
				 SEP_CHAR, SEP_CHAR, (cp + 1));
#else
		return NULL;
#endif
	}
	/* path delimiter somewhere? then has leading environment variable */
	else if ((cp = strchr(dbpath, SEP_CHAR)) != NULL)
	{
		len = cp - dbpath;
		strncpy(buf, dbpath, len);
		buf[len] = '\0';
		envvar = getenv(buf);

		/*
		 * problem getting environment variable? let calling routine
		 * handle it
		 */
		if (envvar == NULL)
			return envvar;

		snprintf(buf, sizeof(buf), "%s%cbase%c%s",
				 envvar, SEP_CHAR, SEP_CHAR, (cp + 1));
	}
	else
	{
		/* no path delimiter? then add the default path prefix */
		snprintf(buf, sizeof(buf), "%s%cbase%c%s",
				 DataDir, SEP_CHAR, SEP_CHAR, dbpath);
	}

	return pstrdup(buf);
}	/* ExpandDatabasePath() */


/* --------------------------------
 *	GetRawDatabaseInfo() -- Find the OID and path of the database.
 *
 *		The database's oid forms half of the unique key for the system
 *		caches and lock tables.  We therefore want it initialized before
 *		we open any relations, since opening relations puts things in the
 *		cache.	To get around this problem, this code opens and scans the
 *		pg_database relation by hand.
 *
 *		This code knows way more than it should about the layout of
 *		tuples on disk, but there seems to be no help for that.
 *		We're pulling ourselves up by the bootstraps here...
 * --------------------------------
 */
void
GetRawDatabaseInfo(char *name, Oid *db_id, char *path)
{
	int			dbfd;
	int			fileflags;
	int			nbytes;
	int			max,
				i;
	HeapTupleData tup;
	Page		pg;
	PageHeader	ph;
	char	   *dbfname;
	Form_pg_database tup_db;

	dbfname = (char *) palloc(strlen(DataDir) + strlen("pg_database") + 2);
	sprintf(dbfname, "%s%cpg_database", DataDir, SEP_CHAR);
	fileflags = O_RDONLY;

#ifndef __CYGWIN32__
	if ((dbfd = open(dbfname, O_RDONLY, 0)) < 0)
#else
	if ((dbfd = open(dbfname, O_RDONLY | O_BINARY, 0)) < 0)
#endif
		elog(FATAL, "Cannot open %s", dbfname);

	pfree(dbfname);

	/* ----------------
	 *	read and examine every page in pg_database
	 *
	 *	Raw I/O! Read those tuples the hard way! Yow!
	 *
	 *	Why don't we use the access methods or move this code
	 *	someplace else?  This is really pg_database schema dependent
	 *	code.  Perhaps it should go in lib/catalog/pg_database?
	 *	-cim 10/3/90
	 *
	 *	mao replies 4 apr 91:  yeah, maybe this should be moved to
	 *	lib/catalog.  however, we CANNOT use the access methods since
	 *	those use the buffer cache, which uses the relation cache, which
	 *	requires that the dbid be set, which is what we're trying to do
	 *	here.
	 * ----------------
	 */
	pg = (Page) palloc(BLCKSZ);
	ph = (PageHeader) pg;

	while ((nbytes = read(dbfd, pg, BLCKSZ)) == BLCKSZ)
	{
		max = PageGetMaxOffsetNumber(pg);

		/* look at each tuple on the page */
		for (i = 0; i <= max; i++)
		{
			int			offset;

			/* if it's a freed tuple, ignore it */
			if (!(ph->pd_linp[i].lp_flags & LP_USED))
				continue;

			/* get a pointer to the tuple itself */
			offset = (int) ph->pd_linp[i].lp_off;
			tup.t_datamcxt = NULL;
			tup.t_data = (HeapTupleHeader) (((char *) pg) + offset);

			/*
			 * if the tuple has been deleted (the database was destroyed),
			 * skip this tuple.  XXX warning, will robinson:  violation of
			 * transaction semantics happens right here.  we should check
			 * to be sure that the xact that deleted this tuple actually
			 * committed.  Only way to do that at init time is to paw over
			 * the log relation by hand, too.  Instead we take the
			 * conservative assumption that if someone tried to delete it,
			 * it's gone.  The other side of the coin is that we might
			 * accept a tuple that was stored and never committed.  All in
			 * all, this code is pretty shaky.  We will cross-check our
			 * result in ReverifyMyDatabase() in postinit.c.
			 *
			 * NOTE: if a bogus tuple in pg_database prevents connection
			 * to a valid database, a fix is to connect to another database
			 * and do "select * from pg_database".  That should cause
			 * committed and dead tuples to be marked with correct states.
			 *
			 * XXX wouldn't it be better to let new backends read the
			 * database OID from a flat file, handled the same way
			 * we handle the password relation?
			 */
			if (TransactionIdIsValid((TransactionId) tup.t_data->t_xmax))
				continue;

			/*
			 * Okay, see if this is the one we want.
			 */
			tup_db = (Form_pg_database) GETSTRUCT(&tup);

			if (strcmp(name, NameStr(tup_db->datname)) == 0)
			{
				/* Found it; extract the OID and the database path. */
				*db_id = tup.t_data->t_oid;
				strncpy(path, VARDATA(&(tup_db->datpath)),
						(VARSIZE(&(tup_db->datpath)) - VARHDRSZ));
				*(path + VARSIZE(&(tup_db->datpath)) - VARHDRSZ) = '\0';
				goto done;
			}
		}
	}

done:
	close(dbfd);
	pfree(pg);
}	/* GetRawDatabaseInfo() */
