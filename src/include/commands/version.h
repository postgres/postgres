/*-------------------------------------------------------------------------
 *
 * version.h--
 *    Header file for versions.  
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: version.h,v 1.3 1996/11/06 10:29:33 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VERSION_H
#define VERSION_H


extern void DefineVersion(char *name, char *fromRelname, char *date);
extern void VersionCreate(char *vname, char *bname);
extern void VersionAppend(char *vname, char *bname);
extern void VersionRetrieve(char *vname, char *bname, char *snapshot);
extern void VersionDelete(char *vname, char *bname, char *snapshot);
extern void VersionReplace(char *vname, char *bname, char *snapshot);
    
#endif	/* VERSION_H */
