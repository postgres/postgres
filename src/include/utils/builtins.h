/*-------------------------------------------------------------------------
 *
 * builtins.h
 *	  Declarations for operations on built-in types.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: builtins.h,v 1.228.2.1 2003/12/02 12:40:08 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUILTINS_H
#define BUILTINS_H

#include "fmgr.h"
#include "nodes/parsenodes.h"
#include "storage/itemptr.h"	/* for setLastTid() */

/*
 *		Defined in adt/
 */

/* acl.c */
extern Datum has_table_privilege_name_name(PG_FUNCTION_ARGS);
extern Datum has_table_privilege_name_id(PG_FUNCTION_ARGS);
extern Datum has_table_privilege_id_name(PG_FUNCTION_ARGS);
extern Datum has_table_privilege_id_id(PG_FUNCTION_ARGS);
extern Datum has_table_privilege_name(PG_FUNCTION_ARGS);
extern Datum has_table_privilege_id(PG_FUNCTION_ARGS);
extern Datum has_database_privilege_name_name(PG_FUNCTION_ARGS);
extern Datum has_database_privilege_name_id(PG_FUNCTION_ARGS);
extern Datum has_database_privilege_id_name(PG_FUNCTION_ARGS);
extern Datum has_database_privilege_id_id(PG_FUNCTION_ARGS);
extern Datum has_database_privilege_name(PG_FUNCTION_ARGS);
extern Datum has_database_privilege_id(PG_FUNCTION_ARGS);
extern Datum has_function_privilege_name_name(PG_FUNCTION_ARGS);
extern Datum has_function_privilege_name_id(PG_FUNCTION_ARGS);
extern Datum has_function_privilege_id_name(PG_FUNCTION_ARGS);
extern Datum has_function_privilege_id_id(PG_FUNCTION_ARGS);
extern Datum has_function_privilege_name(PG_FUNCTION_ARGS);
extern Datum has_function_privilege_id(PG_FUNCTION_ARGS);
extern Datum has_language_privilege_name_name(PG_FUNCTION_ARGS);
extern Datum has_language_privilege_name_id(PG_FUNCTION_ARGS);
extern Datum has_language_privilege_id_name(PG_FUNCTION_ARGS);
extern Datum has_language_privilege_id_id(PG_FUNCTION_ARGS);
extern Datum has_language_privilege_name(PG_FUNCTION_ARGS);
extern Datum has_language_privilege_id(PG_FUNCTION_ARGS);
extern Datum has_schema_privilege_name_name(PG_FUNCTION_ARGS);
extern Datum has_schema_privilege_name_id(PG_FUNCTION_ARGS);
extern Datum has_schema_privilege_id_name(PG_FUNCTION_ARGS);
extern Datum has_schema_privilege_id_id(PG_FUNCTION_ARGS);
extern Datum has_schema_privilege_name(PG_FUNCTION_ARGS);
extern Datum has_schema_privilege_id(PG_FUNCTION_ARGS);

/* bool.c */
extern Datum boolin(PG_FUNCTION_ARGS);
extern Datum boolout(PG_FUNCTION_ARGS);
extern Datum boolrecv(PG_FUNCTION_ARGS);
extern Datum boolsend(PG_FUNCTION_ARGS);
extern Datum booleq(PG_FUNCTION_ARGS);
extern Datum boolne(PG_FUNCTION_ARGS);
extern Datum boollt(PG_FUNCTION_ARGS);
extern Datum boolgt(PG_FUNCTION_ARGS);
extern Datum boolle(PG_FUNCTION_ARGS);
extern Datum boolge(PG_FUNCTION_ARGS);
extern Datum istrue(PG_FUNCTION_ARGS);
extern Datum isfalse(PG_FUNCTION_ARGS);
extern Datum isnottrue(PG_FUNCTION_ARGS);
extern Datum isnotfalse(PG_FUNCTION_ARGS);

/* char.c */
extern Datum charin(PG_FUNCTION_ARGS);
extern Datum charout(PG_FUNCTION_ARGS);
extern Datum charrecv(PG_FUNCTION_ARGS);
extern Datum charsend(PG_FUNCTION_ARGS);
extern Datum chareq(PG_FUNCTION_ARGS);
extern Datum charne(PG_FUNCTION_ARGS);
extern Datum charlt(PG_FUNCTION_ARGS);
extern Datum charle(PG_FUNCTION_ARGS);
extern Datum chargt(PG_FUNCTION_ARGS);
extern Datum charge(PG_FUNCTION_ARGS);
extern Datum charpl(PG_FUNCTION_ARGS);
extern Datum charmi(PG_FUNCTION_ARGS);
extern Datum charmul(PG_FUNCTION_ARGS);
extern Datum chardiv(PG_FUNCTION_ARGS);
extern Datum text_char(PG_FUNCTION_ARGS);
extern Datum char_text(PG_FUNCTION_ARGS);

