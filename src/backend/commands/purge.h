/*-------------------------------------------------------------------------
 *
 * purge.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: purge.h,v 1.1.1.1 1996/07/09 06:21:21 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	PURGE_H
#define	PURGE_H

extern int32 RelationPurge(char *relationName,
			   char *absoluteTimeString,
			   char *relativeTimeString);

#endif	/* PURGE_H */
