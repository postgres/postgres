/*-------------------------------------------------------------------------
 *
 * builtins.h
 *	  Declarations for operations on built-in types.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: builtins.h,v 1.102 2000/02/16 17:26:25 thomas Exp $
 *
 * NOTES
 *	  This should normally only be included by fmgr.h.
 *	  Under no circumstances should it ever be included before
 *	  including fmgr.h!
 * fmgr.h does not seem to include this file, so don't know where this
 *	comment came from. Backend code must include this stuff explicitly
 *	as far as I can tell...
 * - thomas 1998-06-08
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUILTINS_H
#define BUILTINS_H

#include "access/heapam.h"		/* for HeapTuple */
#include "nodes/relation.h"		/* for amcostestimate parameters */
#include "storage/itemptr.h"
#include "utils/array.h"
#include "utils/inet.h"
#include "utils/int8.h"
#include "utils/geo_decls.h"
#include "utils/numeric.h"
#include "utils/datetime.h"
#include "utils/timestamp.h"
#include "utils/nabstime.h"
#include "utils/date.h"

/*
 *		Defined in adt/
 */
/* bool.c */
extern bool boolin(char *b);
extern char *boolout(bool b);
extern bool booleq(bool arg1, bool arg2);
extern bool boolne(bool arg1, bool arg2);
extern bool boollt(bool arg1, bool arg2);
extern bool boolgt(bool arg1, bool arg2);
extern bool boolle(bool arg1, bool arg2);
extern bool boolge(bool arg1, bool arg2);
extern bool istrue(bool arg1);
extern bool isfalse(bool arg1);

/* char.c */
extern int32 charin(char *ch);
extern char *charout(int32 ch);
extern int32 cidin(char *s);
extern char *cidout(int32 c);
extern bool chareq(int8 arg1, int8 arg2);
extern bool charne(int8 arg1, int8 arg2);
extern bool charlt(int8 arg1, int8 arg2);
extern bool charle(int8 arg1, int8 arg2);
extern bool chargt(int8 arg1, int8 arg2);
extern bool charge(int8 arg1, int8 arg2);
extern int8 charpl(int8 arg1, int8 arg2);
extern int8 charmi(int8 arg1, int8 arg2);
extern int8 charmul(int8 arg1, int8 arg2);
extern int8 chardiv(int8 arg1, int8 arg2);
extern bool cideq(int8 arg1, int8 arg2);
extern int8 text_char(text *arg1);
extern text *char_text(int8 arg1);

