/*-------------------------------------------------------------------------
 *
 * restrictinfo.h
 *	  prototypes for restrictinfo.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: restrictinfo.h,v 1.7 1999/07/25 23:07:23 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RESTRICTINFO_H
#define RESTRICTINFO_H

#include "nodes/relation.h"

extern bool restriction_is_or_clause(RestrictInfo *restrictinfo);
extern List *get_actual_clauses(List *restrictinfo_list);

#endif	 /* RESTRICTINFO_H */
