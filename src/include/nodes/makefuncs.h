/*-------------------------------------------------------------------------
 *
 * makefuncs.h--
 *    prototypes for the creator functions (for primitive nodes)
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: makefuncs.h,v 1.1 1996/08/28 01:57:33 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef MAKEFUNC_H
#define MAKEFUNC_H

#include "access/attnum.h"
#include "catalog/pg_operator.h"
#include "utils/fcache.h"
#include "nodes/primnodes.h"

extern Oper *makeOper(Oid opno,
		      Oid opid,
		      Oid opresulttype,
		      int opsize,
		      FunctionCachePtr op_fcache);

extern Var *makeVar(Index varno, 
		    AttrNumber varattno,
		    Oid vartype,
		    Index varnoold,
		    AttrNumber varoattno);

extern Resdom *makeResdom(AttrNumber resno,
			  Oid restype,
			  int reslen,
			  char *resname,
			  Index reskey,
			  Oid reskeyop,
			  int resjunk);
     
extern Const *makeConst(Oid consttype,
			Size constlen,
			Datum constvalue,
			bool constisnull,
			bool constbyval,
			bool constisset);

#endif	/* MAKEFUNC_H */