/* int.c */
extern int32 int2in(char *num);
extern char *int2out(int16 sh);
extern int16 *int2vectorin(char *shs);
extern char *int2vectorout(int16 *shs);
extern int32 *int44in(char *input_string);
extern char *int44out(int32 *an_array);
extern int32 int4in(char *num);
extern char *int4out(int32 l);
extern int32 i2toi4(int16 arg1);
extern int16 i4toi2(int32 arg1);
extern text *int2_text(int16 arg1);
extern int16 text_int2(text *arg1);
extern text *int4_text(int32 arg1);
extern int32 text_int4(text *arg1);
extern bool int4eq(int32 arg1, int32 arg2);
extern bool int4ne(int32 arg1, int32 arg2);
extern bool int4lt(int32 arg1, int32 arg2);
extern bool int4le(int32 arg1, int32 arg2);
extern bool int4gt(int32 arg1, int32 arg2);
extern bool int4ge(int32 arg1, int32 arg2);
extern bool int2eq(int16 arg1, int16 arg2);
extern bool int2ne(int16 arg1, int16 arg2);
extern bool int2lt(int16 arg1, int16 arg2);
extern bool int2le(int16 arg1, int16 arg2);
extern bool int2gt(int16 arg1, int16 arg2);
extern bool int2ge(int16 arg1, int16 arg2);
extern bool int24eq(int32 arg1, int32 arg2);
extern bool int24ne(int32 arg1, int32 arg2);
extern bool int24lt(int32 arg1, int32 arg2);
extern bool int24le(int32 arg1, int32 arg2);
extern bool int24gt(int32 arg1, int32 arg2);
extern bool int24ge(int32 arg1, int32 arg2);
extern bool int42eq(int32 arg1, int32 arg2);
extern bool int42ne(int32 arg1, int32 arg2);
extern bool int42lt(int32 arg1, int32 arg2);
extern bool int42le(int32 arg1, int32 arg2);
extern bool int42gt(int32 arg1, int32 arg2);
extern bool int42ge(int32 arg1, int32 arg2);
extern bool keyfirsteq(int16 *arg1, int16 arg2);
extern int32 int4um(int32 arg);
extern int32 int4pl(int32 arg1, int32 arg2);
extern int32 int4mi(int32 arg1, int32 arg2);
extern int32 int4mul(int32 arg1, int32 arg2);
extern int32 int4div(int32 arg1, int32 arg2);
extern int32 int4inc(int32 arg);
extern int16 int2um(int16 arg);
extern int16 int2pl(int16 arg1, int16 arg2);
extern int16 int2mi(int16 arg1, int16 arg2);
extern int16 int2mul(int16 arg1, int16 arg2);
extern int16 int2div(int16 arg1, int16 arg2);
extern int16 int2inc(int16 arg);
extern int32 int24pl(int32 arg1, int32 arg2);
extern int32 int24mi(int32 arg1, int32 arg2);
extern int32 int24mul(int32 arg1, int32 arg2);
extern int32 int24div(int32 arg1, int32 arg2);
extern int32 int42pl(int32 arg1, int32 arg2);
extern int32 int42mi(int32 arg1, int32 arg2);
extern int32 int42mul(int32 arg1, int32 arg2);
extern int32 int42div(int32 arg1, int32 arg2);
extern int32 int4mod(int32 arg1, int32 arg2);
extern int32 int2mod(int16 arg1, int16 arg2);
extern int32 int24mod(int32 arg1, int32 arg2);
extern int32 int42mod(int32 arg1, int32 arg2);
extern int32 int4fac(int32 arg1);
extern int32 int2fac(int16 arg1);
extern int16 int2larger(int16 arg1, int16 arg2);
extern int16 int2smaller(int16 arg1, int16 arg2);
extern int32 int4larger(int32 arg1, int32 arg2);
extern int32 int4smaller(int32 arg1, int32 arg2);

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
#endif   /* QNX */
extern void itoa(int i, char *a);
extern void ltoa(int32 l, char *a);

/*
 *		Per-opclass comparison functions for new btrees.  These are
 *		stored in pg_amproc and defined in nbtree/
 */
extern int32 btint2cmp(int16 a, int16 b);
extern int32 btint4cmp(int32 a, int32 b);
extern int32 btint8cmp(int64 *a, int64 *b);
extern int32 btint24cmp(int16 a, int32 b);
extern int32 btint42cmp(int32 a, int16 b);
extern int32 btfloat4cmp(float32 a, float32 b);
extern int32 btfloat8cmp(float64 a, float64 b);
extern int32 btoidcmp(Oid a, Oid b);
extern int32 btoidvectorcmp(Oid *a, Oid *b);
extern int32 btabstimecmp(AbsoluteTime a, AbsoluteTime b);
extern int32 btcharcmp(char a, char b);
extern int32 btnamecmp(NameData *a, NameData *b);
extern int32 bttextcmp(struct varlena * a, struct varlena * b);
extern int32 btboolcmp(bool a, bool b);

/* support routines for the rtree access method, by opclass */
extern BOX *rt_box_union(BOX *a, BOX *b);
extern BOX *rt_box_inter(BOX *a, BOX *b);
extern void rt_box_size(BOX *a, float *size);
extern void rt_bigbox_size(BOX *a, float *size);
extern void rt_poly_size(POLYGON *a, float *size);
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
extern void CheckFloat8Val(double val); /* used by lex */
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
extern float64 i4tod(int32 num);
extern float64 i2tod(int16 num);
extern float32 dtof(float64 num);
extern int32 dtoi4(float64 num);
extern int16 dtoi2(float64 num);
extern float32 i4tof(int32 num);
extern float32 i2tof(int16 num);
extern int32 ftoi4(float32 num);
extern int16 ftoi2(float32 num);
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
extern bool nullvalue(Datum value, bool *isNull);
extern bool nonnullvalue(Datum value, bool *isNull);
extern bool oidrand(Oid o, int32 X);
extern bool oidsrand(int32 X);
extern int32 userfntest(int i);

