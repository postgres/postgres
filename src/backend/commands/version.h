/*-------------------------------------------------------------------------
 *
 * version.h--
 *    Header file for versions.  
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: version.h,v 1.1.1.1 1996/07/09 06:21:23 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VERSION_H
#define VERSION_H

#include "postgres.h"
#include "nodes/pg_list.h"

extern void DefineVersion(char *name, char *fromRelname, char *date);
extern void VersionCreate(char *vname, char *bname);
extern void VersionAppend(char *vname, char *bname);
extern void VersionRetrieve(char *vname, char *bname, char *snapshot);
extern void VersionDelete(char *vname, char *bname, char *snapshot);
extern void VersionReplace(char *vname, char *bname, char *snapshot);
    
#endif	/* VERSION_H */
