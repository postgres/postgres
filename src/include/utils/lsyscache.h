/*-------------------------------------------------------------------------
 *
 * lsyscache.h
 *	  Convenience routines for common queries in the system catalog cache.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: lsyscache.h,v 1.32 2001/05/09 00:35:09 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef LSYSCACHE_H
#define LSYSCACHE_H

#include "access/htup.h"

extern bool op_class(Oid opno, Oid opclass, Oid amopid);
extern char *get_attname(Oid relid, AttrNumber attnum);
extern AttrNumber get_attnum(Oid relid, char *attname);
extern Oid	get_atttype(Oid relid, AttrNumber attnum);
extern bool get_attisset(Oid relid, char *attname);
extern int32 get_atttypmod(Oid relid, AttrNumber attnum);
extern RegProcedure get_opcode(Oid opno);
extern char *get_opname(Oid opno);
extern bool op_mergejoinable(Oid opno, Oid ltype, Oid rtype,
				 Oid *leftOp, Oid *rightOp);
extern Oid	op_hashjoinable(Oid opno, Oid ltype, Oid rtype);
extern bool op_iscachable(Oid opno);
extern Oid	get_commutator(Oid opno);
extern Oid	get_negator(Oid opno);
extern RegProcedure get_oprrest(Oid opno);
extern RegProcedure get_oprjoin(Oid opno);
extern Oid	get_func_rettype(Oid funcid);
extern bool func_iscachable(Oid funcid);
extern char *get_rel_name(Oid relid);
extern int16 get_typlen(Oid typid);
extern bool get_typbyval(Oid typid);
extern void get_typlenbyval(Oid typid, int16 *typlen, bool *typbyval);
extern char get_typstorage(Oid typid);
extern Datum get_typdefault(Oid typid);
extern int32 get_typavgwidth(Oid typid, int32 typmod);
extern int32 get_attavgwidth(Oid relid, AttrNumber attnum);
extern bool get_attstatsslot(HeapTuple statstuple,
							 Oid atttype, int32 atttypmod,
							 int reqkind, Oid reqop,
							 Datum **values, int *nvalues,
							 float4 **numbers, int *nnumbers);
extern void free_attstatsslot(Oid atttype,
							  Datum *values, int nvalues,
							  float4 *numbers, int nnumbers);

#define TypeIsToastable(typid)	(get_typstorage(typid) != 'p')

#endif	 /* LSYSCACHE_H */
