/*-------------------------------------------------------------------------
 *
 * database.c
 *	  miscellaneous initialization support stuff
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/misc/Attic/database.c,v 1.58 2003/08/04 02:40:08 momjian Exp $
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
#include "miscadmin.h"
#include "utils/syscache.h"


static bool PhonyHeapTupleSatisfiesNow(HeapTupleHeader tuple);


/*
 * ExpandDatabasePath resolves a proposed database path (obtained from
 * pg_database.datpath) to a full absolute path for further consumption.
 * NULL means an error, which the caller should process. One reason for
 * such an error would be an absolute alternative path when no absolute
 * paths are allowed.
 */

char *
ExpandDatabasePath(const char *dbpath)
{
	char		buf[MAXPGPATH];
	const char *cp;
	int			len;

	AssertArg(dbpath);
	Assert(DataDir);

	if (strlen(dbpath) >= MAXPGPATH)
		return NULL;			/* ain't gonna fit nohow */

	/* leading path delimiter? then already absolute path */
	if (is_absolute_path(dbpath))
	{
#ifdef ALLOW_ABSOLUTE_DBPATHS
		cp = last_path_separator(dbpath);
		len = cp - dbpath;
		strncpy(buf, dbpath, len);
		snprintf(&buf[len], MAXPGPATH - len, "/base/%s", (cp + 1));
#else
		return NULL;
#endif
	}
	/* path delimiter somewhere? then has leading environment variable */
	else if ((cp = first_path_separator(dbpath)) != NULL)
	{
		const char *envvar;

		len = cp - dbpath;
		strncpy(buf, dbpath, len);
		buf[len] = '\0';
		envvar = getenv(buf);
		if (envvar == NULL)
			return NULL;

		snprintf(buf, sizeof(buf), "%s/base/%s", envvar, (cp + 1));
	}
	else
	{
		/* no path delimiter? then add the default path prefix */
		snprintf(buf, sizeof(buf), "%s/base/%s", DataDir, dbpath);
	}

	/*
	 * check for illegal characters in dbpath these should really throw an
	 * error, shouldn't they? or else all callers need to test for NULL
	 */
	for (cp = buf; *cp; cp++)
	{
		/*
		 * The following characters will not be allowed anywhere in the
		 * database path. (Do not include the slash  or '.' here.)
		 */
		char		illegal_dbpath_chars[] =
		"\001\002\003\004\005\006\007\010"
		"\011\012\013\014\015\016\017\020"
		"\021\022\023\024\025\026\027\030"
		"\031\032\033\034\035\036\037"
		"'`";

		const char *cx;

		for (cx = illegal_dbpath_chars; *cx; cx++)
			if (*cp == *cx)
				return NULL;
		/* don't allow access to parent dirs */
		if (strncmp(cp, "/../", 4) == 0)
			return NULL;
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
GetRawDatabaseInfo(const char *name, Oid *db_id, char *path)
{
	int			dbfd;
	int			nbytes;
	int			pathlen;
	HeapTupleData tup;
	Page		pg;
	char	   *dbfname;
	Form_pg_database tup_db;
	RelFileNode rnode;

	rnode.tblNode = 0;
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
			 * database OID from a flat file, handled the same way we
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
				/* Found it; extract the OID and the database path. */
				*db_id = HeapTupleGetOid(&tup);
				pathlen = VARSIZE(&(tup_db->datpath)) - VARHDRSZ;
				if (pathlen < 0)
					pathlen = 0;	/* pure paranoia */
				if (pathlen >= MAXPGPATH)
					pathlen = MAXPGPATH - 1;	/* more paranoia */
				strncpy(path, VARDATA(&(tup_db->datpath)), pathlen);
				path[pathlen] = '\0';
				goto done;
			}
		}
	}

	/* failed to find it... */
	*db_id = InvalidOid;
	*path = '\0';

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
