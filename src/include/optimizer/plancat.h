/*-------------------------------------------------------------------------
 *
 * plancat.h
 *	  prototypes for plancat.c.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: plancat.h,v 1.21 2001/01/24 19:43:26 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANCAT_H
#define PLANCAT_H

#include "nodes/relation.h"


extern void relation_info(Oid relationObjectId,
						  bool *hasindex, long *pages, double *tuples);

extern List *find_secondary_indexes(Oid relationObjectId);

extern List *find_inheritance_children(Oid inhparent);

extern bool has_subclass(Oid relationId);

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
