/*-------------------------------------------------------------------------
 *
 * builtins.h
 *	  Declarations for operations on built-in types.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: builtins.h,v 1.119 2000/07/05 23:11:51 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUILTINS_H
#define BUILTINS_H

#include "nodes/relation.h"		/* for amcostestimate parameters */
#include "storage/itemptr.h"
#include "utils/inet.h"
#include "utils/geo_decls.h"
#include "utils/numeric.h"
#include "utils/lztext.h"

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
extern Datum int2inc(PG_FUNCTION_ARGS);
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
extern NameData *namein(const char *s);
extern char *nameout(const NameData *s);
extern bool nameeq(const NameData *arg1, const NameData *arg2);
extern bool namene(const NameData *arg1, const NameData *arg2);
extern bool namelt(const NameData *arg1, const NameData *arg2);
extern bool namele(const NameData *arg1, const NameData *arg2);
extern bool namegt(const NameData *arg1, const NameData *arg2);
extern bool namege(const NameData *arg1, const NameData *arg2);
extern int	namecpy(Name n1, Name n2);
extern int	namestrcpy(Name name, const char *str);
extern int	namestrcmp(Name name, const char *str);

/* numutils.c */
/* XXX hack.  HP-UX has a ltoa (with different arguments) already. */
#ifdef __hpux
#define ltoa pg_ltoa
#endif	 /* hpux */
extern int32 pg_atoi(char *s, int size, int c);

/* XXX hack.  QNX has itoa and ltoa (with different arguments) already. */
#ifdef __QNX__
#define itoa pg_itoa
#define ltoa pg_ltoa
#endif	 /* QNX */
extern void itoa(int i, char *a);
extern void ltoa(int32 l, char *a);

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

/* support routines for the rtree access method, by opclass */
extern BOX *rt_box_union(BOX *a, BOX *b);
extern BOX *rt_box_inter(BOX *a, BOX *b);
extern void rt_box_size(BOX *a, float *size);
extern void rt_bigbox_size(BOX *a, float *size);
extern Datum rt_poly_size(PG_FUNCTION_ARGS);
extern POLYGON *rt_poly_union(POLYGON *a, POLYGON *b);
extern POLYGON *rt_poly_inter(POLYGON *a, POLYGON *b);

/* projection utilities */
/* extern char *GetAttributeByName();
   extern char *GetAttributeByNum(); ,
 in executor/executor.h*/


extern int32 pqtest(struct varlena * vlena);

/* arrayfuncs.c */

/* filename.c */
extern char *filename_in(char *file);
extern char *filename_out(char *s);

/* float.c */
extern float32 float4in(char *num);
extern char *float4out(float32 num);
extern float64 float8in(char *num);
extern char *float8out(float64 num);
extern float32 float4abs(float32 arg1);
extern float32 float4um(float32 arg1);
extern float32 float4larger(float32 arg1, float32 arg2);
extern float32 float4smaller(float32 arg1, float32 arg2);
extern float64 float8abs(float64 arg1);
extern float64 float8um(float64 arg1);
extern float64 float8larger(float64 arg1, float64 arg2);
extern float64 float8smaller(float64 arg1, float64 arg2);
extern float32 float4pl(float32 arg1, float32 arg2);
extern float32 float4mi(float32 arg1, float32 arg2);
extern float32 float4mul(float32 arg1, float32 arg2);
extern float32 float4div(float32 arg1, float32 arg2);
extern float32 float4inc(float32 arg1);
extern float64 float8pl(float64 arg1, float64 arg2);
extern float64 float8mi(float64 arg1, float64 arg2);
extern float64 float8mul(float64 arg1, float64 arg2);
extern float64 float8div(float64 arg1, float64 arg2);
extern float64 float8inc(float64 arg1);
extern bool float4eq(float32 arg1, float32 arg2);
extern bool float4ne(float32 arg1, float32 arg2);
extern bool float4lt(float32 arg1, float32 arg2);
extern bool float4le(float32 arg1, float32 arg2);
extern bool float4gt(float32 arg1, float32 arg2);
extern bool float4ge(float32 arg1, float32 arg2);
extern bool float8eq(float64 arg1, float64 arg2);
extern bool float8ne(float64 arg1, float64 arg2);
extern bool float8lt(float64 arg1, float64 arg2);
extern bool float8le(float64 arg1, float64 arg2);
extern bool float8gt(float64 arg1, float64 arg2);
extern bool float8ge(float64 arg1, float64 arg2);
extern float64 ftod(float32 num);
extern Datum i4tod(PG_FUNCTION_ARGS);
extern Datum i2tod(PG_FUNCTION_ARGS);
extern float32 dtof(float64 num);
extern int32 dtoi4(float64 num);
extern Datum dtoi2(PG_FUNCTION_ARGS);
extern Datum i4tof(PG_FUNCTION_ARGS);
extern Datum i2tof(PG_FUNCTION_ARGS);
extern int32 ftoi4(float32 num);
extern Datum ftoi2(PG_FUNCTION_ARGS);
extern float64 text_float8(text *str);
extern float32 text_float4(text *str);
extern text *float8_text(float64 num);
extern text *float4_text(float32 num);
extern float64 dround(float64 arg1);
extern float64 dtrunc(float64 arg1);
extern float64 dsqrt(float64 arg1);
extern float64 dcbrt(float64 arg1);
extern float64 dpow(float64 arg1, float64 arg2);
extern float64 dexp(float64 arg1);
extern float64 dlog1(float64 arg1);
extern float64 dlog10(float64 arg1);
extern float64 dacos(float64 arg1);
extern float64 dasin(float64 arg1);
extern float64 datan(float64 arg1);
extern float64 datan2(float64 arg1, float64 arg2);
extern float64 dcos(float64 arg1);
extern float64 dcot(float64 arg1);
extern float64 dsin(float64 arg1);
extern float64 dtan(float64 arg1);
extern float64 degrees(float64 arg1);
extern float64 dpi(void);
extern float64 radians(float64 arg1);
extern float64 dtan(float64 arg1);
extern float64 drandom(void);
extern int32 setseed(float64 seed);