/* int.c */
extern Datum int2in(PG_FUNCTION_ARGS);
extern Datum int2out(PG_FUNCTION_ARGS);
extern Datum int2recv(PG_FUNCTION_ARGS);
extern Datum int2send(PG_FUNCTION_ARGS);
extern Datum int2vectorin(PG_FUNCTION_ARGS);
extern Datum int2vectorout(PG_FUNCTION_ARGS);
extern Datum int2vectorrecv(PG_FUNCTION_ARGS);
extern Datum int2vectorsend(PG_FUNCTION_ARGS);
extern Datum int2vectoreq(PG_FUNCTION_ARGS);
extern Datum int4in(PG_FUNCTION_ARGS);
extern Datum int4out(PG_FUNCTION_ARGS);
extern Datum int4recv(PG_FUNCTION_ARGS);
extern Datum int4send(PG_FUNCTION_ARGS);
extern Datum i2toi4(PG_FUNCTION_ARGS);
extern Datum i4toi2(PG_FUNCTION_ARGS);
extern Datum int2_text(PG_FUNCTION_ARGS);
extern Datum text_int2(PG_FUNCTION_ARGS);
extern Datum int4_text(PG_FUNCTION_ARGS);
extern Datum text_int4(PG_FUNCTION_ARGS);
extern Datum int4eq(PG_FUNCTION_ARGS);
extern Datum int4ne(PG_FUNCTION_ARGS);
extern Datum int4lt(PG_FUNCTION_ARGS);
extern Datum int4le(PG_FUNCTION_ARGS);
extern Datum int4gt(PG_FUNCTION_ARGS);
extern Datum int4ge(PG_FUNCTION_ARGS);
extern Datum int2eq(PG_FUNCTION_ARGS);
extern Datum int2ne(PG_FUNCTION_ARGS);
extern Datum int2lt(PG_FUNCTION_ARGS);
extern Datum int2le(PG_FUNCTION_ARGS);
extern Datum int2gt(PG_FUNCTION_ARGS);
extern Datum int2ge(PG_FUNCTION_ARGS);
extern Datum int24eq(PG_FUNCTION_ARGS);
extern Datum int24ne(PG_FUNCTION_ARGS);
extern Datum int24lt(PG_FUNCTION_ARGS);
extern Datum int24le(PG_FUNCTION_ARGS);
extern Datum int24gt(PG_FUNCTION_ARGS);
extern Datum int24ge(PG_FUNCTION_ARGS);
extern Datum int42eq(PG_FUNCTION_ARGS);
extern Datum int42ne(PG_FUNCTION_ARGS);
extern Datum int42lt(PG_FUNCTION_ARGS);
extern Datum int42le(PG_FUNCTION_ARGS);
extern Datum int42gt(PG_FUNCTION_ARGS);
extern Datum int42ge(PG_FUNCTION_ARGS);
extern Datum int4um(PG_FUNCTION_ARGS);
extern Datum int4up(PG_FUNCTION_ARGS);
extern Datum int4pl(PG_FUNCTION_ARGS);
extern Datum int4mi(PG_FUNCTION_ARGS);
extern Datum int4mul(PG_FUNCTION_ARGS);
extern Datum int4div(PG_FUNCTION_ARGS);
extern Datum int4abs(PG_FUNCTION_ARGS);
extern Datum int4inc(PG_FUNCTION_ARGS);
extern Datum int2um(PG_FUNCTION_ARGS);
extern Datum int2up(PG_FUNCTION_ARGS);
extern Datum int2pl(PG_FUNCTION_ARGS);
extern Datum int2mi(PG_FUNCTION_ARGS);
extern Datum int2mul(PG_FUNCTION_ARGS);
extern Datum int2div(PG_FUNCTION_ARGS);
extern Datum int2abs(PG_FUNCTION_ARGS);
extern Datum int24pl(PG_FUNCTION_ARGS);
extern Datum int24mi(PG_FUNCTION_ARGS);
extern Datum int24mul(PG_FUNCTION_ARGS);
extern Datum int24div(PG_FUNCTION_ARGS);
extern Datum int42pl(PG_FUNCTION_ARGS);
extern Datum int42mi(PG_FUNCTION_ARGS);
extern Datum int42mul(PG_FUNCTION_ARGS);
extern Datum int42div(PG_FUNCTION_ARGS);
extern Datum int4mod(PG_FUNCTION_ARGS);
extern Datum int2mod(PG_FUNCTION_ARGS);
extern Datum int24mod(PG_FUNCTION_ARGS);
extern Datum int42mod(PG_FUNCTION_ARGS);
extern Datum int4fac(PG_FUNCTION_ARGS);
extern Datum int2fac(PG_FUNCTION_ARGS);
extern Datum int2larger(PG_FUNCTION_ARGS);
extern Datum int2smaller(PG_FUNCTION_ARGS);
extern Datum int4larger(PG_FUNCTION_ARGS);
extern Datum int4smaller(PG_FUNCTION_ARGS);

extern Datum int4and(PG_FUNCTION_ARGS);
extern Datum int4or(PG_FUNCTION_ARGS);
extern Datum int4xor(PG_FUNCTION_ARGS);
extern Datum int4not(PG_FUNCTION_ARGS);
extern Datum int4shl(PG_FUNCTION_ARGS);
extern Datum int4shr(PG_FUNCTION_ARGS);
extern Datum int2and(PG_FUNCTION_ARGS);
extern Datum int2or(PG_FUNCTION_ARGS);
extern Datum int2xor(PG_FUNCTION_ARGS);
extern Datum int2not(PG_FUNCTION_ARGS);
extern Datum int2shl(PG_FUNCTION_ARGS);
extern Datum int2shr(PG_FUNCTION_ARGS);

/* name.c */
extern Datum namein(PG_FUNCTION_ARGS);
extern Datum nameout(PG_FUNCTION_ARGS);
extern Datum namerecv(PG_FUNCTION_ARGS);
extern Datum namesend(PG_FUNCTION_ARGS);
extern Datum nameeq(PG_FUNCTION_ARGS);
extern Datum namene(PG_FUNCTION_ARGS);
extern Datum namelt(PG_FUNCTION_ARGS);
extern Datum namele(PG_FUNCTION_ARGS);
extern Datum namegt(PG_FUNCTION_ARGS);
extern Datum namege(PG_FUNCTION_ARGS);
extern Datum name_pattern_eq(PG_FUNCTION_ARGS);
extern Datum name_pattern_ne(PG_FUNCTION_ARGS);
extern Datum name_pattern_lt(PG_FUNCTION_ARGS);
extern Datum name_pattern_le(PG_FUNCTION_ARGS);
extern Datum name_pattern_gt(PG_FUNCTION_ARGS);
extern Datum name_pattern_ge(PG_FUNCTION_ARGS);
extern int	namecpy(Name n1, Name n2);
extern int	namestrcpy(Name name, const char *str);
extern int	namestrcmp(Name name, const char *str);
extern Datum current_user(PG_FUNCTION_ARGS);
extern Datum session_user(PG_FUNCTION_ARGS);
extern Datum current_schema(PG_FUNCTION_ARGS);
extern Datum current_schemas(PG_FUNCTION_ARGS);

