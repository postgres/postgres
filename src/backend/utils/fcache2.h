/*-------------------------------------------------------------------------
 *
 * fcache2.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: fcache2.h,v 1.1.1.1 1996/07/09 06:22:01 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FCACHE2_H
#define FCACHE2_H

extern void
setFcache(Node *node, Oid foid, List *argList, ExprContext *econtext);

#endif	/* FCACHE2_H */
