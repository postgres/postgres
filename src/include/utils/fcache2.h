/*-------------------------------------------------------------------------
 *
 * fcache2.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: fcache2.h,v 1.6 1998/01/24 22:50:42 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FCACHE2_H
#define FCACHE2_H

#include <nodes/execnodes.h>

extern void setFcache(Node *node, Oid foid, List *argList, ExprContext *econtext);

#endif							/* FCACHE2_H */
