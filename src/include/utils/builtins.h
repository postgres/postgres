/*-------------------------------------------------------------------------
 *
 * builtins.h
 *	  Declarations for operations on built-in types.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: builtins.h,v 1.130 2000/08/03 23:07:51 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUILTINS_H
#define BUILTINS_H

#include "nodes/relation.h"		/* for amcostestimate parameters */
#include "storage/itemptr.h"
#include "utils/numeric.h"

/*
 *		Defined in adt/
 */
/* bool.c */
extern Datum boolin(PG_FUNCTION_ARGS);
extern Datum boolout(PG_FUNCTION_ARGS);
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
extern Datum cidin(PG_FUNCTION_ARGS);
extern Datum cidout(PG_FUNCTION_ARGS);
extern Datum cideq(PG_FUNCTION_ARGS);

/* int.c */
extern Datum int2in(PG_FUNCTION_ARGS);
extern Datum int2out(PG_FUNCTION_ARGS);
extern Datum int2vectorin(PG_FUNCTION_ARGS);
extern Datum int2vectorout(PG_FUNCTION_ARGS);
extern Datum int2vectoreq(PG_FUNCTION_ARGS);
extern Datum int44in(PG_FUNCTION_ARGS);
extern Datum int44out(PG_FUNCTION_ARGS);
extern Datum int4in(PG_FUNCTION_ARGS);
extern Datum int4out(PG_FUNCTION_ARGS);
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
extern Datum int4pl(PG_FUNCTION_ARGS);
extern Datum int4mi(PG_FUNCTION_ARGS);
extern Datum int4mul(PG_FUNCTION_ARGS);
extern Datum int4div(PG_FUNCTION_ARGS);
extern Datum int4abs(PG_FUNCTION_ARGS);
extern Datum int4inc(PG_FUNCTION_ARGS);
extern Datum int2um(PG_FUNCTION_ARGS);
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

/* name.c */
extern Datum namein(PG_FUNCTION_ARGS);
extern Datum nameout(PG_FUNCTION_ARGS);
extern Datum nameeq(PG_FUNCTION_ARGS);
extern Datum namene(PG_FUNCTION_ARGS);
extern Datum namelt(PG_FUNCTION_ARGS);
extern Datum namele(PG_FUNCTION_ARGS);
extern Datum namegt(PG_FUNCTION_ARGS);
extern Datum namege(PG_FUNCTION_ARGS);
extern Datum getpgusername(PG_FUNCTION_ARGS);
extern int	namecpy(Name n1, Name n2);
extern int	namestrcpy(Name name, const char *str);
extern int	namestrcmp(Name name, const char *str);

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
extern Datum btcharcmp(PG_FUNCTION_ARGS);
extern Datum btnamecmp(PG_FUNCTION_ARGS);
extern Datum bttextcmp(PG_FUNCTION_ARGS);

/* float.c */
extern Datum float4in(PG_FUNCTION_ARGS);
extern Datum float4out(PG_FUNCTION_ARGS);
extern Datum float8in(PG_FUNCTION_ARGS);
extern Datum float8out(PG_FUNCTION_ARGS);
extern Datum float4abs(PG_FUNCTION_ARGS);
extern Datum float4um(PG_FUNCTION_ARGS);
extern Datum float4larger(PG_FUNCTION_ARGS);
extern Datum float4smaller(PG_FUNCTION_ARGS);
extern Datum float8abs(PG_FUNCTION_ARGS);
extern Datum float8um(PG_FUNCTION_ARGS);
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
extern Datum oidrand(PG_FUNCTION_ARGS);
extern Datum oidsrand(PG_FUNCTION_ARGS);
extern Datum userfntest(PG_FUNCTION_ARGS);

/* not_in.c */
extern Datum int4notin(PG_FUNCTION_ARGS);
extern Datum oidnotin(PG_FUNCTION_ARGS);

