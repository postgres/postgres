/*-------------------------------------------------------------------------
 *
 * plancat.h--
 *	  prototypes for plancat.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: plancat.h,v 1.7 1998/01/24 22:49:50 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANCAT_H
#define PLANCAT_H

#include <nodes/parsenodes.h>

/*
 * transient data structure to hold return value of index_info. Note that
 * indexkeys, orderOprs and classlist is "null-terminated".
 */
typedef struct IdxInfoRetval
{
	Oid			relid;			/* OID of the index relation (not the OID
								 * of the relation being indexed) */
	Oid			relam;			/* OID of the pg_am of this index */
	int			pages;			/* number of pages in the index relation */
	int			tuples;			/* number of tuples in the index relation */
	int		   *indexkeys;		/* keys over which we're indexing */
	Oid		   *orderOprs;		/* operators used for ordering purposes */
	Oid		   *classlist;		/* classes of AM operators */
	Oid			indproc;
	Node	   *indpred;
} IdxInfoRetval;


extern void relation_info(Query *root,
			  Oid relid,
			  bool *hashindex, int *pages,
			  int *tuples);

extern bool index_info(Query *root,
		   bool first, int relid, IdxInfoRetval *info);

extern Cost restriction_selectivity(Oid functionObjectId,
						Oid operatorObjectId,
						Oid relationObjectId,
						AttrNumber attributeNumber,
						char *constValue,
						int32 constFlag);

extern void index_selectivity(Oid indid, Oid *classes, List *opnos,
				  Oid relid, List *attnos, List *values, List *flags,
				  int32 nkeys, float *idxPages, float *idxSelec);

extern Cost join_selectivity(Oid functionObjectId, Oid operatorObjectId,
				 Oid relationObjectId1, AttrNumber attributeNumber1,
				 Oid relationObjectId2, AttrNumber attributeNumber2);

extern List *find_inheritance_children(Oid inhparent);
extern List *VersionGetParents(Oid verrelid);

#endif							/* PLANCAT_H */
