/*-------------------------------------------------------------------------
 *
 * sets.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: sets.h,v 1.1.1.1 1996/07/09 06:22:02 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SETS_H
#define SETS_H

/* Temporary name of set, before SetDefine changes it. */
#define GENERICSETNAME "zyxset"

extern Oid SetDefine(char *querystr, char *typename);
extern int seteval(Oid funcoid);

#endif	/* SETS_H */
