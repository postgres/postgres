/*-------------------------------------------------------------------------
 *
 * builtins.h--
 *	  Declarations for operations on built-in types.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: builtins.h,v 1.69.2.1 1998/12/14 00:15:18 thomas Exp $
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

#include <storage/itemptr.h>
#include <utils/geo_decls.h>
#include <utils/datetime.h>
#include <utils/nabstime.h>
#include <utils/int8.h>
#include <utils/cash.h>
#include <utils/inet.h>
#include <utils/rel.h>

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
extern text* char_text(int8 arg1);

/* int.c */
extern int32 int2in(char *num);
extern char *int2out(int16 sh);
extern int16 *int28in(char *shs);
extern char *int28out(int16 *shs);
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
extern NameData *namein(char *s);
extern char *nameout(NameData *s);
extern bool nameeq(NameData *arg1, NameData *arg2);
extern bool namene(NameData *arg1, NameData *arg2);
extern bool namelt(NameData *arg1, NameData *arg2);
extern bool namele(NameData *arg1, NameData *arg2);
extern bool namegt(NameData *arg1, NameData *arg2);
extern bool namege(NameData *arg1, NameData *arg2);
extern int	namecpy(Name n1, Name n2);
extern int	namestrcpy(Name name, char *str);
extern int	namestrcmp(Name name, char *str);

/* numutils.c */
/* XXX hack.  HP-UX has a ltoa (with different arguments) already. */
#ifdef __hpux
#define ltoa pg_ltoa
#endif	 /* hpux */
extern int32 pg_atoi(char *s, int size, int c);
extern void itoa(int i, char *a);
extern void ltoa(int32 l, char *a);

/*
 *		Per-opclass comparison functions for new btrees.  These are
 *		stored in pg_amproc and defined in nbtree/
 */
extern int32 btint2cmp(int16 a, int16 b);
extern int32 btint4cmp(int32 a, int32 b);
extern int32 btint24cmp(int16 a, int32 b);
extern int32 btint42cmp(int32 a, int16 b);
extern int32 btfloat4cmp(float32 a, float32 b);
extern int32 btfloat8cmp(float64 a, float64 b);
extern int32 btoidcmp(Oid a, Oid b);
extern int32 btoid8cmp(Oid *a, Oid *b);
extern int32 btabstimecmp(AbsoluteTime a, AbsoluteTime b);
extern int32 btcharcmp(char a, char b);
extern int32 btnamecmp(NameData *a, NameData *b);
extern int32 bttextcmp(struct varlena * a, struct varlena * b);

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

/* date.c */
extern int32 reltimein(char *timestring);
extern char *reltimeout(int32 timevalue);
extern TimeInterval tintervalin(char *intervalstr);
extern char *tintervalout(TimeInterval interval);
extern RelativeTime timespan_reltime(TimeSpan *timespan);
extern TimeSpan *reltime_timespan(RelativeTime reltime);
extern TimeInterval mktinterval(AbsoluteTime t1, AbsoluteTime t2);
extern AbsoluteTime timepl(AbsoluteTime t1, RelativeTime t2);
extern AbsoluteTime timemi(AbsoluteTime t1, RelativeTime t2);

/* extern RelativeTime abstimemi(AbsoluteTime t1, AbsoluteTime t2);  static*/
extern int	ininterval(AbsoluteTime t, TimeInterval interval);
extern RelativeTime intervalrel(TimeInterval interval);
extern AbsoluteTime timenow(void);
extern bool reltimeeq(RelativeTime t1, RelativeTime t2);
extern bool reltimene(RelativeTime t1, RelativeTime t2);
extern bool reltimelt(RelativeTime t1, RelativeTime t2);
extern bool reltimegt(RelativeTime t1, RelativeTime t2);
extern bool reltimele(RelativeTime t1, RelativeTime t2);
extern bool reltimege(RelativeTime t1, RelativeTime t2);
extern bool intervalsame(TimeInterval i1, TimeInterval i2);
extern bool intervaleq(TimeInterval i1, TimeInterval i2);
extern bool intervalne(TimeInterval i1, TimeInterval i2);
extern bool intervallt(TimeInterval i1, TimeInterval i2);
extern bool intervalgt(TimeInterval i1, TimeInterval i2);
extern bool intervalle(TimeInterval i1, TimeInterval i2);
extern bool intervalge(TimeInterval i1, TimeInterval i2);
extern bool intervalleneq(TimeInterval i, RelativeTime t);
extern bool intervallenne(TimeInterval i, RelativeTime t);
extern bool intervallenlt(TimeInterval i, RelativeTime t);
extern bool intervallengt(TimeInterval i, RelativeTime t);
extern bool intervallenle(TimeInterval i, RelativeTime t);
extern bool intervallenge(TimeInterval i, RelativeTime t);
extern bool intervalct(TimeInterval i1, TimeInterval i2);
extern bool intervalov(TimeInterval i1, TimeInterval i2);
extern AbsoluteTime intervalstart(TimeInterval i);
extern AbsoluteTime intervalend(TimeInterval i);
extern text *timeofday(void);

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

