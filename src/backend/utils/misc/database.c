/*-------------------------------------------------------------------------
 *
 * database.c--
 *	  miscellanious initialization support stuff
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/misc/Attic/database.c,v 1.12 1998/07/24 03:31:59 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include "postgres.h"

#include "access/heapam.h"
#include "access/xact.h"
#include "catalog/catname.h"
#ifdef MB
#include "catalog/pg_database_mb.h"
#include "mb/pg_wchar.h"
#else
#include "catalog/pg_database.h"
#endif
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/builtins.h"
#include "utils/syscache.h"


/* GetDatabaseInfo()
 * Pull database information from pg_database.
 */
int
GetDatabaseInfo(char *name, Oid *owner, char *path)
{
	Oid			dbowner,
				dbid;
	char		dbpath[MAXPGPATH + 1];
	text	   *dbtext;

	Relation	dbrel;
	HeapTuple	dbtup;
	HeapTuple	tup;
	Buffer		buf;
	HeapScanDesc scan;
	ScanKeyData scanKey;

	dbrel = heap_openr(DatabaseRelationName);
	if (!RelationIsValid(dbrel))
		elog(FATAL, "GetDatabaseInfo: cannot open relation \"%-.*s\"",
			 DatabaseRelationName);

	ScanKeyEntryInitialize(&scanKey, 0, Anum_pg_database_datname,
						   F_NAMEEQ, NameGetDatum(name));

	scan = heap_beginscan(dbrel, 0, false, 1, &scanKey);
	if (!HeapScanIsValid(scan))
		elog(ERROR, "GetDatabaseInfo: cannot begin scan of %s", DatabaseRelationName);

	/*
	 * Since we're going to close the relation, copy the tuple.
	 */
	tup = heap_getnext(scan, 0, &buf);

	if (HeapTupleIsValid(tup))
	{
		dbtup = heap_copytuple(tup);
		ReleaseBuffer(buf);
	}
	else
		dbtup = tup;

	heap_endscan(scan);

	if (!HeapTupleIsValid(dbtup))
	{
		elog(NOTICE, "GetDatabaseInfo: %s entry not found %s",
			 DatabaseRelationName, name);
		return TRUE;
	}

	dbowner = (Oid) heap_getattr(dbtup,
								 Anum_pg_database_datdba,
								 RelationGetTupleDescriptor(dbrel),
								 (char *) NULL);
	dbid = dbtup->t_oid;

	dbtext = (text *) heap_getattr(dbtup,
								   Anum_pg_database_datpath,
								   RelationGetTupleDescriptor(dbrel),
								   (char *) NULL);

	memcpy(dbpath, VARDATA(dbtext), (VARSIZE(dbtext) - VARHDRSZ));
	*(dbpath + (VARSIZE(dbtext) - VARHDRSZ)) = '\0';

	heap_close(dbrel);

	owner = palloc(sizeof(Oid));
	*owner = dbowner;
	path = palloc(strlen(dbpath) + 1);
	strcpy(path, dbpath);

	return FALSE;
}	/* GetDatabaseInfo() */

