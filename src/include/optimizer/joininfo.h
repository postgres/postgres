/*-------------------------------------------------------------------------
 *
 * joininfo.h
 *	  prototypes for joininfo.c.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: joininfo.h,v 1.22 2003/01/20 18:55:04 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef JOININFO_H
#define JOININFO_H

#include "nodes/relation.h"

extern JoinInfo *find_joininfo_node(RelOptInfo *this_rel, List *join_relids);
extern JoinInfo *make_joininfo_node(RelOptInfo *this_rel, List *join_relids);

#endif   /* JOININFO_H */
