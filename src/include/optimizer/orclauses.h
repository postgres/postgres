/*-------------------------------------------------------------------------
 *
 * orclauses.h
 *	  prototypes for orclauses.c.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/orclauses.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ORCLAUSES_H
#define ORCLAUSES_H

#include "nodes/relation.h"

extern void extract_restriction_or_clauses(PlannerInfo *root);

#endif   /* ORCLAUSES_H */