/* numutils.c */
extern int32 pg_atoi(char *s, int size, int c);
extern void pg_itoa(int16 i, char *a);
extern void pg_ltoa(int32 l, char *a);

/*
 *		Per-opclass comparison functions for new btrees.  These are
 *		stored in pg_amproc and defined in access/nbtree/nbtcompare.c
 */
extern Datum btboolcmp(PG_FUNCTION_ARGS);
extern Datum btint2cmp(PG_FUNCTION_ARGS);
extern Datum btint4cmp(PG_FUNCTION_ARGS);
extern Datum btint8cmp(PG_FUNCTION_ARGS);
extern Datum btfloat4cmp(PG_FUNCTION_ARGS);
extern Datum btfloat8cmp(PG_FUNCTION_ARGS);
extern Datum btoidcmp(PG_FUNCTION_ARGS);
extern Datum btoidvectorcmp(PG_FUNCTION_ARGS);
extern Datum btabstimecmp(PG_FUNCTION_ARGS);
extern Datum btreltimecmp(PG_FUNCTION_ARGS);
extern Datum bttintervalcmp(PG_FUNCTION_ARGS);
extern Datum btcharcmp(PG_FUNCTION_ARGS);
extern Datum btnamecmp(PG_FUNCTION_ARGS);
extern Datum bttextcmp(PG_FUNCTION_ARGS);
extern Datum btname_pattern_cmp(PG_FUNCTION_ARGS);
extern Datum bttext_pattern_cmp(PG_FUNCTION_ARGS);

/* float.c */
extern DLLIMPORT int	extra_float_digits;

extern Datum float4in(PG_FUNCTION_ARGS);
extern Datum float4out(PG_FUNCTION_ARGS);
extern Datum float4recv(PG_FUNCTION_ARGS);
extern Datum float4send(PG_FUNCTION_ARGS);
extern Datum float8in(PG_FUNCTION_ARGS);
extern Datum float8out(PG_FUNCTION_ARGS);
extern Datum float8recv(PG_FUNCTION_ARGS);
extern Datum float8send(PG_FUNCTION_ARGS);
extern Datum float4abs(PG_FUNCTION_ARGS);
extern Datum float4um(PG_FUNCTION_ARGS);
extern Datum float4up(PG_FUNCTION_ARGS);
extern Datum float4larger(PG_FUNCTION_ARGS);
extern Datum float4smaller(PG_FUNCTION_ARGS);
extern Datum float8abs(PG_FUNCTION_ARGS);
extern Datum float8um(PG_FUNCTION_ARGS);
extern Datum float8up(PG_FUNCTION_ARGS);
extern Datum float8larger(PG_FUNCTION_ARGS);
extern Datum float8smaller(PG_FUNCTION_ARGS);
extern Datum float4pl(PG_FUNCTION_ARGS);
extern Datum float4mi(PG_FUNCTION_ARGS);
extern Datum float4mul(PG_FUNCTION_ARGS);
extern Datum float4div(PG_FUNCTION_ARGS);
extern Datum float8pl(PG_FUNCTION_ARGS);
extern Datum float8mi(PG_FUNCTION_ARGS);
extern Datum float8mul(PG_FUNCTION_ARGS);
extern Datum float8div(PG_FUNCTION_ARGS);
extern Datum float4eq(PG_FUNCTION_ARGS);
extern Datum float4ne(PG_FUNCTION_ARGS);
extern Datum float4lt(PG_FUNCTION_ARGS);
extern Datum float4le(PG_FUNCTION_ARGS);
extern Datum float4gt(PG_FUNCTION_ARGS);
extern Datum float4ge(PG_FUNCTION_ARGS);
extern Datum float8eq(PG_FUNCTION_ARGS);
extern Datum float8ne(PG_FUNCTION_ARGS);
extern Datum float8lt(PG_FUNCTION_ARGS);
extern Datum float8le(PG_FUNCTION_ARGS);
extern Datum float8gt(PG_FUNCTION_ARGS);
extern Datum float8ge(PG_FUNCTION_ARGS);
extern Datum ftod(PG_FUNCTION_ARGS);
extern Datum i4tod(PG_FUNCTION_ARGS);
extern Datum i2tod(PG_FUNCTION_ARGS);
extern Datum dtof(PG_FUNCTION_ARGS);
extern Datum dtoi4(PG_FUNCTION_ARGS);
extern Datum dtoi2(PG_FUNCTION_ARGS);
extern Datum i4tof(PG_FUNCTION_ARGS);
extern Datum i2tof(PG_FUNCTION_ARGS);
extern Datum ftoi4(PG_FUNCTION_ARGS);
extern Datum ftoi2(PG_FUNCTION_ARGS);
extern Datum text_float8(PG_FUNCTION_ARGS);
extern Datum text_float4(PG_FUNCTION_ARGS);
extern Datum float8_text(PG_FUNCTION_ARGS);
extern Datum float4_text(PG_FUNCTION_ARGS);
extern Datum dround(PG_FUNCTION_ARGS);
extern Datum dceil(PG_FUNCTION_ARGS);
extern Datum dfloor(PG_FUNCTION_ARGS);
extern Datum dsign(PG_FUNCTION_ARGS);
extern Datum dtrunc(PG_FUNCTION_ARGS);
extern Datum dsqrt(PG_FUNCTION_ARGS);
extern Datum dcbrt(PG_FUNCTION_ARGS);
extern Datum dpow(PG_FUNCTION_ARGS);
extern Datum dexp(PG_FUNCTION_ARGS);
extern Datum dlog1(PG_FUNCTION_ARGS);
extern Datum dlog10(PG_FUNCTION_ARGS);
extern Datum dacos(PG_FUNCTION_ARGS);
extern Datum dasin(PG_FUNCTION_ARGS);
extern Datum datan(PG_FUNCTION_ARGS);
extern Datum datan2(PG_FUNCTION_ARGS);
extern Datum dcos(PG_FUNCTION_ARGS);
extern Datum dcot(PG_FUNCTION_ARGS);
extern Datum dsin(PG_FUNCTION_ARGS);
extern Datum dtan(PG_FUNCTION_ARGS);
extern Datum degrees(PG_FUNCTION_ARGS);
extern Datum dpi(PG_FUNCTION_ARGS);
extern Datum radians(PG_FUNCTION_ARGS);
extern Datum drandom(PG_FUNCTION_ARGS);
extern Datum setseed(PG_FUNCTION_ARGS);
extern Datum float8_accum(PG_FUNCTION_ARGS);
extern Datum float4_accum(PG_FUNCTION_ARGS);
extern Datum float8_avg(PG_FUNCTION_ARGS);
extern Datum float8_variance(PG_FUNCTION_ARGS);
extern Datum float8_stddev(PG_FUNCTION_ARGS);
extern Datum float48pl(PG_FUNCTION_ARGS);
extern Datum float48mi(PG_FUNCTION_ARGS);
extern Datum float48mul(PG_FUNCTION_ARGS);
extern Datum float48div(PG_FUNCTION_ARGS);
extern Datum float84pl(PG_FUNCTION_ARGS);
extern Datum float84mi(PG_FUNCTION_ARGS);
extern Datum float84mul(PG_FUNCTION_ARGS);
extern Datum float84div(PG_FUNCTION_ARGS);
extern Datum float48eq(PG_FUNCTION_ARGS);
extern Datum float48ne(PG_FUNCTION_ARGS);
extern Datum float48lt(PG_FUNCTION_ARGS);
extern Datum float48le(PG_FUNCTION_ARGS);
extern Datum float48gt(PG_FUNCTION_ARGS);
extern Datum float48ge(PG_FUNCTION_ARGS);
extern Datum float84eq(PG_FUNCTION_ARGS);
extern Datum float84ne(PG_FUNCTION_ARGS);
extern Datum float84lt(PG_FUNCTION_ARGS);
extern Datum float84le(PG_FUNCTION_ARGS);
extern Datum float84gt(PG_FUNCTION_ARGS);
extern Datum float84ge(PG_FUNCTION_ARGS);

