/*-------------------------------------------------------------------------
 *
 * sets.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: sets.h,v 1.2 1997/09/07 05:03:01 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SETS_H
#define SETS_H

/* Temporary name of set, before SetDefine changes it. */
#define GENERICSETNAME "zyxset"

extern Oid		SetDefine(char *querystr, char *typename);
extern int		seteval(Oid funcoid);

#endif							/* SETS_H */
