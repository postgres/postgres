/*-------------------------------------------------------------------------
 *
 * sets.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: sets.h,v 1.4 1998/09/01 04:39:33 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SETS_H
#define SETS_H

/* Temporary name of set, before SetDefine changes it. */
#define GENERICSETNAME "zyxset"

extern Oid	SetDefine(char *querystr, char *typename);
extern int	seteval(Oid funcoid);

#endif	 /* SETS_H */