/* misc.c */
extern Datum nullvalue(PG_FUNCTION_ARGS);
extern Datum nonnullvalue(PG_FUNCTION_ARGS);
extern Datum current_database(PG_FUNCTION_ARGS);

/* not_in.c */
extern Datum int4notin(PG_FUNCTION_ARGS);
extern Datum oidnotin(PG_FUNCTION_ARGS);

/* oid.c */
extern Datum oidin(PG_FUNCTION_ARGS);
extern Datum oidout(PG_FUNCTION_ARGS);
extern Datum oidrecv(PG_FUNCTION_ARGS);
extern Datum oidsend(PG_FUNCTION_ARGS);
extern Datum oideq(PG_FUNCTION_ARGS);
extern Datum oidne(PG_FUNCTION_ARGS);
extern Datum oidlt(PG_FUNCTION_ARGS);
extern Datum oidle(PG_FUNCTION_ARGS);
extern Datum oidge(PG_FUNCTION_ARGS);
extern Datum oidgt(PG_FUNCTION_ARGS);
extern Datum oidlarger(PG_FUNCTION_ARGS);
extern Datum oidsmaller(PG_FUNCTION_ARGS);
extern Datum oid_text(PG_FUNCTION_ARGS);
extern Datum text_oid(PG_FUNCTION_ARGS);
extern Datum oidvectorin(PG_FUNCTION_ARGS);
extern Datum oidvectorout(PG_FUNCTION_ARGS);
extern Datum oidvectorrecv(PG_FUNCTION_ARGS);
extern Datum oidvectorsend(PG_FUNCTION_ARGS);
extern Datum oidvectoreq(PG_FUNCTION_ARGS);
extern Datum oidvectorne(PG_FUNCTION_ARGS);
extern Datum oidvectorlt(PG_FUNCTION_ARGS);
extern Datum oidvectorle(PG_FUNCTION_ARGS);
extern Datum oidvectorge(PG_FUNCTION_ARGS);
extern Datum oidvectorgt(PG_FUNCTION_ARGS);

/* pseudotypes.c */
extern Datum record_in(PG_FUNCTION_ARGS);
extern Datum record_out(PG_FUNCTION_ARGS);
extern Datum record_recv(PG_FUNCTION_ARGS);
extern Datum record_send(PG_FUNCTION_ARGS);
extern Datum cstring_in(PG_FUNCTION_ARGS);
extern Datum cstring_out(PG_FUNCTION_ARGS);
extern Datum cstring_recv(PG_FUNCTION_ARGS);
extern Datum cstring_send(PG_FUNCTION_ARGS);
extern Datum any_in(PG_FUNCTION_ARGS);
extern Datum any_out(PG_FUNCTION_ARGS);
extern Datum anyarray_in(PG_FUNCTION_ARGS);
extern Datum anyarray_out(PG_FUNCTION_ARGS);
extern Datum anyarray_recv(PG_FUNCTION_ARGS);
extern Datum anyarray_send(PG_FUNCTION_ARGS);
extern Datum void_in(PG_FUNCTION_ARGS);
extern Datum void_out(PG_FUNCTION_ARGS);
extern Datum trigger_in(PG_FUNCTION_ARGS);
extern Datum trigger_out(PG_FUNCTION_ARGS);
extern Datum language_handler_in(PG_FUNCTION_ARGS);
extern Datum language_handler_out(PG_FUNCTION_ARGS);
extern Datum internal_in(PG_FUNCTION_ARGS);
extern Datum internal_out(PG_FUNCTION_ARGS);
extern Datum opaque_in(PG_FUNCTION_ARGS);
extern Datum opaque_out(PG_FUNCTION_ARGS);
extern Datum anyelement_in(PG_FUNCTION_ARGS);
extern Datum anyelement_out(PG_FUNCTION_ARGS);

