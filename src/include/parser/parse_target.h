/*-------------------------------------------------------------------------
 *
 * parse_target.h
 *	  handle target lists
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/parser/parse_target.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_TARGET_H
#define PARSE_TARGET_H

#include "parser/parse_node.h"


extern List *transformTargetList(ParseState *pstate, List *targetlist,
								 ParseExprKind exprKind);
extern List *transformExpressionList(ParseState *pstate, List *exprlist,
									 ParseExprKind exprKind, bool allowDefault);
extern void resolveTargetListUnknowns(ParseState *pstate, List *targetlist);
extern void markTargetListOrigins(ParseState *pstate, List *targetlist);
extern TargetEntry *transformTargetEntry(ParseState *pstate,
										 Node *node, Node *expr, ParseExprKind exprKind,
										 char *colname, bool resjunk);
extern Expr *transformAssignedExpr(ParseState *pstate, Expr *expr,
								   ParseExprKind exprKind,
								   const char *colname,
								   int attrno,
								   List *indirection,
								   int location);
extern void updateTargetListEntry(ParseState *pstate, TargetEntry *tle,
								  char *colname, int attrno,
								  List *indirection,
								  int location);
extern Node *transformAssignmentIndirection(ParseState *pstate,
											Node *basenode,
											const char *targetName,
											bool targetIsSubscripting,
											Oid targetTypeId,
											int32 targetTypMod,
											Oid targetCollation,
											List *indirection,
											ListCell *indirection_cell,
											Node *rhs,
											CoercionContext ccontext,
											int location);
extern List *checkInsertTargets(ParseState *pstate, List *cols,
								List **attrnos);
extern TupleDesc expandRecordVariable(ParseState *pstate, Var *var,
									  int levelsup);
extern char *FigureColname(Node *node);
extern char *FigureIndexColname(Node *node);

#endif							/* PARSE_TARGET_H */
