/*-------------------------------------------------------------------------
 *
 * purge.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: purge.h,v 1.1 1996/08/28 07:21:48 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	PURGE_H
#define	PURGE_H

extern int32 RelationPurge(char *relationName,
			   char *absoluteTimeString,
			   char *relativeTimeString);

#endif	/* PURGE_H */