/* regexp.c */
extern Datum nameregexeq(PG_FUNCTION_ARGS);
extern Datum nameregexne(PG_FUNCTION_ARGS);
extern Datum textregexeq(PG_FUNCTION_ARGS);
extern Datum textregexne(PG_FUNCTION_ARGS);
extern Datum nameicregexeq(PG_FUNCTION_ARGS);
extern Datum nameicregexne(PG_FUNCTION_ARGS);
extern Datum texticregexeq(PG_FUNCTION_ARGS);
extern Datum texticregexne(PG_FUNCTION_ARGS);
extern Datum textregexsubstr(PG_FUNCTION_ARGS);
extern Datum similar_escape(PG_FUNCTION_ARGS);
extern const char *assign_regex_flavor(const char *value,
					bool doit, bool interactive);

/* regproc.c */
extern Datum regprocin(PG_FUNCTION_ARGS);
extern Datum regprocout(PG_FUNCTION_ARGS);
extern Datum regprocrecv(PG_FUNCTION_ARGS);
extern Datum regprocsend(PG_FUNCTION_ARGS);
extern Datum regprocedurein(PG_FUNCTION_ARGS);
extern Datum regprocedureout(PG_FUNCTION_ARGS);
extern Datum regprocedurerecv(PG_FUNCTION_ARGS);
extern Datum regproceduresend(PG_FUNCTION_ARGS);
extern Datum regoperin(PG_FUNCTION_ARGS);
extern Datum regoperout(PG_FUNCTION_ARGS);
extern Datum regoperrecv(PG_FUNCTION_ARGS);
extern Datum regopersend(PG_FUNCTION_ARGS);
extern Datum regoperatorin(PG_FUNCTION_ARGS);
extern Datum regoperatorout(PG_FUNCTION_ARGS);
extern Datum regoperatorrecv(PG_FUNCTION_ARGS);
extern Datum regoperatorsend(PG_FUNCTION_ARGS);
extern Datum regclassin(PG_FUNCTION_ARGS);
extern Datum regclassout(PG_FUNCTION_ARGS);
extern Datum regclassrecv(PG_FUNCTION_ARGS);
extern Datum regclasssend(PG_FUNCTION_ARGS);
extern Datum regtypein(PG_FUNCTION_ARGS);
extern Datum regtypeout(PG_FUNCTION_ARGS);
extern Datum regtyperecv(PG_FUNCTION_ARGS);
extern Datum regtypesend(PG_FUNCTION_ARGS);
extern List *stringToQualifiedNameList(const char *string, const char *caller);
extern char *format_procedure(Oid procedure_oid);
extern char *format_operator(Oid operator_oid);

/* ruleutils.c */
extern Datum pg_get_ruledef(PG_FUNCTION_ARGS);
extern Datum pg_get_ruledef_ext(PG_FUNCTION_ARGS);
extern Datum pg_get_viewdef(PG_FUNCTION_ARGS);
extern Datum pg_get_viewdef_ext(PG_FUNCTION_ARGS);
extern Datum pg_get_viewdef_name(PG_FUNCTION_ARGS);
extern Datum pg_get_viewdef_name_ext(PG_FUNCTION_ARGS);
extern Datum pg_get_indexdef(PG_FUNCTION_ARGS);
extern Datum pg_get_indexdef_ext(PG_FUNCTION_ARGS);
extern Datum pg_get_triggerdef(PG_FUNCTION_ARGS);
extern Datum pg_get_constraintdef(PG_FUNCTION_ARGS);
extern Datum pg_get_constraintdef_ext(PG_FUNCTION_ARGS);
extern Datum pg_get_userbyid(PG_FUNCTION_ARGS);
extern Datum pg_get_expr(PG_FUNCTION_ARGS);
extern Datum pg_get_expr_ext(PG_FUNCTION_ARGS);
extern char *deparse_expression(Node *expr, List *dpcontext,
				   bool forceprefix, bool showimplicit);
extern List *deparse_context_for(const char *aliasname, Oid relid);
extern List *deparse_context_for_plan(int outer_varno, Node *outercontext,
						 int inner_varno, Node *innercontext,
						 List *rtable);
extern Node *deparse_context_for_rte(RangeTblEntry *rte);
extern Node *deparse_context_for_subplan(const char *name, List *tlist,
							List *rtable);
extern const char *quote_identifier(const char *ident);
extern char *quote_qualified_identifier(const char *namespace,
						   const char *ident);

/* tid.c */
extern Datum tidin(PG_FUNCTION_ARGS);
extern Datum tidout(PG_FUNCTION_ARGS);
extern Datum tidrecv(PG_FUNCTION_ARGS);
extern Datum tidsend(PG_FUNCTION_ARGS);
extern Datum tideq(PG_FUNCTION_ARGS);
extern Datum currtid_byreloid(PG_FUNCTION_ARGS);
extern Datum currtid_byrelname(PG_FUNCTION_ARGS);
extern void setLastTid(const ItemPointer tid);

/* varchar.c */
extern Datum bpcharin(PG_FUNCTION_ARGS);
extern Datum bpcharout(PG_FUNCTION_ARGS);
extern Datum bpcharrecv(PG_FUNCTION_ARGS);
extern Datum bpcharsend(PG_FUNCTION_ARGS);
extern Datum bpchar(PG_FUNCTION_ARGS);
extern Datum char_bpchar(PG_FUNCTION_ARGS);
extern Datum name_bpchar(PG_FUNCTION_ARGS);
extern Datum bpchar_name(PG_FUNCTION_ARGS);
extern Datum bpchareq(PG_FUNCTION_ARGS);
extern Datum bpcharne(PG_FUNCTION_ARGS);
extern Datum bpcharlt(PG_FUNCTION_ARGS);
extern Datum bpcharle(PG_FUNCTION_ARGS);
extern Datum bpchargt(PG_FUNCTION_ARGS);
extern Datum bpcharge(PG_FUNCTION_ARGS);
extern Datum bpcharcmp(PG_FUNCTION_ARGS);
extern Datum bpcharlen(PG_FUNCTION_ARGS);
extern Datum bpcharoctetlen(PG_FUNCTION_ARGS);
extern Datum hashbpchar(PG_FUNCTION_ARGS);

