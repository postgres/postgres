/*-------------------------------------------------------------------------
 *
 * catalog_utils.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: catalog_utils.h,v 1.13 1997/09/08 21:53:35 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CATALOG_UTILS_H
#define CATALOG_UTILS_H

#include <catalog/pg_type.h>
#include <access/htup.h>

typedef HeapTuple Type;
typedef HeapTuple Operator;

extern Type get_id_type(Oid id);
extern char *get_id_typname(Oid id);
extern Type type(char *);
extern Oid	att_typeid(Relation rd, int attid);
extern int	att_attnelems(Relation rd, int attid);
extern Oid	typeid(Type tp);
extern int16 tlen(Type t);
extern bool tbyval(Type t);
extern char *tname(Type t);
extern int	tbyvalue(Type t);
extern Oid	oprid(Operator op);
extern Operator oper(char *op, Oid arg1, Oid arg2, bool noWarnings);
extern Operator right_oper(char *op, Oid arg);
extern Operator left_oper(char *op, Oid arg);
extern int	varattno(Relation rd, char *a);
extern bool varisset(Relation rd, char *name);
extern int	nf_varattno(Relation rd, char *a);
extern char *getAttrName(Relation rd, int attrno);
extern char *instr2(Type tp, char *string, int typlen);
extern Oid	GetArrayElementType(Oid typearray);
extern Oid	funcid_get_rettype(Oid funcid);
extern bool
func_get_detail(char *funcname, int nargs, Oid *oid_array,
			Oid *funcid, Oid *rettype, bool *retset, Oid **true_typeids);
extern Oid	typeid_get_retinfunc(Oid type_id);
extern Oid	typeid_get_retoutfunc(Oid type_id);
extern Oid	typeid_get_relid(Oid type_id);
extern Oid	get_typrelid(Type typ);
extern Oid	get_typelem(Oid type_id);
extern void func_error(char *caller, char *funcname, int nargs, Oid *argtypes);
extern void agg_error(char *caller, char *aggname, Oid basetypeID);

#endif							/* CATALOG_UTILS_H */
