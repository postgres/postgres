/*-------------------------------------------------------------------------
 *
 * plancat.h
 *	  prototypes for plancat.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: plancat.h,v 1.14 1999/11/21 23:25:42 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANCAT_H
#define PLANCAT_H

#include "nodes/parsenodes.h"


extern void relation_info(Query *root, Index relid,
						  bool *hasindex, int *pages, int *tuples);

extern List *find_secondary_indexes(Query *root, Index relid);

extern Cost restriction_selectivity(Oid functionObjectId,
						Oid operatorObjectId,
						Oid relationObjectId,
						AttrNumber attributeNumber,
						Datum constValue,
						int constFlag);

extern void index_selectivity(Query *root, int relid, Oid indexid,
							  List *indexquals,
							  float *idxPages, float *idxSelec);

extern Cost join_selectivity(Oid functionObjectId, Oid operatorObjectId,
				 Oid relationObjectId1, AttrNumber attributeNumber1,
				 Oid relationObjectId2, AttrNumber attributeNumber2);

extern List *find_inheritance_children(Oid inhparent);

#endif	 /* PLANCAT_H */
