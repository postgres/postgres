/*-------------------------------------------------------------------------
 *
 * cluster.h--
 *    header file for postgres cluster command stuff 
 *
 * Copyright (c) 1994-5, Regents of the University of California
 *
 * $Id: cluster.h,v 1.1.1.1 1996/07/09 06:21:19 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	CLUSTER_H
#define	CLUSTER_H

/*
 * defines for contant stuff
 */
#define _TEMP_RELATION_KEY_ 		"clXXXXXXXX"
#define _SIZE_OF_TEMP_RELATION_KEY_ 	11


/*
 * functions
 */
extern void cluster(char oldrelname[], char oldindexname[]);
extern Relation copy_heap(Oid OIDOldHeap);
extern void copy_index(Oid OIDOldIndex, Oid OIDNewHeap);
extern void rebuildheap(Oid OIDNewHeap, Oid OIDOldHeap, Oid OIDOldIndex);

#endif	/* CLUSTER_H */
