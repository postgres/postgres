/*-------------------------------------------------------------------------
 *
 * restrictinfo.h
 *	  prototypes for restrictinfo.c.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: restrictinfo.h,v 1.10 2001/01/24 19:43:26 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RESTRICTINFO_H
#define RESTRICTINFO_H

#include "nodes/relation.h"

extern bool restriction_is_or_clause(RestrictInfo *restrictinfo);
extern List *get_actual_clauses(List *restrictinfo_list);
extern void get_actual_join_clauses(List *restrictinfo_list,
									List **joinquals, List **otherquals);

#endif	 /* RESTRICTINFO_H */