/* geo_ops.c, geo_selfuncs.c */
extern double *box_area(BOX *box);

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
extern bool int4notin(int16 not_in_arg, char *relation_and_attr);
extern bool oidnotin(Oid the_oid, char *compare);

/* oid.c */
extern Oid *oid8in(char *oidString);
extern char *oid8out(Oid *oidArray);
extern Oid	oidin(char *s);
extern char *oidout(Oid o);
extern bool oideq(Oid arg1, Oid arg2);
extern bool oidne(Oid arg1, Oid arg2);
extern bool oid8eq(Oid *arg1, Oid *arg2);
extern bool oid8ne(Oid *arg1, Oid *arg2);
extern bool oid8lt(Oid *arg1, Oid *arg2);
extern bool oid8le(Oid *arg1, Oid *arg2);
extern bool oid8ge(Oid *arg1, Oid *arg2);
extern bool oid8gt(Oid *arg1, Oid *arg2);
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
extern text *oid8types(Oid *oidArray);
extern Oid	regproctooid(RegProcedure rp);

/* define macro to replace mixed-case function call - tgl 97/04/27 */
#define RegprocToOid(rp) regproctooid(rp)

/* selfuncs.c */
extern float64 eqsel(Oid opid, Oid relid, AttrNumber attno, char *value, int32 flag);
extern float64 neqsel(Oid opid, Oid relid, AttrNumber attno, char *value, int32 flag);
extern float64 intltsel(Oid opid, Oid relid, AttrNumber attno, int32 value, int32 flag);
extern float64 intgtsel(Oid opid, Oid relid, AttrNumber attno, int32 value, int32 flag);
extern float64 eqjoinsel(Oid opid, Oid relid1, AttrNumber attno1, Oid relid2, AttrNumber attno2);
extern float64 neqjoinsel(Oid opid, Oid relid1, AttrNumber attno1, Oid relid2, AttrNumber attno2);
extern float64 intltjoinsel(Oid opid, Oid relid1, AttrNumber attno1, Oid relid2, AttrNumber attno2);
extern float64 intgtjoinsel(Oid opid, Oid relid1, AttrNumber attno1, Oid relid2, AttrNumber attno2);
extern float64 btreesel(Oid operatorOid, Oid indrelid, AttrNumber attributeNumber, char *constValue, int32 constFlag, int32 nIndexKeys, Oid indexrelid);
extern float64 btreenpage(Oid operatorOid, Oid indrelid, AttrNumber attributeNumber, char *constValue, int32 constFlag, int32 nIndexKeys, Oid indexrelid);
extern float64 hashsel(Oid operatorOid, Oid indrelid, AttrNumber attributeNumber, char *constValue, int32 constFlag, int32 nIndexKeys, Oid indexrelid);
extern float64 hashnpage(Oid operatorOid, Oid indrelid, AttrNumber attributeNumber, char *constValue, int32 constFlag, int32 nIndexKeys, Oid indexrelid);
extern float64 rtsel(Oid operatorOid, Oid indrelid, AttrNumber attributeNumber, char *constValue, int32 constFlag, int32 nIndexKeys, Oid indexrelid);
extern float64 rtnpage(Oid operatorOid, Oid indrelid, AttrNumber attributeNumber, char *constValue, int32 constFlag, int32 nIndexKeys, Oid indexrelid);
extern float64 gistsel(Oid operatorObjectId, Oid indrelid, AttrNumber attributeNumber, char *constValue, int32 constFlag, int32 nIndexKeys, Oid indexrelid);
extern float64 gistnpage(Oid operatorObjectId, Oid indrelid, AttrNumber attributeNumber, char *constValue, int32 constFlag, int32 nIndexKeys, Oid indexrelid);

/* tid.c */
extern ItemPointer tidin(char *str);
extern char *tidout(ItemPointer itemPtr);

