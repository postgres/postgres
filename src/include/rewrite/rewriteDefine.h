/*-------------------------------------------------------------------------
 *
 * rewriteDefine.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteDefine.h,v 1.5 1997/11/26 01:14:22 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEDEFINE_H
#define REWRITEDEFINE_H

#include "nodes/parsenodes.h"

extern void DefineQueryRewrite(RuleStmt *args);

#endif							/* REWRITEDEFINE_H */
