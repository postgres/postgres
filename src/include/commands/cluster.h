/*-------------------------------------------------------------------------
 *
 * cluster.h
 *	  header file for postgres cluster command stuff
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * $Id: cluster.h,v 1.9 2001/01/24 19:43:23 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLUSTER_H
#define CLUSTER_H

/*
 * defines for contant stuff
 */
#define _TEMP_RELATION_KEY_				"clXXXXXXXX"
#define _SIZE_OF_TEMP_RELATION_KEY_		11


/*
 * functions
 */
extern void cluster(char *oldrelname, char *oldindexname);

#endif	 /* CLUSTER_H */