extern Datum varcharin(PG_FUNCTION_ARGS);
extern Datum varcharout(PG_FUNCTION_ARGS);
extern Datum varcharrecv(PG_FUNCTION_ARGS);
extern Datum varcharsend(PG_FUNCTION_ARGS);
extern Datum varchar(PG_FUNCTION_ARGS);

/* varlena.c */
extern Datum textin(PG_FUNCTION_ARGS);
extern Datum textout(PG_FUNCTION_ARGS);
extern Datum textrecv(PG_FUNCTION_ARGS);
extern Datum textsend(PG_FUNCTION_ARGS);
extern Datum textcat(PG_FUNCTION_ARGS);
extern Datum texteq(PG_FUNCTION_ARGS);
extern Datum textne(PG_FUNCTION_ARGS);
extern Datum text_lt(PG_FUNCTION_ARGS);
extern Datum text_le(PG_FUNCTION_ARGS);
extern Datum text_gt(PG_FUNCTION_ARGS);
extern Datum text_ge(PG_FUNCTION_ARGS);
extern Datum text_larger(PG_FUNCTION_ARGS);
extern Datum text_smaller(PG_FUNCTION_ARGS);
extern Datum text_pattern_eq(PG_FUNCTION_ARGS);
extern Datum text_pattern_ne(PG_FUNCTION_ARGS);
extern Datum text_pattern_lt(PG_FUNCTION_ARGS);
extern Datum text_pattern_le(PG_FUNCTION_ARGS);
extern Datum text_pattern_gt(PG_FUNCTION_ARGS);
extern Datum text_pattern_ge(PG_FUNCTION_ARGS);
extern Datum textlen(PG_FUNCTION_ARGS);
extern Datum textoctetlen(PG_FUNCTION_ARGS);
extern Datum textpos(PG_FUNCTION_ARGS);
extern Datum text_substr(PG_FUNCTION_ARGS);
extern Datum text_substr_no_len(PG_FUNCTION_ARGS);
extern Datum name_text(PG_FUNCTION_ARGS);
extern Datum text_name(PG_FUNCTION_ARGS);
extern int	varstr_cmp(char *arg1, int len1, char *arg2, int len2);
extern List *textToQualifiedNameList(text *textval, const char *caller);
extern bool SplitIdentifierString(char *rawstring, char separator,
					  List **namelist);
extern Datum replace_text(PG_FUNCTION_ARGS);
extern Datum split_text(PG_FUNCTION_ARGS);
extern Datum text_to_array(PG_FUNCTION_ARGS);
extern Datum array_to_text(PG_FUNCTION_ARGS);
extern Datum to_hex32(PG_FUNCTION_ARGS);
extern Datum to_hex64(PG_FUNCTION_ARGS);
extern Datum md5_text(PG_FUNCTION_ARGS);

extern Datum unknownin(PG_FUNCTION_ARGS);
extern Datum unknownout(PG_FUNCTION_ARGS);
extern Datum unknownrecv(PG_FUNCTION_ARGS);
extern Datum unknownsend(PG_FUNCTION_ARGS);

extern Datum byteain(PG_FUNCTION_ARGS);
extern Datum byteaout(PG_FUNCTION_ARGS);
extern Datum bytearecv(PG_FUNCTION_ARGS);
extern Datum byteasend(PG_FUNCTION_ARGS);
extern Datum byteaoctetlen(PG_FUNCTION_ARGS);
extern Datum byteaGetByte(PG_FUNCTION_ARGS);
extern Datum byteaGetBit(PG_FUNCTION_ARGS);
extern Datum byteaSetByte(PG_FUNCTION_ARGS);
extern Datum byteaSetBit(PG_FUNCTION_ARGS);
extern Datum binary_encode(PG_FUNCTION_ARGS);
extern Datum binary_decode(PG_FUNCTION_ARGS);
extern Datum byteaeq(PG_FUNCTION_ARGS);
extern Datum byteane(PG_FUNCTION_ARGS);
extern Datum bytealt(PG_FUNCTION_ARGS);
extern Datum byteale(PG_FUNCTION_ARGS);
extern Datum byteagt(PG_FUNCTION_ARGS);
extern Datum byteage(PG_FUNCTION_ARGS);
extern Datum byteacmp(PG_FUNCTION_ARGS);
extern Datum byteacat(PG_FUNCTION_ARGS);
extern Datum byteapos(PG_FUNCTION_ARGS);
extern Datum bytea_substr(PG_FUNCTION_ARGS);
extern Datum bytea_substr_no_len(PG_FUNCTION_ARGS);

/* version.c */
extern Datum pgsql_version(PG_FUNCTION_ARGS);

/* xid.c */
extern Datum xidin(PG_FUNCTION_ARGS);
extern Datum xidout(PG_FUNCTION_ARGS);
extern Datum xidrecv(PG_FUNCTION_ARGS);
extern Datum xidsend(PG_FUNCTION_ARGS);
extern Datum xideq(PG_FUNCTION_ARGS);
extern Datum xid_age(PG_FUNCTION_ARGS);
extern Datum cidin(PG_FUNCTION_ARGS);
extern Datum cidout(PG_FUNCTION_ARGS);
extern Datum cidrecv(PG_FUNCTION_ARGS);
extern Datum cidsend(PG_FUNCTION_ARGS);
extern Datum cideq(PG_FUNCTION_ARGS);

/* like.c */
extern Datum namelike(PG_FUNCTION_ARGS);
extern Datum namenlike(PG_FUNCTION_ARGS);
extern Datum nameiclike(PG_FUNCTION_ARGS);
extern Datum nameicnlike(PG_FUNCTION_ARGS);
extern Datum textlike(PG_FUNCTION_ARGS);
extern Datum textnlike(PG_FUNCTION_ARGS);
extern Datum texticlike(PG_FUNCTION_ARGS);
extern Datum texticnlike(PG_FUNCTION_ARGS);
extern Datum bytealike(PG_FUNCTION_ARGS);
extern Datum byteanlike(PG_FUNCTION_ARGS);
extern Datum like_escape(PG_FUNCTION_ARGS);
extern Datum like_escape_bytea(PG_FUNCTION_ARGS);

