/*-------------------------------------------------------------------------
 *
 * version.c--
 *    Routines to handle Postgres version number.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/utils/Attic/version.c,v 1.2 1996/11/11 14:44:04 scrappy Exp $
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
#include <unistd.h>
#include <errno.h>

#include "postgres.h"

#include "storage/fd.h"	 /* for O_ */

#include "version.h"    


static void
PathSetVersionFilePath(const char *path, char *filepathbuf) {
/*----------------------------------------------------------------------------
  PathSetVersionFilePath
 
  Destructively change "filepathbuf" to contain the concatenation of "path"
  and the name of the version file name.
----------------------------------------------------------------------------*/
    if (strlen(path) > (MAXPGPATH - sizeof(PG_VERFILE) - 1))
        *filepathbuf = '\0';
    else
        sprintf(filepathbuf, "%s%c%s", path, SEP_CHAR, PG_VERFILE);
}



void
ValidatePgVersion(const char *path, char **reason_p) {
/*----------------------------------------------------------------------------
    Determine whether the PG_VERSION file in directory <path> indicates
    a data version compatible with the version of this program.
 
    If compatible, return <*reason_p> == NULL.  Otherwise, malloc space,
    fill it with a text string explaining how it isn't compatible (or why
    we can't tell), and return a pointer to that space as <*reason_p>.
-----------------------------------------------------------------------------*/
    int	 fd;
    char version[4];
    char full_path[MAXPGPATH+1];
#ifndef WIN32
    struct stat	statbuf;
#else    
    struct _stat statbuf;
#endif    
    PathSetVersionFilePath(path, full_path);
    
    if (stat(full_path, &statbuf) < 0) {
        *reason_p = malloc(200);
        sprintf(*reason_p, "File '%s' does not exist.", full_path);
    } else {
        fd = open(full_path, O_RDONLY, 0);
        if (fd < 0) {
            *reason_p = malloc(200);
            sprintf(*reason_p, "Unable to open file '%s'.  Errno = %s (%d).",
                    full_path, strerror(errno), errno);
        } else {
            if (read(fd, version, 4) < 4 ||
                !isascii(version[0]) || !isdigit(version[0]) ||
                version[1] != '.' ||
                !isascii(version[2]) || !isdigit(version[2]) ||
                version[3] != '\n') {

                *reason_p = malloc(200);
                sprintf(*reason_p, "File '%s' does not have a valid format "
                        "for a PG_VERSION file.", full_path);
            } else {
                if (version[2] != '0' + PG_VERSION ||
                    version[0] != '0' + PG_RELEASE) {
                    *reason_p = malloc(200);
                    sprintf(*reason_p, 
                            "Version number in file '%s' should be %d.%d, "
                            "not %c.%c.",
                            full_path, 
                            PG_RELEASE, PG_VERSION, version[0], version[2]);
                } else *reason_p = NULL;
            }
            close(fd);                
        }
    }
}



void
SetPgVersion(const char *path, char **reason_p) {
/*---------------------------------------------------------------------------
  Create the PG_VERSION file in the directory <path>.
  
  If we fail, allocate storage, fill it with a text string explaining why,
  and return a pointer to that storage as <*reason_p>.  If we succeed,
  return *reason_p = NULL.
---------------------------------------------------------------------------*/
    int	fd;
    char version[4];
    char full_path[MAXPGPATH+1];
    
    PathSetVersionFilePath(path, full_path);
    
    fd = open(full_path, O_WRONLY|O_CREAT|O_EXCL, 0666);
    if (fd < 0) {
        *reason_p = malloc(100 + strlen(full_path));
        sprintf(*reason_p, 
                "Unable to create file '%s', errno from open(): %s (%d).",
                full_path, strerror(errno), errno);
    } else {
        int rc;  /* return code from some function we call */

        version[0] = '0' + PG_RELEASE;
        version[1] = '.';
        version[2] = '0' + PG_VERSION;
        version[3] = '\n';
        rc = write(fd, version, 4);
        if (rc != 4) {
            *reason_p = malloc(100 + strlen(full_path));
            sprintf(*reason_p, 
                    "Failed to write to file '%s', after it was already "
                    "open.  Errno from write(): %s (%d)", 
                    full_path, strerror(errno), errno);
        } else *reason_p = NULL;
        close(fd);
    }
}
