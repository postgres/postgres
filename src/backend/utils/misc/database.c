/*-------------------------------------------------------------------------
 *
 * database.c
 *	  miscellaneous initialization support stuff
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/misc/Attic/database.c,v 1.37 2000/04/12 17:16:07 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "access/xact.h"
#include "catalog/catname.h"
#include "catalog/pg_database.h"
#include "miscadmin.h"
#include "utils/syscache.h"


/*
 * ExpandDatabasePath resolves a proposed database path (obtained from
 * pg_database.datpath) to a full absolute path for further consumption.
 * NULL means an error, which the caller should process. One reason for
 * such an error would be an absolute alternative path when no absolute
 * paths are alllowed.
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
	if (*dbpath == SEP_CHAR)
	{
#ifdef ALLOW_ABSOLUTE_DBPATHS
		cp = strrchr(dbpath, SEP_CHAR);
		len = cp - dbpath;
		strncpy(buf, dbpath, len);
		snprintf(&buf[len], MAXPGPATH - len, "%cbase%c%s",
				 SEP_CHAR, SEP_CHAR, (cp + 1));
#else
		return NULL;
#endif
	}
	/* path delimiter somewhere? then has leading environment variable */
	else if ((cp = strchr(dbpath, SEP_CHAR)) != NULL)
	{
		const char *envvar;

		len = cp - dbpath;
		strncpy(buf, dbpath, len);
		buf[len] = '\0';
		envvar = getenv(buf);
		if (envvar == NULL)
			return NULL;

		snprintf(buf, sizeof(buf), "%s%cbase%c%s",
				 envvar, SEP_CHAR, SEP_CHAR, (cp + 1));
	}
	else
	{
		/* no path delimiter? then add the default path prefix */
		snprintf(buf, sizeof(buf), "%s%cbase%c%s",
				 DataDir, SEP_CHAR, SEP_CHAR, dbpath);
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
	int			max,
				i;
	HeapTupleData tup;
	Page		pg;
	PageHeader	ph;
	char	   *dbfname;
	Form_pg_database tup_db;

	dbfname = (char *) palloc(strlen(DataDir) + strlen(DatabaseRelationName) + 2);
	sprintf(dbfname, "%s%c%s", DataDir, SEP_CHAR, DatabaseRelationName);

#ifndef __CYGWIN32__
	if ((dbfd = open(dbfname, O_RDONLY, 0)) < 0)
#else
	if ((dbfd = open(dbfname, O_RDONLY | O_BINARY, 0)) < 0)
#endif
		elog(FATAL, "cannot open %s: %s", dbfname, strerror(errno));

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
			 * accept a tuple that was stored and never committed.	All in
			 * all, this code is pretty shaky.	We will cross-check our
			 * result in ReverifyMyDatabase() in postinit.c.
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
