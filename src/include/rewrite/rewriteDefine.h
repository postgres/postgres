/*-------------------------------------------------------------------------
 *
 * rewriteDefine.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteDefine.h,v 1.7 1999/02/13 23:21:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEDEFINE_H
#define REWRITEDEFINE_H

#include "nodes/parsenodes.h"

extern void DefineQueryRewrite(RuleStmt *args);

#endif	 /* REWRITEDEFINE_H */
