/*-------------------------------------------------------------------------
 *
 * cluster.h--
 *	  header file for postgres cluster command stuff
 *
 * Copyright (c) 1994-5, Regents of the University of California
 *
 * $Id: cluster.h,v 1.3 1997/09/07 04:57:21 momjian Exp $
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
extern void		cluster(char oldrelname[], char oldindexname[]);

#endif							/* CLUSTER_H */
