/*-------------------------------------------------------------------------
 *
 * fcache2.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: fcache2.h,v 1.7 1998/09/01 04:39:08 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FCACHE2_H
#define FCACHE2_H

#include <nodes/execnodes.h>

extern void setFcache(Node *node, Oid foid, List *argList, ExprContext *econtext);

#endif	 /* FCACHE2_H */