extern float64 float48pl(float32 arg1, float64 arg2);
extern float64 float48mi(float32 arg1, float64 arg2);
extern float64 float48mul(float32 arg1, float64 arg2);
extern float64 float48div(float32 arg1, float64 arg2);
extern float64 float84pl(float64 arg1, float32 arg2);
extern float64 float84mi(float64 arg1, float32 arg2);
extern float64 float84mul(float64 arg1, float32 arg2);
extern float64 float84div(float64 arg1, float32 arg2);
extern bool float48eq(float32 arg1, float64 arg2);
extern bool float48ne(float32 arg1, float64 arg2);
extern bool float48lt(float32 arg1, float64 arg2);
extern bool float48le(float32 arg1, float64 arg2);
extern bool float48gt(float32 arg1, float64 arg2);
extern bool float48ge(float32 arg1, float64 arg2);
extern bool float84eq(float64 arg1, float32 arg2);
extern bool float84ne(float64 arg1, float32 arg2);
extern bool float84lt(float64 arg1, float32 arg2);
extern bool float84le(float64 arg1, float32 arg2);
extern bool float84gt(float64 arg1, float32 arg2);
extern bool float84ge(float64 arg1, float32 arg2);

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
extern bool nameregexeq(NameData *n, struct varlena * p);
extern bool nameregexne(NameData *s, struct varlena * p);
extern bool textregexeq(struct varlena * s, struct varlena * p);
extern bool textregexne(struct varlena * s, struct varlena * p);
extern bool nameicregexeq(NameData *s, struct varlena * p);
extern bool nameicregexne(NameData *s, struct varlena * p);
extern bool texticregexeq(struct varlena * s, struct varlena * p);
extern bool texticregexne(struct varlena * s, struct varlena * p);


/* regproc.c */
extern Datum regprocin(PG_FUNCTION_ARGS);
extern Datum regprocout(PG_FUNCTION_ARGS);
extern Datum oidvectortypes(PG_FUNCTION_ARGS);
extern Datum regproctooid(PG_FUNCTION_ARGS);

/* define macro to replace mixed-case function call - tgl 97/04/27 */
#define RegprocToOid(rp) ((Oid) (rp))

/* ruleutils.c */
extern text *pg_get_ruledef(NameData *rname);
extern text *pg_get_viewdef(NameData *rname);
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
extern ItemPointer tidin(const char *str);
extern char *tidout(ItemPointer itemPtr);
extern bool tideq(ItemPointer, ItemPointer);
extern Datum currtid_byreloid(PG_FUNCTION_ARGS);
extern Datum currtid_byrelname(PG_FUNCTION_ARGS);

/* varchar.c */

