/*-------------------------------------------------------------------------
 *
 * plancat.h
 *	  prototypes for plancat.c.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: plancat.h,v 1.17 2000/01/26 05:58:20 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANCAT_H
#define PLANCAT_H

#include "nodes/relation.h"


extern void relation_info(Query *root, Index relid,
						  bool *hasindex, long *pages, double *tuples);

extern List *find_secondary_indexes(Query *root, Index relid);

extern List *find_inheritance_children(Oid inhparent);

extern Selectivity restriction_selectivity(Oid functionObjectId,
						Oid operatorObjectId,
						Oid relationObjectId,
						AttrNumber attributeNumber,
						Datum constValue,
						int constFlag);

extern Selectivity join_selectivity(Oid functionObjectId, Oid operatorObjectId,
				 Oid relationObjectId1, AttrNumber attributeNumber1,
				 Oid relationObjectId2, AttrNumber attributeNumber2);

#endif	 /* PLANCAT_H */