/* define macros to replace mixed-case function calls - tgl 97/04/27 */
#define NullValue(v,b) nullvalue(v,b)
#define NonNullValue(v,b) nonnullvalue(v,b)

/* not_in.c */
extern bool int4notin(int32 not_in_arg, char *relation_and_attr);
extern bool oidnotin(Oid the_oid, char *compare);

/* oid.c */
extern Oid *oidvectorin(char *oidString);
extern char *oidvectorout(Oid *oidArray);
extern Oid	oidin(char *s);
extern char *oidout(Oid o);
extern bool oideq(Oid arg1, Oid arg2);
extern bool oidne(Oid arg1, Oid arg2);
extern bool oidvectoreq(Oid *arg1, Oid *arg2);
extern bool oidvectorne(Oid *arg1, Oid *arg2);
extern bool oidvectorlt(Oid *arg1, Oid *arg2);
extern bool oidvectorle(Oid *arg1, Oid *arg2);
extern bool oidvectorge(Oid *arg1, Oid *arg2);
extern bool oidvectorgt(Oid *arg1, Oid *arg2);
extern bool oideqint4(Oid arg1, int32 arg2);
extern bool int4eqoid(int32 arg1, Oid arg2);
extern text *oid_text(Oid arg1);
extern Oid	text_oid(text *arg1);

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
extern int32 regprocin(char *pro_name_and_oid);
extern char *regprocout(RegProcedure proid);
extern text *oidvectortypes(Oid *oidArray);
extern Oid	regproctooid(RegProcedure rp);

/* define macro to replace mixed-case function call - tgl 97/04/27 */
#define RegprocToOid(rp) regproctooid(rp)

/* ruleutils.c */
extern text *pg_get_ruledef(NameData *rname);
extern text	*pg_get_viewdef(NameData *rname);
extern text	*pg_get_indexdef(Oid indexrelid);
extern NameData *pg_get_userbyid(int32 uid);
extern char *deparse_expression(Node *expr, List *rangetables,
								bool forceprefix);

/* selfuncs.c */
extern float64 eqsel(Oid opid, Oid relid, AttrNumber attno, Datum value, int32 flag);
extern float64 neqsel(Oid opid, Oid relid, AttrNumber attno, Datum value, int32 flag);
extern float64 scalarltsel(Oid opid, Oid relid, AttrNumber attno, Datum value, int32 flag);
extern float64 scalargtsel(Oid opid, Oid relid, AttrNumber attno, Datum value, int32 flag);
extern float64 eqjoinsel(Oid opid, Oid relid1, AttrNumber attno1, Oid relid2, AttrNumber attno2);
extern float64 neqjoinsel(Oid opid, Oid relid1, AttrNumber attno1, Oid relid2, AttrNumber attno2);
extern float64 scalarltjoinsel(Oid opid, Oid relid1, AttrNumber attno1, Oid relid2, AttrNumber attno2);
extern float64 scalargtjoinsel(Oid opid, Oid relid1, AttrNumber attno1, Oid relid2, AttrNumber attno2);
extern bool convert_to_scalar(Datum value, Oid typid, double *scaleval);

extern void btcostestimate(Query *root, RelOptInfo *rel,
						   IndexOptInfo *index, List *indexQuals,
						   Cost *indexStartupCost,
						   Cost *indexTotalCost,
						   Selectivity *indexSelectivity);
extern void rtcostestimate(Query *root, RelOptInfo *rel,
						   IndexOptInfo *index, List *indexQuals,
						   Cost *indexStartupCost,
						   Cost *indexTotalCost,
						   Selectivity *indexSelectivity);
extern void hashcostestimate(Query *root, RelOptInfo *rel,
							 IndexOptInfo *index, List *indexQuals,
							 Cost *indexStartupCost,
							 Cost *indexTotalCost,
							 Selectivity *indexSelectivity);