extern Datum bpcharin(PG_FUNCTION_ARGS);
extern Datum bpcharout(PG_FUNCTION_ARGS);
extern Datum bpchar(PG_FUNCTION_ARGS);
extern Datum _bpchar(PG_FUNCTION_ARGS);
extern Datum char_bpchar(PG_FUNCTION_ARGS);
extern Datum bpchar_char(PG_FUNCTION_ARGS);
extern char *name_bpchar(NameData *s);
extern NameData *bpchar_name(char *s);
extern bool bpchareq(char *arg1, char *arg2);
extern bool bpcharne(char *arg1, char *arg2);
extern bool bpcharlt(char *arg1, char *arg2);
extern bool bpcharle(char *arg1, char *arg2);
extern bool bpchargt(char *arg1, char *arg2);
extern bool bpcharge(char *arg1, char *arg2);
extern int32 bpcharcmp(char *arg1, char *arg2);
extern int32 bpcharlen(char *arg);
extern int32 bpcharoctetlen(char *arg);
extern Datum hashbpchar(PG_FUNCTION_ARGS);

extern Datum varcharin(PG_FUNCTION_ARGS);
extern Datum varcharout(PG_FUNCTION_ARGS);
extern Datum varchar(PG_FUNCTION_ARGS);
extern Datum _varchar(PG_FUNCTION_ARGS);
extern bool varchareq(char *arg1, char *arg2);
extern bool varcharne(char *arg1, char *arg2);
extern bool varcharlt(char *arg1, char *arg2);
extern bool varcharle(char *arg1, char *arg2);
extern bool varchargt(char *arg1, char *arg2);
extern bool varcharge(char *arg1, char *arg2);
extern int32 varcharcmp(char *arg1, char *arg2);
extern int32 varcharlen(char *arg);
extern int32 varcharoctetlen(char *arg);

/* varlena.c */
extern Datum textin(PG_FUNCTION_ARGS);
extern Datum textout(PG_FUNCTION_ARGS);
extern text *textcat(text *arg1, text *arg2);
extern bool texteq(text *arg1, text *arg2);
extern bool textne(text *arg1, text *arg2);
extern int	varstr_cmp(char *arg1, int len1, char *arg2, int len2);
extern bool text_lt(text *arg1, text *arg2);
extern bool text_le(text *arg1, text *arg2);
extern bool text_gt(text *arg1, text *arg2);
extern bool text_ge(text *arg1, text *arg2);
extern text *text_larger(text *arg1, text *arg2);
extern text *text_smaller(text *arg1, text *arg2);
extern int32 textlen(text *arg);
extern int32 textoctetlen(text *arg);
extern int32 textpos(text *arg1, text *arg2);
extern Datum text_substr(PG_FUNCTION_ARGS);
extern text *name_text(NameData *s);
extern NameData *text_name(text *s);

extern bytea *byteain(char *inputText);
extern char *byteaout(bytea *vlena);
extern int32 byteaoctetlen(bytea *v);
extern Datum byteaGetByte(PG_FUNCTION_ARGS);
extern Datum byteaGetBit(PG_FUNCTION_ARGS);
extern Datum byteaSetByte(PG_FUNCTION_ARGS);
extern Datum byteaSetBit(PG_FUNCTION_ARGS);

/* like.c */
extern bool namelike(NameData *n, struct varlena * p);
extern bool namenlike(NameData *s, struct varlena * p);
extern bool textlike(struct varlena * s, struct varlena * p);
extern bool textnlike(struct varlena * s, struct varlena * p);

/* oracle_compat.c */

extern text *lower(text *string);
extern text *upper(text *string);
extern text *initcap(text *string);
extern Datum lpad(PG_FUNCTION_ARGS);
extern Datum rpad(PG_FUNCTION_ARGS);
extern text *btrim(text *string, text *set);
extern text *ltrim(text *string, text *set);
extern text *rtrim(text *string, text *set);
extern text *translate(text *string, text *from, text *to);
extern Datum ichar(PG_FUNCTION_ARGS);
extern Datum repeat(PG_FUNCTION_ARGS);
extern int4 ascii(text *string);

/* acl.c */

/* inet_net_ntop.c */
extern char *inet_net_ntop(int af, const void *src, int bits, char *dst, size_t size);
extern char *inet_cidr_ntop(int af, const void *src, int bits, char *dst, size_t size);

/* inet_net_pton.c */
extern int	inet_net_pton(int af, const char *src, void *dst, size_t size);

/* network.c */
extern inet *inet_in(char *str);
extern char *inet_out(inet *addr);
extern inet *cidr_in(char *str);
extern char *cidr_out(inet *addr);
extern bool network_lt(inet *a1, inet *a2);
extern bool network_le(inet *a1, inet *a2);
extern bool network_eq(inet *a1, inet *a2);
extern bool network_ge(inet *a1, inet *a2);
extern bool network_gt(inet *a1, inet *a2);
extern bool network_ne(inet *a1, inet *a2);
extern bool network_sub(inet *a1, inet *a2);
extern bool network_subeq(inet *a1, inet *a2);
extern bool network_sup(inet *a1, inet *a2);
extern bool network_supeq(inet *a1, inet *a2);
extern int4 network_cmp(inet *a1, inet *a2);

