/*-------------------------------------------------------------------------
 *
 * clauseinfo.h--
 *	  prototypes for clauseinfo.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: clauseinfo.h,v 1.9 1998/09/01 04:36:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLAUSEINFO_H
#define CLAUSEINFO_H

#include "nodes/pg_list.h"
#include "nodes/relation.h"

extern bool valid_or_clause(ClauseInfo * clauseinfo);
extern List *get_actual_clauses(List *clauseinfo_list);
extern void get_relattvals(List *clauseinfo_list, List **attnos,
			   List **values, List **flags);
extern void get_joinvars(Oid relid, List *clauseinfo_list,
			 List **attnos, List **values, List **flags);
extern List *get_opnos(List *clauseinfo_list);

#endif	 /* CLAUSEINFO_H */