/* oracle_compat.c */
extern Datum lower(PG_FUNCTION_ARGS);
extern Datum upper(PG_FUNCTION_ARGS);
extern Datum initcap(PG_FUNCTION_ARGS);
extern Datum lpad(PG_FUNCTION_ARGS);
extern Datum rpad(PG_FUNCTION_ARGS);
extern Datum btrim(PG_FUNCTION_ARGS);
extern Datum btrim1(PG_FUNCTION_ARGS);
extern Datum byteatrim(PG_FUNCTION_ARGS);
extern Datum ltrim(PG_FUNCTION_ARGS);
extern Datum ltrim1(PG_FUNCTION_ARGS);
extern Datum rtrim(PG_FUNCTION_ARGS);
extern Datum rtrim1(PG_FUNCTION_ARGS);
extern Datum translate(PG_FUNCTION_ARGS);
extern Datum chr (PG_FUNCTION_ARGS);
extern Datum repeat(PG_FUNCTION_ARGS);
extern Datum ascii(PG_FUNCTION_ARGS);

/* inet_net_ntop.c */
extern char *inet_net_ntop(int af, const void *src, int bits,
			  char *dst, size_t size);
extern char *inet_cidr_ntop(int af, const void *src, int bits,
			   char *dst, size_t size);

/* inet_net_pton.c */
extern int inet_net_pton(int af, const char *src,
			  void *dst, size_t size);

/* network.c */
extern Datum inet_in(PG_FUNCTION_ARGS);
extern Datum inet_out(PG_FUNCTION_ARGS);
extern Datum inet_recv(PG_FUNCTION_ARGS);
extern Datum inet_send(PG_FUNCTION_ARGS);
extern Datum cidr_in(PG_FUNCTION_ARGS);
extern Datum cidr_out(PG_FUNCTION_ARGS);
extern Datum cidr_recv(PG_FUNCTION_ARGS);
extern Datum cidr_send(PG_FUNCTION_ARGS);
extern Datum network_cmp(PG_FUNCTION_ARGS);
extern Datum network_lt(PG_FUNCTION_ARGS);
extern Datum network_le(PG_FUNCTION_ARGS);
extern Datum network_eq(PG_FUNCTION_ARGS);
extern Datum network_ge(PG_FUNCTION_ARGS);
extern Datum network_gt(PG_FUNCTION_ARGS);
extern Datum network_ne(PG_FUNCTION_ARGS);
extern Datum network_sub(PG_FUNCTION_ARGS);
extern Datum network_subeq(PG_FUNCTION_ARGS);
extern Datum network_sup(PG_FUNCTION_ARGS);
extern Datum network_supeq(PG_FUNCTION_ARGS);
extern Datum network_network(PG_FUNCTION_ARGS);
extern Datum network_netmask(PG_FUNCTION_ARGS);
extern Datum network_hostmask(PG_FUNCTION_ARGS);
extern Datum network_masklen(PG_FUNCTION_ARGS);
extern Datum network_family(PG_FUNCTION_ARGS);
extern Datum network_broadcast(PG_FUNCTION_ARGS);
extern Datum network_host(PG_FUNCTION_ARGS);
extern Datum network_show(PG_FUNCTION_ARGS);
extern Datum network_abbrev(PG_FUNCTION_ARGS);
extern double convert_network_to_scalar(Datum value, Oid typid);
extern Datum text_cidr(PG_FUNCTION_ARGS);
extern Datum text_inet(PG_FUNCTION_ARGS);
extern Datum inet_set_masklen(PG_FUNCTION_ARGS);
extern Datum network_scan_first(Datum in);
extern Datum network_scan_last(Datum in);

/* mac.c */
extern Datum macaddr_in(PG_FUNCTION_ARGS);
extern Datum macaddr_out(PG_FUNCTION_ARGS);
extern Datum macaddr_recv(PG_FUNCTION_ARGS);
extern Datum macaddr_send(PG_FUNCTION_ARGS);
extern Datum macaddr_cmp(PG_FUNCTION_ARGS);
extern Datum macaddr_lt(PG_FUNCTION_ARGS);
extern Datum macaddr_le(PG_FUNCTION_ARGS);
extern Datum macaddr_eq(PG_FUNCTION_ARGS);
extern Datum macaddr_ge(PG_FUNCTION_ARGS);
extern Datum macaddr_gt(PG_FUNCTION_ARGS);
extern Datum macaddr_ne(PG_FUNCTION_ARGS);
extern Datum macaddr_trunc(PG_FUNCTION_ARGS);
extern Datum macaddr_text(PG_FUNCTION_ARGS);
extern Datum text_macaddr(PG_FUNCTION_ARGS);
extern Datum hashmacaddr(PG_FUNCTION_ARGS);