extern text *network_network(inet *addr);
extern text *network_netmask(inet *addr);
extern int4 network_masklen(inet *addr);
extern text *network_broadcast(inet *addr);
extern text *network_host(inet *addr);

/* mac.c */
extern macaddr *macaddr_in(char *str);
extern char *macaddr_out(macaddr *addr);
extern bool macaddr_lt(macaddr *a1, macaddr *a2);
extern bool macaddr_le(macaddr *a1, macaddr *a2);
extern bool macaddr_eq(macaddr *a1, macaddr *a2);
extern bool macaddr_ge(macaddr *a1, macaddr *a2);
extern bool macaddr_gt(macaddr *a1, macaddr *a2);
extern bool macaddr_ne(macaddr *a1, macaddr *a2);
extern int4 macaddr_cmp(macaddr *a1, macaddr *a2);
extern text *macaddr_manuf(macaddr *addr);

/* numeric.c */
extern Datum numeric_in(PG_FUNCTION_ARGS);
extern Datum numeric_out(PG_FUNCTION_ARGS);
extern Datum numeric(PG_FUNCTION_ARGS);
extern Numeric numeric_abs(Numeric num);
extern Numeric numeric_uminus(Numeric num);
extern Numeric numeric_sign(Numeric num);
extern Datum numeric_round(PG_FUNCTION_ARGS);
extern Datum numeric_trunc(PG_FUNCTION_ARGS);
extern Numeric numeric_ceil(Numeric num);
extern Numeric numeric_floor(Numeric num);
extern int32 numeric_cmp(Numeric num1, Numeric num2);
extern bool numeric_eq(Numeric num1, Numeric num2);
extern bool numeric_ne(Numeric num1, Numeric num2);
extern bool numeric_gt(Numeric num1, Numeric num2);
extern bool numeric_ge(Numeric num1, Numeric num2);
extern bool numeric_lt(Numeric num1, Numeric num2);
extern bool numeric_le(Numeric num1, Numeric num2);
extern Numeric numeric_add(Numeric num1, Numeric num2);
extern Numeric numeric_sub(Numeric num1, Numeric num2);
extern Numeric numeric_mul(Numeric num1, Numeric num2);
extern Numeric numeric_div(Numeric num1, Numeric num2);
extern Numeric numeric_mod(Numeric num1, Numeric num2);
extern Numeric numeric_inc(Numeric num);
extern Numeric numeric_dec(Numeric num);
extern Numeric numeric_smaller(Numeric num1, Numeric num2);
extern Numeric numeric_larger(Numeric num1, Numeric num2);
extern Numeric numeric_sqrt(Numeric num);
extern Numeric numeric_exp(Numeric num);
extern Numeric numeric_ln(Numeric num);
extern Numeric numeric_log(Numeric num1, Numeric num2);
extern Numeric numeric_power(Numeric num1, Numeric num2);
extern Datum int4_numeric(PG_FUNCTION_ARGS);
extern int32 numeric_int4(Numeric num);
extern Numeric int8_numeric(int64 *val);
extern int64 *numeric_int8(Numeric num);
extern Datum int2_numeric(PG_FUNCTION_ARGS);
extern Datum numeric_int2(PG_FUNCTION_ARGS);
extern Numeric float4_numeric(float32 val);
extern float32 numeric_float4(Numeric num);
extern Numeric float8_numeric(float64 val);
extern float64 numeric_float8(Numeric num);

/* lztext.c */
lztext	   *lztextin(char *str);
char	   *lztextout(lztext *lz);
text	   *lztext_text(lztext *lz);
lztext	   *text_lztext(text *txt);
int32		lztextlen(lztext *lz);
int32		lztextoctetlen(lztext *lz);
int32		lztext_cmp(lztext *lz1, lztext *lz2);
bool		lztext_eq(lztext *lz1, lztext *lz2);
bool		lztext_ne(lztext *lz1, lztext *lz2);
bool		lztext_gt(lztext *lz1, lztext *lz2);
bool		lztext_ge(lztext *lz1, lztext *lz2);
bool		lztext_lt(lztext *lz1, lztext *lz2);
bool		lztext_le(lztext *lz1, lztext *lz2);

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

#endif	 /* BUILTINS_H */