/* oid.c */
extern Datum oidvectorin(PG_FUNCTION_ARGS);
extern Datum oidvectorout(PG_FUNCTION_ARGS);
extern Datum oidin(PG_FUNCTION_ARGS);
extern Datum oidout(PG_FUNCTION_ARGS);
extern Datum oideq(PG_FUNCTION_ARGS);
extern Datum oidne(PG_FUNCTION_ARGS);
extern Datum oidvectoreq(PG_FUNCTION_ARGS);
extern Datum oidvectorne(PG_FUNCTION_ARGS);
extern Datum oidvectorlt(PG_FUNCTION_ARGS);
extern Datum oidvectorle(PG_FUNCTION_ARGS);
extern Datum oidvectorge(PG_FUNCTION_ARGS);
extern Datum oidvectorgt(PG_FUNCTION_ARGS);
extern Datum oideqint4(PG_FUNCTION_ARGS);
extern Datum int4eqoid(PG_FUNCTION_ARGS);
extern Datum oid_text(PG_FUNCTION_ARGS);
extern Datum text_oid(PG_FUNCTION_ARGS);

/* regexp.c */
extern Datum nameregexeq(PG_FUNCTION_ARGS);
extern Datum nameregexne(PG_FUNCTION_ARGS);
extern Datum textregexeq(PG_FUNCTION_ARGS);
extern Datum textregexne(PG_FUNCTION_ARGS);
extern Datum nameicregexeq(PG_FUNCTION_ARGS);
extern Datum nameicregexne(PG_FUNCTION_ARGS);
extern Datum texticregexeq(PG_FUNCTION_ARGS);
extern Datum texticregexne(PG_FUNCTION_ARGS);

/* regproc.c */
extern Datum regprocin(PG_FUNCTION_ARGS);
extern Datum regprocout(PG_FUNCTION_ARGS);
extern Datum regproctooid(PG_FUNCTION_ARGS);

/* define macro to replace mixed-case function call - tgl 97/04/27 */
#define RegprocToOid(rp) ((Oid) (rp))

/* ruleutils.c */
extern Datum pg_get_ruledef(PG_FUNCTION_ARGS);
extern Datum pg_get_viewdef(PG_FUNCTION_ARGS);
extern Datum pg_get_indexdef(PG_FUNCTION_ARGS);
extern Datum pg_get_userbyid(PG_FUNCTION_ARGS);
extern char *deparse_expression(Node *expr, List *rangetables,
				   bool forceprefix);

/* selfuncs.c */
extern Datum eqsel(PG_FUNCTION_ARGS);
extern Datum neqsel(PG_FUNCTION_ARGS);
extern Datum scalarltsel(PG_FUNCTION_ARGS);
extern Datum scalargtsel(PG_FUNCTION_ARGS);
extern Datum regexeqsel(PG_FUNCTION_ARGS);
extern Datum icregexeqsel(PG_FUNCTION_ARGS);
extern Datum likesel(PG_FUNCTION_ARGS);
extern Datum regexnesel(PG_FUNCTION_ARGS);
extern Datum icregexnesel(PG_FUNCTION_ARGS);
extern Datum nlikesel(PG_FUNCTION_ARGS);

extern Datum eqjoinsel(PG_FUNCTION_ARGS);
extern Datum neqjoinsel(PG_FUNCTION_ARGS);
extern Datum scalarltjoinsel(PG_FUNCTION_ARGS);
extern Datum scalargtjoinsel(PG_FUNCTION_ARGS);
extern Datum regexeqjoinsel(PG_FUNCTION_ARGS);
extern Datum icregexeqjoinsel(PG_FUNCTION_ARGS);
extern Datum likejoinsel(PG_FUNCTION_ARGS);
extern Datum regexnejoinsel(PG_FUNCTION_ARGS);
extern Datum icregexnejoinsel(PG_FUNCTION_ARGS);
extern Datum nlikejoinsel(PG_FUNCTION_ARGS);

