/*-------------------------------------------------------------------------
 *
 * joininfo.h
 *	  prototypes for joininfo.c.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: joininfo.h,v 1.23 2003/01/24 03:58:43 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef JOININFO_H
#define JOININFO_H

#include "nodes/relation.h"


extern JoinInfo *find_joininfo_node(RelOptInfo *this_rel, Relids join_relids);
extern JoinInfo *make_joininfo_node(RelOptInfo *this_rel, Relids join_relids);

extern void add_join_clause_to_rels(Query *root,
									RestrictInfo *restrictinfo,
									Relids join_relids);
extern void remove_join_clause_from_rels(Query *root,
										 RestrictInfo *restrictinfo,
										 Relids join_relids);

#endif   /* JOININFO_H */
