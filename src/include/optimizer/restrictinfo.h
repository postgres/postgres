/*-------------------------------------------------------------------------
 *
 * restrictinfo.h
 *	  prototypes for restrictinfo.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: restrictinfo.h,v 1.3 1999/05/25 16:14:22 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RESTRICTINFO_H
#define RESTRICTINFO_H

#include "nodes/pg_list.h"
#include "nodes/relation.h"

extern bool valid_or_clause(RestrictInfo * restrictinfo);
extern List *get_actual_clauses(List *restrictinfo_list);
extern void get_relattvals(List *restrictinfo_list, List **attnos,
			   List **values, List **flags);
extern void get_joinvars(Oid relid, List *restrictinfo_list,
			 List **attnos, List **values, List **flags);
extern List *get_opnos(List *restrictinfo_list);

#endif	 /* RESTRICTINFO_H */
