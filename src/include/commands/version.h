/*-------------------------------------------------------------------------
 *
 * version.h--
 *    Header file for versions.  
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: version.h,v 1.2 1996/10/31 09:48:24 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VERSION_H
#define VERSION_H

#include "nodes/pg_list.h"

extern void DefineVersion(char *name, char *fromRelname, char *date);
extern void VersionCreate(char *vname, char *bname);
extern void VersionAppend(char *vname, char *bname);
extern void VersionRetrieve(char *vname, char *bname, char *snapshot);
extern void VersionDelete(char *vname, char *bname, char *snapshot);
extern void VersionReplace(char *vname, char *bname, char *snapshot);
    
#endif	/* VERSION_H */
