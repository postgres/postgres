/*-------------------------------------------------------------------------
 *
 * catalog_utils.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: catalog_utils.h,v 1.1 1996/08/28 07:23:51 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	CATALOG_UTILS_H
#define	CATALOG_UTILS_H


#include "postgres.h"

#include "access/htup.h"
#include "utils/rel.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "utils/syscache.h"
    
typedef HeapTuple	Type;
typedef HeapTuple	Operator;

extern bool check_typeid(long id);
extern Type get_id_type(long id);
extern char *get_id_typname(long id);
extern Type type(char *);
extern Oid att_typeid(Relation rd, int attid);
extern int att_attnelems(Relation rd, int attid);
extern Oid typeid(Type tp);
extern int16 tlen(Type t);
extern bool tbyval(Type t);
extern char *tname(Type t);
extern int tbyvalue(Type t);
extern Oid oprid(Operator op);
extern Operator oper(char *op, int arg1, int arg2);
extern Operator right_oper(char *op, int arg);
extern Operator left_oper(char *op, int arg);
extern int varattno(Relation rd, char *a);
extern bool varisset(Relation rd, char *name);
extern int nf_varattno(Relation rd, char *a);
extern char *getAttrName(Relation rd, int attrno);
extern char *outstr(char *typename, char *value);
extern char *instr2(Type tp, char *string, int typlen);
extern char *instr1(TypeTupleForm tp, char *string, int typlen);
extern Oid GetArrayElementType(Oid typearray);
extern Oid funcid_get_rettype(Oid funcid);
extern bool func_get_detail(char *funcname, int nargs, Oid *oid_array,
	    Oid *funcid, Oid *rettype, bool *retset, Oid **true_typeids);
extern Oid typeid_get_retinfunc(int type_id);
extern Oid typeid_get_relid(int type_id);
extern Oid get_typrelid(Type typ);
extern Oid get_typelem(Oid type_id);
extern char FindDelimiter(char *typename);
extern void op_error(char *op, int arg1, int arg2);
extern void func_error(char *caller, char *funcname, int nargs, int *argtypes);

#endif	/* CATALOG_UTILS_H */
