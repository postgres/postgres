/*-------------------------------------------------------------------------
 *
 * magic.c--
 *    magic number management routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/utils/init/Attic/magic.c,v 1.1.1.1 1996/07/09 06:22:09 scrappy Exp $
 *
 * NOTES
 *	XXX eventually, should be able to handle version identifiers
 *	of length != 4.
 *
 *  STANDALONE CODE - do not use error routines as this code is linked with
 *  stuff that does not cinterface.a
 *-------------------------------------------------------------------------
 */
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>

#include "postgres.h"

#include "utils/elog.h"
#include "miscadmin.h"		/* for global decls */

#include "storage/fd.h"		/* for O_ */

static char	Pg_verfile[] = PG_VERFILE;


/*
 * private function prototypes
 */
static void PathSetVersionFilePath(char path[], char filepathbuf[]);

/*
 * DatabaseMetaGunkIsConsistent
 *
 * Returns 1 iff all version numbers and ownerships are consistent.
 *
 * Note that we have to go through the whole rigmarole of generating the path
 * and checking the existence of the database whether Noversion is set or not.
 */
int
DatabaseMetaGunkIsConsistent(char *database, char *path)
{
    int		isValid;
#ifndef WIN32
    struct stat	statbuf;
#else    
    struct _stat statbuf;
#endif

    /* XXX We haven't changed PG_VERSION since 1.1! */
#ifndef WIN32    
    isValid = ValidPgVersion(DataDir);
    sprintf(path, "%s/base/%s", DataDir, database);
    isValid = ValidPgVersion(path) || isValid;
#endif /* WIN32 */
    
    if (stat(path, &statbuf) < 0)
	elog(FATAL, "database %s does not exist, bailing out...",
	     database);
    
    return(isValid);
}


/*
 * ValidPgVersion	- verifies the consistency of the database
 *
 *	Returns 1 iff the catalog version number (from the version number file
 *	in the directory specified in "path") is consistent with the backend
 *	version number.
 */
int
ValidPgVersion(char *path)
{
    int		fd;
    char		version[4], buf[MAXPGPATH+1];
#ifndef WIN32
    struct stat	statbuf;
#else    
    struct _stat statbuf;
#endif    
    u_short		my_euid = geteuid();
    
    PathSetVersionFilePath(path, buf);
    
    if (stat(buf, &statbuf) >= 0) {
	if (statbuf.st_uid != my_euid && my_euid != 0)
	    elog(FATAL,
		 "process userid (%d) != database owner (%d)",
		 my_euid, statbuf.st_uid);
    } else
	return(0);
    
    if ((fd = open(buf, O_RDONLY, 0)) < 0) {
	if (!Noversion)
	    elog(DEBUG, "ValidPgVersion: %s: %m", buf);
	return(0);
    }
    
    if (read(fd, version, 4) < 4 ||
	!isascii(version[0]) || !isdigit(version[0]) ||
	version[1] != '.' ||
	!isascii(version[2]) || !isdigit(version[2]) ||
	version[3] != '\n')
	elog(FATAL, "ValidPgVersion: %s: bad format", buf);
    if (version[2] != '0' + PG_VERSION ||
	version[0] != '0' + PG_RELEASE) {
	if (!Noversion)
	    elog(DEBUG,
		 "ValidPgVersion: should be %d.%d not %c.%c",
		 PG_RELEASE, PG_VERSION, version[0], version[2]);
	close(fd);
	return(0);
    }
    close(fd);
    return(1);
}


/*
 *	SetPgVersion	- writes the version to a database directory
 */
void
SetPgVersion(char *path)
{
    int	fd;
    char	version[4], buf[MAXPGPATH+1];
    
    PathSetVersionFilePath(path, buf);
    
    if ((fd = open(buf, O_WRONLY|O_CREAT|O_EXCL, 0666)) < 0)
	elog(FATAL, "SetPgVersion: %s: %m", buf);
    
    version[0] = '0' + PG_RELEASE;
    version[1] = '.';
    version[2] = '0' + PG_VERSION;
    version[3] = '\n';
    if (write(fd, version, 4) != 4)
	elog(WARN, "SetPgVersion: %s: %m", buf);
    
    close(fd);
}


/*
 * PathSetVersionFilePath
 *
 * Destructively change "filepathbuf" to contain the concatenation of "path"
 * and the name of the version file name.
 */
static void
PathSetVersionFilePath(char *path, char *filepathbuf)
{
    if (strlen(path) > (MAXPGPATH - sizeof(Pg_verfile) - 1))
	elog(FATAL, "PathSetVersionFilePath: %s: path too long");
    (void) sprintf(filepathbuf, "%s%c%s", path, SEP_CHAR, Pg_verfile);
}