extern Datum btcostestimate(PG_FUNCTION_ARGS);
extern Datum rtcostestimate(PG_FUNCTION_ARGS);
extern Datum hashcostestimate(PG_FUNCTION_ARGS);
extern Datum gistcostestimate(PG_FUNCTION_ARGS);

/* selfuncs.c supporting routines that are also used by optimizer code */
typedef enum
{
	Pattern_Type_Like, Pattern_Type_Regex, Pattern_Type_Regex_IC
} Pattern_Type;

typedef enum
{
	Pattern_Prefix_None, Pattern_Prefix_Partial, Pattern_Prefix_Exact
} Pattern_Prefix_Status;

extern Pattern_Prefix_Status pattern_fixed_prefix(char *patt,
												  Pattern_Type ptype,
												  char **prefix,
												  char **rest);
extern char *make_greater_string(const char *str, Oid datatype);

/* tid.c */
extern Datum tidin(PG_FUNCTION_ARGS);
extern Datum tidout(PG_FUNCTION_ARGS);
extern Datum tideq(PG_FUNCTION_ARGS);
extern Datum currtid_byreloid(PG_FUNCTION_ARGS);
extern Datum currtid_byrelname(PG_FUNCTION_ARGS);

/* varchar.c */
extern Datum bpcharin(PG_FUNCTION_ARGS);
extern Datum bpcharout(PG_FUNCTION_ARGS);
extern Datum bpchar(PG_FUNCTION_ARGS);
extern Datum _bpchar(PG_FUNCTION_ARGS);
extern Datum char_bpchar(PG_FUNCTION_ARGS);
extern Datum bpchar_char(PG_FUNCTION_ARGS);
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
extern Datum varchar(PG_FUNCTION_ARGS);
extern Datum _varchar(PG_FUNCTION_ARGS);
extern Datum varchareq(PG_FUNCTION_ARGS);
extern Datum varcharne(PG_FUNCTION_ARGS);
extern Datum varcharlt(PG_FUNCTION_ARGS);
extern Datum varcharle(PG_FUNCTION_ARGS);
extern Datum varchargt(PG_FUNCTION_ARGS);
extern Datum varcharge(PG_FUNCTION_ARGS);
extern Datum varcharcmp(PG_FUNCTION_ARGS);
extern Datum varcharlen(PG_FUNCTION_ARGS);
extern Datum varcharoctetlen(PG_FUNCTION_ARGS);

/* varlena.c */
extern Datum textin(PG_FUNCTION_ARGS);
extern Datum textout(PG_FUNCTION_ARGS);
extern Datum textcat(PG_FUNCTION_ARGS);
extern Datum texteq(PG_FUNCTION_ARGS);
extern Datum textne(PG_FUNCTION_ARGS);
extern Datum text_lt(PG_FUNCTION_ARGS);
extern Datum text_le(PG_FUNCTION_ARGS);
extern Datum text_gt(PG_FUNCTION_ARGS);
extern Datum text_ge(PG_FUNCTION_ARGS);
extern Datum text_larger(PG_FUNCTION_ARGS);
extern Datum text_smaller(PG_FUNCTION_ARGS);
extern Datum textlen(PG_FUNCTION_ARGS);
extern Datum textoctetlen(PG_FUNCTION_ARGS);
extern Datum textpos(PG_FUNCTION_ARGS);
extern Datum text_substr(PG_FUNCTION_ARGS);
extern Datum name_text(PG_FUNCTION_ARGS);
extern Datum text_name(PG_FUNCTION_ARGS);
extern int	varstr_cmp(char *arg1, int len1, char *arg2, int len2);

extern Datum byteain(PG_FUNCTION_ARGS);
extern Datum byteaout(PG_FUNCTION_ARGS);
extern Datum byteaoctetlen(PG_FUNCTION_ARGS);
extern Datum byteaGetByte(PG_FUNCTION_ARGS);
extern Datum byteaGetBit(PG_FUNCTION_ARGS);
extern Datum byteaSetByte(PG_FUNCTION_ARGS);
extern Datum byteaSetBit(PG_FUNCTION_ARGS);