/* timestamp.c */
extern time_t timestamp_in(const char *timestamp_str);
extern char *timestamp_out(time_t timestamp);
extern time_t now(void);
bool		timestampeq(time_t t1, time_t t2);
bool		timestampne(time_t t1, time_t t2);
bool		timestamplt(time_t t1, time_t t2);
bool		timestampgt(time_t t1, time_t t2);
bool		timestample(time_t t1, time_t t2);
bool		timestampge(time_t t1, time_t t2);
DateTime   *timestamp_datetime(time_t timestamp);
time_t		datetime_timestamp(DateTime *datetime);

/* varchar.c */
extern char *bpcharin(char *s, int dummy, int32 atttypmod);
extern char *bpcharout(char *s);
extern char *bpchar(char *s, int32 slen);
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

/* datetime.c */
extern DateADT date_in(char *datestr);
extern char *date_out(DateADT dateVal);
extern bool date_eq(DateADT dateVal1, DateADT dateVal2);
extern bool date_ne(DateADT dateVal1, DateADT dateVal2);
extern bool date_lt(DateADT dateVal1, DateADT dateVal2);
extern bool date_le(DateADT dateVal1, DateADT dateVal2);
extern bool date_gt(DateADT dateVal1, DateADT dateVal2);
extern bool date_ge(DateADT dateVal1, DateADT dateVal2);
extern int	date_cmp(DateADT dateVal1, DateADT dateVal2);
extern DateADT date_larger(DateADT dateVal1, DateADT dateVal2);
extern DateADT date_smaller(DateADT dateVal1, DateADT dateVal2);
extern int32 date_mi(DateADT dateVal1, DateADT dateVal2);
extern DateADT date_pli(DateADT dateVal, int32 days);
extern DateADT date_mii(DateADT dateVal, int32 days);
extern DateTime *date_datetime(DateADT date);
extern DateADT datetime_date(DateTime *datetime);
extern DateTime *datetime_datetime(DateADT date, TimeADT *time);
extern DateADT abstime_date(AbsoluteTime abstime);

extern TimeADT *time_in(char *timestr);
extern char *time_out(TimeADT *time);
extern bool time_eq(TimeADT *time1, TimeADT *time2);
extern bool time_ne(TimeADT *time1, TimeADT *time2);
extern bool time_lt(TimeADT *time1, TimeADT *time2);
extern bool time_le(TimeADT *time1, TimeADT *time2);
extern bool time_gt(TimeADT *time1, TimeADT *time2);
extern bool time_ge(TimeADT *time1, TimeADT *time2);
extern int	time_cmp(TimeADT *time1, TimeADT *time2);
extern TimeADT *datetime_time(DateTime *datetime);
extern int32 int4reltime(int32 timevalue);

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
extern text *substr(text *string, int4 m, int4 n);
extern text *translate(text *string, char from, char to);

/* acl.c */

/* inet_net_ntop.c */
char *inet_net_ntop(int af, const void *src, int bits, char *dst, size_t size);
char *inet_cidr_ntop(int af, const void *src, int bits, char *dst, size_t size);

/* inet_net_pton.c */
int inet_net_pton(int af, const char *src, void *dst, size_t size);

/* network.c */
inet	   *inet_in(char *str);
char	   *inet_out(inet * addr);
inet	   *cidr_in(char *str);
char	   *cidr_out(inet *addr);
bool		network_lt(inet * a1, inet * a2);
bool		network_le(inet * a1, inet * a2);
bool		network_eq(inet * a1, inet * a2);
bool		network_ge(inet * a1, inet * a2);
bool		network_gt(inet * a1, inet * a2);
bool		network_ne(inet * a1, inet * a2);
bool		network_sub(inet * a1, inet * a2);
bool		network_subeq(inet * a1, inet * a2);
bool		network_sup(inet * a1, inet * a2);
bool		network_supeq(inet * a1, inet * a2);
int4		network_cmp(inet * a1, inet * a2);

text	   *network_network(inet * addr);
text	   *network_netmask(inet * addr);
int4		network_masklen(inet * addr);
text	   *network_broadcast(inet * addr);
text	   *network_host(inet * addr);

/* mac.c */
macaddr    *macaddr_in(char *str);
char	   *macaddr_out(macaddr * addr);
bool		macaddr_lt(macaddr * a1, macaddr * a2);
bool		macaddr_le(macaddr * a1, macaddr * a2);
bool		macaddr_eq(macaddr * a1, macaddr * a2);
bool		macaddr_ge(macaddr * a1, macaddr * a2);
bool		macaddr_gt(macaddr * a1, macaddr * a2);
bool		macaddr_ne(macaddr * a1, macaddr * a2);
int4		macaddr_cmp(macaddr * a1, macaddr * a2);
text	   *macaddr_manuf(macaddr * addr);


#endif	 /* BUILTINS_H */