extern void gistcostestimate(Query *root, RelOptInfo *rel,
							 IndexOptInfo *index, List *indexQuals,
							 Cost *indexStartupCost,
							 Cost *indexTotalCost,
							 Selectivity *indexSelectivity);

/* tid.c */
extern ItemPointer tidin(const char *str);
extern char *tidout(ItemPointer itemPtr);
extern bool tideq(ItemPointer,ItemPointer);
extern bool tidne(ItemPointer,ItemPointer);
extern text *tid_text(ItemPointer);
extern ItemPointer text_tid(const text *); 
extern ItemPointer currtid_byreloid(Oid relOid, ItemPointer); 
extern ItemPointer currtid_byrelname(const text* relName, ItemPointer); 

/* varchar.c */
extern char *bpcharin(char *s, int dummy, int32 atttypmod);
extern char *bpcharout(char *s);
extern char *bpchar(char *s, int32 slen);
extern ArrayType *_bpchar(ArrayType *v, int32 slen);
extern char *char_bpchar(int32 c);
extern int32 bpchar_char(char *s);
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
extern uint32 hashbpchar(struct varlena * key);

extern char *varcharin(char *s, int dummy, int32 atttypmod);
extern char *varcharout(char *s);
extern char *varchar(char *s, int32 slen);
extern ArrayType *_varchar(ArrayType *v, int32 slen);
extern bool varchareq(char *arg1, char *arg2);
extern bool varcharne(char *arg1, char *arg2);
extern bool varcharlt(char *arg1, char *arg2);
extern bool varcharle(char *arg1, char *arg2);
extern bool varchargt(char *arg1, char *arg2);
extern bool varcharge(char *arg1, char *arg2);
extern int32 varcharcmp(char *arg1, char *arg2);
extern int32 varcharlen(char *arg);
extern int32 varcharoctetlen(char *arg);
extern uint32 hashvarchar(struct varlena * key);

/* varlena.c */
extern text *textin(char *inputText);
extern char *textout(text *vlena);
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
extern text *text_substr(text *string, int32 m, int32 n);
extern text *name_text(NameData *s);
extern NameData *text_name(text *s);

extern struct varlena *byteain(char *inputText);
extern char *byteaout(struct varlena * vlena);
extern int32 byteaGetSize(struct varlena * v);
extern int32 byteaGetByte(struct varlena * v, int32 n);
extern int32 byteaGetBit(struct varlena * v, int32 n);
extern struct varlena *byteaSetByte(struct varlena * v, int32 n, int32 newByte);
extern struct varlena *byteaSetBit(struct varlena * v, int32 n, int32 newBit);

/* like.c */
extern bool namelike(NameData *n, struct varlena * p);
extern bool namenlike(NameData *s, struct varlena * p);
extern bool textlike(struct varlena * s, struct varlena * p);
extern bool textnlike(struct varlena * s, struct varlena * p);

/* oracle_compat.c */

extern text *lower(text *string);
extern text *upper(text *string);
extern text *initcap(text *string);
extern text *lpad(text *string1, int4 len, text *string2);
extern text *rpad(text *string1, int4 len, text *string2);
extern text *ltrim(text *string, text *set);
extern text *rtrim(text *string, text *set);
extern text *translate(text *string, char from, char to);

/* acl.c */

/* inet_net_ntop.c */
char	   *inet_net_ntop(int af, const void *src, int bits, char *dst, size_t size);
char	   *inet_cidr_ntop(int af, const void *src, int bits, char *dst, size_t size);

/* inet_net_pton.c */
int			inet_net_pton(int af, const char *src, void *dst, size_t size);

/* network.c */
inet	   *inet_in(char *str);
char	   *inet_out(inet *addr);
inet	   *cidr_in(char *str);
char	   *cidr_out(inet *addr);
bool		network_lt(inet *a1, inet *a2);
bool		network_le(inet *a1, inet *a2);
bool		network_eq(inet *a1, inet *a2);
bool		network_ge(inet *a1, inet *a2);
bool		network_gt(inet *a1, inet *a2);
bool		network_ne(inet *a1, inet *a2);
bool		network_sub(inet *a1, inet *a2);
bool		network_subeq(inet *a1, inet *a2);
bool		network_sup(inet *a1, inet *a2);
bool		network_supeq(inet *a1, inet *a2);
int4		network_cmp(inet *a1, inet *a2);