char *
ExpandDatabasePath(char *dbpath)
{
	char	   *path;
	char	   *cp;
	char		buf[MAXPGPATH + 1];

	/* leading path delimiter? then already absolute path */
	if (*dbpath == SEP_CHAR)
	{
#ifdef ALLOW_ABSOLUTE_DBPATHS
		cp = strrchr(dbpath, SEP_CHAR);
		strncpy(buf, dbpath, (cp - dbpath));
		sprintf(&buf[cp - dbpath], "%cbase%c%s", SEP_CHAR, SEP_CHAR, (cp + 1));
#else
		return NULL;
#endif
	}
	/* path delimiter somewhere? then has leading environment variable */
	else if (strchr(dbpath, SEP_CHAR) != NULL)
	{
		cp = strchr(dbpath, SEP_CHAR);
		strncpy(buf, dbpath, (cp - dbpath));
		buf[cp - dbpath] = '\0';
		path = getenv(buf);

		/*
		 * problem getting environment variable? let calling routine
		 * handle it
		 */
		if (path == NULL)
			return path;

		sprintf(buf, "%s%cbase%c%s", path, SEP_CHAR, SEP_CHAR, (cp + 1));
	}
	/* no path delimiter? then add the default path prefixes */
	else
		sprintf(buf, "%s%cbase%c%s", DataDir, SEP_CHAR, SEP_CHAR, dbpath);

	path = palloc(strlen(buf) + 1);
	strcpy(path, buf);

	return path;
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
 *		This algorithm relies on the fact that first attribute in the
 *		pg_database relation schema is the database name.  It also knows
 *		about the internal format of tuples on disk and the length of
 *		the datname attribute.	It knows the location of the pg_database
 *		file.
 *		Actually, the code looks as though it is using the pg_database
 *		tuple definition to locate the database name, so the above statement
 *		seems to be no longer correct. - thomas 1997-11-01
 *
 *		This code is called from InitPostgres(), before we chdir() to the
 *		local database directory and before we open any relations.
 *		Used to be called after the chdir(), but we now want to confirm
 *		the location of the target database using pg_database info.
 *		- thomas 1997-11-01
 * --------------------------------
 */
void
#ifdef MB
GetRawDatabaseInfo(char *name, Oid *owner, Oid *db_id, char *path, int *encoding)
#else
GetRawDatabaseInfo(char *name, Oid *owner, Oid *db_id, char *path)
#endif
{
	int			dbfd;
	int			fileflags;
	int			nbytes;
	int			max,
				i;
	HeapTuple	tup;
	Page		pg;
	PageHeader	ph;
	char	   *dbfname;
	Form_pg_database tup_db;

	dbfname = (char *) palloc(strlen(DataDir) + strlen("pg_database") + 2);
	sprintf(dbfname, "%s%cpg_database", DataDir, SEP_CHAR);
	fileflags = O_RDONLY;

	if ((dbfd = open(dbfname, O_RDONLY, 0)) < 0)
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
			tup = (HeapTuple) (((char *) pg) + offset);

			/*
			 * if the tuple has been deleted (the database was destroyed),
			 * skip this tuple.  XXX warning, will robinson:  violation of
			 * transaction semantics happens right here.  we should check
			 * to be sure that the xact that deleted this tuple actually
			 * committed.  only way to do this at init time is to paw over
			 * the log relation by hand, too.  let's be optimistic.
			 *
			 * XXX This is an evil type cast.  tup->t_xmax is char[5] while
			 * TransactionId is struct * { char data[5] }.	It works but
			 * if data is ever moved and no longer the first field this
			 * will be broken!! -mer 11 Nov 1991.
			 */
			if (TransactionIdIsValid((TransactionId) tup->t_xmax))
				continue;

			/*
			 * Okay, see if this is the one we want. XXX 1 july 91:  mao
			 * and mer discover that tuples now squash t_bits.	Why is
			 * this?
			 *
			 * 24 july 92:	mer realizes that the t_bits field is only used
			 * in the event of null values.  If no fields are null we
			 * reduce the header size by doing the squash.	t_hoff tells
			 * you exactly how big the header actually is. use the PC
			 * means of getting at sys cat attrs.
			 */
			tup_db = (Form_pg_database) GETSTRUCT(tup);

			if (strcmp(name, tup_db->datname.data) == 0)
			{
				*db_id = tup->t_oid;
				strncpy(path, VARDATA(&(tup_db->datpath)),
						(VARSIZE(&(tup_db->datpath)) - VARHDRSZ));
				*(path + VARSIZE(&(tup_db->datpath)) - VARHDRSZ) = '\0';
#ifdef MB
				*encoding = tup_db->encoding;
#endif
				goto done;
			}
		}
	}

done:
	close(dbfd);
	pfree(pg);
}	/* GetRawDatabaseInfo() */
