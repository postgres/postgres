/*-------------------------------------------------------------------------
 *
 * purge.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: purge.h,v 1.3 1997/09/08 02:35:53 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PURGE_H
#define PURGE_H

extern		int32
RelationPurge(char *relationName,
			  char *absoluteTimeString,
			  char *relativeTimeString);

#endif							/* PURGE_H */
