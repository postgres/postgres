/*-------------------------------------------------------------------------
 *
 * makefuncs.h
 *	  prototypes for the creator functions (for primitive nodes)
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: makefuncs.h,v 1.40 2002/09/04 20:31:43 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef MAKEFUNC_H
#define MAKEFUNC_H

#include "nodes/parsenodes.h"

extern A_Expr *makeA_Expr(int oper, List *name, Node *lexpr, Node *rexpr);

extern A_Expr *makeSimpleA_Expr(int oper, const char *name,
				 Node *lexpr, Node *rexpr);

extern Oper *makeOper(Oid opno,
		 Oid opid,
		 Oid opresulttype,
		 bool opretset);

extern Var *makeVar(Index varno,
		AttrNumber varattno,
		Oid vartype,
		int32 vartypmod,
		Index varlevelsup);

extern TargetEntry *makeTargetEntry(Resdom *resdom, Node *expr);

extern Resdom *makeResdom(AttrNumber resno,
		   Oid restype,
		   int32 restypmod,
		   char *resname,
		   bool resjunk);

extern Const *makeConst(Oid consttype,
		  int constlen,
		  Datum constvalue,
		  bool constisnull,
		  bool constbyval,
		  bool constisset,
		  bool constiscast);

extern Const *makeNullConst(Oid consttype);

extern Alias *makeAlias(const char *aliasname, List *colnames);

extern RelabelType *makeRelabelType(Node *arg, Oid rtype, int32 rtypmod);

extern RangeVar *makeRangeVar(char *schemaname, char *relname);

extern TypeName *makeTypeName(char *typnam);

#endif   /* MAKEFUNC_H */
