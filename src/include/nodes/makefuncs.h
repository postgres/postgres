/*-------------------------------------------------------------------------
 *
 * makefuncs.h--
 *	  prototypes for the creator functions (for primitive nodes)
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: makefuncs.h,v 1.15 1998/10/01 22:51:22 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef MAKEFUNC_H
#define MAKEFUNC_H

#include <nodes/primnodes.h>
#include <nodes/parsenodes.h>
#include <utils/fcache.h>

extern Oper *makeOper(Oid opno,
					  Oid opid,
					  Oid opresulttype,
					  int opsize,
					  FunctionCachePtr op_fcache);

extern Var *makeVar(Index varno,
					AttrNumber varattno,
					Oid vartype,
					int32 vartypmod,
					Index varlevelsup,
					Index varnoold,
					AttrNumber varoattno);

extern TargetEntry *makeTargetEntry(Resdom *resdom, Node *expr);

extern Resdom *makeResdom(AttrNumber resno,
						  Oid restype,
						  int32 restypmod,
						  char *resname,
						  Index reskey,
						  Oid reskeyop,
						  int resjunk);

extern Const *makeConst(Oid consttype,
						int constlen,
						Datum constvalue,
						bool constisnull,
						bool constbyval,
						bool constisset,
						bool constiscast);

#endif	 /* MAKEFUNC_H */