/* version.c */
extern Datum pgsql_version(PG_FUNCTION_ARGS);

/* like.c */
extern Datum namelike(PG_FUNCTION_ARGS);
extern Datum namenlike(PG_FUNCTION_ARGS);
extern Datum textlike(PG_FUNCTION_ARGS);
extern Datum textnlike(PG_FUNCTION_ARGS);

/* oracle_compat.c */
extern Datum lower(PG_FUNCTION_ARGS);
extern Datum upper(PG_FUNCTION_ARGS);
extern Datum initcap(PG_FUNCTION_ARGS);
extern Datum lpad(PG_FUNCTION_ARGS);
extern Datum rpad(PG_FUNCTION_ARGS);
extern Datum btrim(PG_FUNCTION_ARGS);
extern Datum ltrim(PG_FUNCTION_ARGS);
extern Datum rtrim(PG_FUNCTION_ARGS);
extern Datum translate(PG_FUNCTION_ARGS);
extern Datum ichar(PG_FUNCTION_ARGS);
extern Datum repeat(PG_FUNCTION_ARGS);
extern Datum ascii(PG_FUNCTION_ARGS);

/* inet_net_ntop.c */
extern char *inet_net_ntop(int af, const void *src, int bits,
						   char *dst, size_t size);
extern char *inet_cidr_ntop(int af, const void *src, int bits,
							char *dst, size_t size);

/* inet_net_pton.c */
extern int	inet_net_pton(int af, const char *src,
						  void *dst, size_t size);

/* network.c */
extern Datum inet_in(PG_FUNCTION_ARGS);
extern Datum inet_out(PG_FUNCTION_ARGS);
extern Datum cidr_in(PG_FUNCTION_ARGS);
extern Datum cidr_out(PG_FUNCTION_ARGS);
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
extern Datum network_masklen(PG_FUNCTION_ARGS);
extern Datum network_broadcast(PG_FUNCTION_ARGS);
extern Datum network_host(PG_FUNCTION_ARGS);

/* mac.c */
extern Datum macaddr_in(PG_FUNCTION_ARGS);
extern Datum macaddr_out(PG_FUNCTION_ARGS);
extern Datum macaddr_cmp(PG_FUNCTION_ARGS);
extern Datum macaddr_lt(PG_FUNCTION_ARGS);
extern Datum macaddr_le(PG_FUNCTION_ARGS);
extern Datum macaddr_eq(PG_FUNCTION_ARGS);
extern Datum macaddr_ge(PG_FUNCTION_ARGS);
extern Datum macaddr_gt(PG_FUNCTION_ARGS);
extern Datum macaddr_ne(PG_FUNCTION_ARGS);
extern Datum macaddr_manuf(PG_FUNCTION_ARGS);

/* numeric.c */
extern Datum numeric_in(PG_FUNCTION_ARGS);
extern Datum numeric_out(PG_FUNCTION_ARGS);
extern Datum numeric(PG_FUNCTION_ARGS);
extern Datum numeric_abs(PG_FUNCTION_ARGS);
extern Datum numeric_uminus(PG_FUNCTION_ARGS);
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
extern Datum float4_numeric(PG_FUNCTION_ARGS);
extern Datum numeric_float4(PG_FUNCTION_ARGS);
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

/* even if MULTIBYTE is not enabled, these functions are necessary
 * since pg_proc.h has references to them.
 */
extern Datum getdatabaseencoding(PG_FUNCTION_ARGS);
extern Datum PG_encoding_to_char(PG_FUNCTION_ARGS);
extern Datum PG_char_to_encoding(PG_FUNCTION_ARGS);

/* format_type.c */
extern Datum format_type(PG_FUNCTION_ARGS);
extern Datum oidvectortypes(PG_FUNCTION_ARGS);

#endif	 /* BUILTINS_H */
