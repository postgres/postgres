/*-------------------------------------------------------------------------
 *
 * nodeFuncs.h
 *
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: nodeFuncs.h,v 1.18 2002/12/12 15:49:40 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEFUNCS_H
#define NODEFUNCS_H

#include "nodes/primnodes.h"

extern bool single_node(Node *node);
extern bool var_is_outer(Var *var);
extern bool var_is_rel(Var *var);
extern void set_opfuncid(OpExpr *opexpr);

#endif   /* NODEFUNCS_H */
