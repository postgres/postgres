/*-------------------------------------------------------------------------
 *
 * sets.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: sets.h,v 1.13 2002/03/29 19:06:26 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SETS_H
#define SETS_H

#include "fmgr.h"


/* Temporary name of a set function, before SetDefine changes it. */
#define GENERICSETNAME "ZYX#Set#ZYX"

extern Oid	SetDefine(char *querystr, Oid elemType);

extern Datum seteval(PG_FUNCTION_ARGS);

#endif   /* SETS_H */
