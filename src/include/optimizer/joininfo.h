/*-------------------------------------------------------------------------
 *
 * joininfo.h
 *	  prototypes for joininfo.c.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: joininfo.h,v 1.16 2000/06/08 22:37:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef JOININFO_H
#define JOININFO_H

#include "nodes/relation.h"

extern JoinInfo *find_joininfo_node(RelOptInfo *this_rel, List *join_relids);

#endif	 /* JOININFO_H */
