/*-------------------------------------------------------------------------
 *
 * lsyscache.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: lsyscache.h,v 1.6 1997/09/08 21:55:11 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef LSYSCACHE_H
#define LSYSCACHE_H

#include <access/attnum.h>
#include <access/htup.h>

extern bool op_class(Oid opid, int32 opclass, Oid amopid);
extern char *get_attname(Oid relid, AttrNumber attnum);
extern AttrNumber get_attnum(Oid relid, char *attname);
extern Oid	get_atttype(Oid relid, AttrNumber attnum);
extern bool get_attisset(Oid relid, char *attname);
extern RegProcedure get_opcode(Oid opid);
extern char *get_opname(Oid opid);
extern bool
op_mergesortable(Oid opid, Oid ltype, Oid rtype,
				 Oid *leftOp, Oid *rightOp);
extern Oid	op_hashjoinable(Oid opid, Oid ltype, Oid rtype);
extern Oid	get_commutator(Oid opid);
extern HeapTuple get_operator_tuple(Oid opno);
extern Oid	get_negator(Oid opid);
extern RegProcedure get_oprrest(Oid opid);
extern RegProcedure get_oprjoin(Oid opid);
extern int	get_relnatts(Oid relid);
extern char *get_rel_name(Oid relid);
extern struct varlena *get_relstub(Oid relid, int no, bool *islast);
extern Oid	get_ruleid(char *rulename);
extern Oid	get_eventrelid(Oid ruleid);
extern int16 get_typlen(Oid typid);
extern bool get_typbyval(Oid typid);
extern struct varlena *get_typdefault(Oid typid);

#endif							/* LSYSCACHE_H */
