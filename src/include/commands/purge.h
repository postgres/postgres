/*-------------------------------------------------------------------------
 *
 * purge.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: purge.h,v 1.2 1997/09/07 04:57:26 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PURGE_H
#define PURGE_H

extern int32
RelationPurge(char *relationName,
			  char *absoluteTimeString,
			  char *relativeTimeString);

#endif							/* PURGE_H */