text	   *network_network(inet *addr);
text	   *network_netmask(inet *addr);
int4		network_masklen(inet *addr);
text	   *network_broadcast(inet *addr);
text	   *network_host(inet *addr);

/* mac.c */
macaddr    *macaddr_in(char *str);
char	   *macaddr_out(macaddr *addr);
bool		macaddr_lt(macaddr *a1, macaddr *a2);
bool		macaddr_le(macaddr *a1, macaddr *a2);
bool		macaddr_eq(macaddr *a1, macaddr *a2);
bool		macaddr_ge(macaddr *a1, macaddr *a2);
bool		macaddr_gt(macaddr *a1, macaddr *a2);
bool		macaddr_ne(macaddr *a1, macaddr *a2);
int4		macaddr_cmp(macaddr *a1, macaddr *a2);
text	   *macaddr_manuf(macaddr *addr);

/* numeric.c */
Numeric		numeric_in(char *str, int dummy, int32 typmod);
char	   *numeric_out(Numeric num);
Numeric		numeric(Numeric num, int32 typmod);
Numeric		numeric_abs(Numeric num);
Numeric		numeric_sign(Numeric num);
Numeric		numeric_round(Numeric num, int32 scale);
Numeric		numeric_trunc(Numeric num, int32 scale);
Numeric		numeric_ceil(Numeric num);
Numeric		numeric_floor(Numeric num);
int32		numeric_cmp(Numeric num1, Numeric num2);
bool		numeric_eq(Numeric num1, Numeric num2);
bool		numeric_ne(Numeric num1, Numeric num2);
bool		numeric_gt(Numeric num1, Numeric num2);
bool		numeric_ge(Numeric num1, Numeric num2);
bool		numeric_lt(Numeric num1, Numeric num2);
bool		numeric_le(Numeric num1, Numeric num2);
Numeric		numeric_add(Numeric num1, Numeric num2);
Numeric		numeric_sub(Numeric num1, Numeric num2);
Numeric		numeric_mul(Numeric num1, Numeric num2);
Numeric		numeric_div(Numeric num1, Numeric num2);
Numeric		numeric_mod(Numeric num1, Numeric num2);
Numeric		numeric_inc(Numeric num);
Numeric		numeric_dec(Numeric num);
Numeric		numeric_smaller(Numeric num1, Numeric num2);
Numeric		numeric_larger(Numeric num1, Numeric num2);
Numeric		numeric_sqrt(Numeric num);
Numeric		numeric_exp(Numeric num);
Numeric		numeric_ln(Numeric num);
Numeric		numeric_log(Numeric num1, Numeric num2);
Numeric		numeric_power(Numeric num1, Numeric num2);
Numeric		int4_numeric(int32 val);
int32		numeric_int4(Numeric num);
Numeric		float4_numeric(float32 val);
float32		numeric_float4(Numeric num);
Numeric		float8_numeric(float64 val);
float64		numeric_float8(Numeric num);

/* ri_triggers.c */
HeapTuple	RI_FKey_check_ins(FmgrInfo *proinfo);
HeapTuple	RI_FKey_check_upd(FmgrInfo *proinfo);
HeapTuple	RI_FKey_noaction_del(FmgrInfo *proinfo);
HeapTuple	RI_FKey_noaction_upd(FmgrInfo *proinfo);
HeapTuple	RI_FKey_cascade_del(FmgrInfo *proinfo);
HeapTuple	RI_FKey_cascade_upd(FmgrInfo *proinfo);
HeapTuple	RI_FKey_restrict_del(FmgrInfo *proinfo);
HeapTuple	RI_FKey_restrict_upd(FmgrInfo *proinfo);
HeapTuple	RI_FKey_setnull_del(FmgrInfo *proinfo);
HeapTuple	RI_FKey_setnull_upd(FmgrInfo *proinfo);
HeapTuple	RI_FKey_setdefault_del(FmgrInfo *proinfo);
HeapTuple	RI_FKey_setdefault_upd(FmgrInfo *proinfo);

#endif	 /* BUILTINS_H */