/* numeric.c */
extern Datum numeric_in(PG_FUNCTION_ARGS);
extern Datum numeric_out(PG_FUNCTION_ARGS);
extern Datum numeric_recv(PG_FUNCTION_ARGS);
extern Datum numeric_send(PG_FUNCTION_ARGS);
extern Datum numeric(PG_FUNCTION_ARGS);
extern Datum numeric_abs(PG_FUNCTION_ARGS);
extern Datum numeric_uminus(PG_FUNCTION_ARGS);
extern Datum numeric_uplus(PG_FUNCTION_ARGS);
extern Datum numeric_sign(PG_FUNCTION_ARGS);
extern Datum numeric_round(PG_FUNCTION_ARGS);
extern Datum numeric_trunc(PG_FUNCTION_ARGS);
extern Datum numeric_ceil(PG_FUNCTION_ARGS);
extern Datum numeric_floor(PG_FUNCTION_ARGS);
extern Datum numeric_cmp(PG_FUNCTION_ARGS);
extern Datum numeric_eq(PG_FUNCTION_ARGS);
extern Datum numeric_ne(PG_FUNCTION_ARGS);
extern Datum numeric_gt(PG_FUNCTION_ARGS);
extern Datum numeric_ge(PG_FUNCTION_ARGS);
extern Datum numeric_lt(PG_FUNCTION_ARGS);
extern Datum numeric_le(PG_FUNCTION_ARGS);
extern Datum numeric_add(PG_FUNCTION_ARGS);
extern Datum numeric_sub(PG_FUNCTION_ARGS);
extern Datum numeric_mul(PG_FUNCTION_ARGS);
extern Datum numeric_div(PG_FUNCTION_ARGS);
extern Datum numeric_mod(PG_FUNCTION_ARGS);
extern Datum numeric_inc(PG_FUNCTION_ARGS);
extern Datum numeric_smaller(PG_FUNCTION_ARGS);
extern Datum numeric_larger(PG_FUNCTION_ARGS);
extern Datum numeric_sqrt(PG_FUNCTION_ARGS);
extern Datum numeric_exp(PG_FUNCTION_ARGS);
extern Datum numeric_ln(PG_FUNCTION_ARGS);
extern Datum numeric_log(PG_FUNCTION_ARGS);
extern Datum numeric_power(PG_FUNCTION_ARGS);
extern Datum int4_numeric(PG_FUNCTION_ARGS);
extern Datum numeric_int4(PG_FUNCTION_ARGS);
extern Datum int8_numeric(PG_FUNCTION_ARGS);
extern Datum numeric_int8(PG_FUNCTION_ARGS);
extern Datum int2_numeric(PG_FUNCTION_ARGS);
extern Datum numeric_int2(PG_FUNCTION_ARGS);
extern Datum float8_numeric(PG_FUNCTION_ARGS);
extern Datum numeric_float8(PG_FUNCTION_ARGS);
extern Datum numeric_float8_no_overflow(PG_FUNCTION_ARGS);
extern Datum float4_numeric(PG_FUNCTION_ARGS);
extern Datum numeric_float4(PG_FUNCTION_ARGS);
extern Datum text_numeric(PG_FUNCTION_ARGS);
extern Datum numeric_text(PG_FUNCTION_ARGS);
extern Datum numeric_accum(PG_FUNCTION_ARGS);
extern Datum int2_accum(PG_FUNCTION_ARGS);
extern Datum int4_accum(PG_FUNCTION_ARGS);
extern Datum int8_accum(PG_FUNCTION_ARGS);
extern Datum numeric_avg(PG_FUNCTION_ARGS);
extern Datum numeric_variance(PG_FUNCTION_ARGS);
extern Datum numeric_stddev(PG_FUNCTION_ARGS);
extern Datum int2_sum(PG_FUNCTION_ARGS);
extern Datum int4_sum(PG_FUNCTION_ARGS);
extern Datum int8_sum(PG_FUNCTION_ARGS);
extern Datum int2_avg_accum(PG_FUNCTION_ARGS);
extern Datum int4_avg_accum(PG_FUNCTION_ARGS);
extern Datum int8_avg(PG_FUNCTION_ARGS);

/* ri_triggers.c */
extern Datum RI_FKey_check_ins(PG_FUNCTION_ARGS);
extern Datum RI_FKey_check_upd(PG_FUNCTION_ARGS);
extern Datum RI_FKey_noaction_del(PG_FUNCTION_ARGS);
extern Datum RI_FKey_noaction_upd(PG_FUNCTION_ARGS);
extern Datum RI_FKey_cascade_del(PG_FUNCTION_ARGS);
extern Datum RI_FKey_cascade_upd(PG_FUNCTION_ARGS);
extern Datum RI_FKey_restrict_del(PG_FUNCTION_ARGS);
extern Datum RI_FKey_restrict_upd(PG_FUNCTION_ARGS);
extern Datum RI_FKey_setnull_del(PG_FUNCTION_ARGS);
extern Datum RI_FKey_setnull_upd(PG_FUNCTION_ARGS);
extern Datum RI_FKey_setdefault_del(PG_FUNCTION_ARGS);
extern Datum RI_FKey_setdefault_upd(PG_FUNCTION_ARGS);

/* encoding support functions */
extern Datum getdatabaseencoding(PG_FUNCTION_ARGS);
extern Datum database_character_set(PG_FUNCTION_ARGS);
extern Datum pg_client_encoding(PG_FUNCTION_ARGS);
extern Datum PG_encoding_to_char(PG_FUNCTION_ARGS);
extern Datum PG_char_to_encoding(PG_FUNCTION_ARGS);
extern Datum PG_character_set_name(PG_FUNCTION_ARGS);
extern Datum PG_character_set_id(PG_FUNCTION_ARGS);
extern Datum pg_convert(PG_FUNCTION_ARGS);
extern Datum pg_convert2(PG_FUNCTION_ARGS);

/* format_type.c */
extern Datum format_type(PG_FUNCTION_ARGS);
extern char *format_type_be(Oid type_oid);
extern char *format_type_with_typemod(Oid type_oid, int32 typemod);
extern Datum oidvectortypes(PG_FUNCTION_ARGS);
extern int32 type_maximum_size(Oid type_oid, int32 typemod);

/* quote.c */
extern Datum quote_ident(PG_FUNCTION_ARGS);
extern Datum quote_literal(PG_FUNCTION_ARGS);

/* guc.c */
extern Datum show_config_by_name(PG_FUNCTION_ARGS);
extern Datum set_config_by_name(PG_FUNCTION_ARGS);
extern Datum show_all_settings(PG_FUNCTION_ARGS);

/* lockfuncs.c */
extern Datum pg_lock_status(PG_FUNCTION_ARGS);

/* catalog/pg_conversion.c */
extern Datum pg_convert_using(PG_FUNCTION_ARGS);

#endif   /* BUILTINS_H */
