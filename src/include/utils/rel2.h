/*-------------------------------------------------------------------------
 *
 * rel2.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rel2.h,v 1.8 1999/02/13 23:22:30 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TMP_REL2_H
#define TMP_REL2_H

#include <utils/rel.h>

extern IndexStrategy RelationGetIndexStrategy(Relation relation);

extern void RelationSetIndexSupport(Relation relation, IndexStrategy strategy,
						RegProcedure *support);

#endif	 /* TMP_REL2_H */
