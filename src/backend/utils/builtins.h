/*-------------------------------------------------------------------------
 *
 * builtins.h--
 *    Declarations for operations on built-in types.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: builtins.h,v 1.1.1.1 1996/07/09 06:22:01 scrappy Exp $
 *
 * NOTES
 *    This should normally only be included by fmgr.h.
 *    Under no circumstances should it ever be included before 
 *    including fmgr.h!
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUILTINS_H
#define BUILTINS_H

#include "postgres.h"

#include "storage/itemptr.h"

#include "storage/large_object.h"

#include "utils/geo-decls.h"

/*
 *	Defined in adt/
 */
/* bool.c */
extern int32 boolin(char *b);
extern char *boolout(long b);
extern int32 booleq(int8 arg1, int8 arg2);
extern int32 boolne(int8 arg1, int8 arg2);

/* char.c */
extern int32 charin(char *ch);
extern char *charout(int32 ch);
extern int32 cidin(char *s);
extern char *cidout(int32 c);
extern char *char16in(char *s);
extern char *char16out(char *s);
extern int32 chareq(int8 arg1, int8 arg2);
extern int32 charne(int8 arg1, int8 arg2);
extern int32 charlt(int8 arg1, int8 arg2);
extern int32 charle(int8 arg1, int8 arg2);
extern int32 chargt(int8 arg1, int8 arg2);
extern int32 charge(int8 arg1, int8 arg2);
extern int8 charpl(int8 arg1, int8 arg2);
extern int8 charmi(int8 arg1, int8 arg2);
extern int8 charmul(int8 arg1, int8 arg2);
extern int8 chardiv(int8 arg1, int8 arg2);
extern int32 cideq(int8 arg1, int8 arg2);
extern int32 char16eq(char *arg1, char *arg2);
extern int32 char16ne(char *arg1, char *arg2);
extern int32 char16lt(char *arg1, char *arg2);
extern int32 char16le(char *arg1, char *arg2);
extern int32 char16gt(char *arg1, char *arg2);
extern int32 char16ge(char *arg1, char *arg2);
extern uint16 char2in(char *s);
extern char *char2out(uint16 s);
extern int32 char2eq(uint16 a, uint16 b);
extern int32 char2ne(uint16 a, uint16 b);
extern int32 char2lt(uint16 a, uint16 b);
extern int32 char2le(uint16 a, uint16 b);
extern int32 char2gt(uint16 a, uint16 b);
extern int32 char2ge(uint16 a, uint16 b);
extern int32 char2cmp(uint16 a, uint16 b);
extern uint32 char4in(char *s);
extern char *char4out(uint32 s);
extern int32 char4eq(uint32 a, uint32 b);
extern int32 char4ne(uint32 a, uint32 b);
extern int32 char4lt(uint32 a, uint32 b);
extern int32 char4le(uint32 a, uint32 b);
extern int32 char4gt(uint32 a, uint32 b);
extern int32 char4ge(uint32 a, uint32 b);
extern int32 char4cmp(uint32 a, uint32 b);
extern char *char8in(char *s);
extern char *char8out(char *s);
extern int32 char8eq(char *arg1, char *arg2);
extern int32 char8ne(char *arg1, char *arg2);
extern int32 char8lt(char *arg1, char *arg2);
extern int32 char8le(char *arg1, char *arg2);
extern int32 char8gt(char *arg1, char *arg2);
extern int32 char8ge(char *arg1, char *arg2);
extern int32 char8cmp(char *arg1, char *arg2);

/* int.c */
extern int32 int2in(char *num);
extern char *int2out(int16 sh);
extern int16 *int28in(char *shs);
extern char *int28out(int16 (*shs)[]);
extern int32 *int44in(char *input_string);
extern char *int44out(int32 an_array[]);
extern int32 int4in(char *num);
extern char *int4out(int32 l);
extern int32 i2toi4(int16 arg1);
extern int16 i4toi2(int32 arg1);
extern int32 int4eq(int32 arg1, int32 arg2);
extern int32 int4ne(int32 arg1, int32 arg2);
extern int32 int4lt(int32 arg1, int32 arg2);
extern int32 int4le(int32 arg1, int32 arg2);
extern int32 int4gt(int32 arg1, int32 arg2);
extern int32 int4ge(int32 arg1, int32 arg2);
extern int32 int2eq(int16 arg1, int16 arg2);
extern int32 int2ne(int16 arg1, int16 arg2);
extern int32 int2lt(int16 arg1, int16 arg2);
extern int32 int2le(int16 arg1, int16 arg2);
extern int32 int2gt(int16 arg1, int16 arg2);
extern int32 int2ge(int16 arg1, int16 arg2);
extern int32 int24eq(int32 arg1, int32 arg2);
extern int32 int24ne(int32 arg1, int32 arg2);
extern int32 int24lt(int32 arg1, int32 arg2);
extern int32 int24le(int32 arg1, int32 arg2);
extern int32 int24gt(int32 arg1, int32 arg2);
extern int32 int24ge(int32 arg1, int32 arg2);
extern int32 int42eq(int32 arg1, int32 arg2);
extern int32 int42ne(int32 arg1, int32 arg2);
extern int32 int42lt(int32 arg1, int32 arg2);
extern int32 int42le(int32 arg1, int32 arg2);
extern int32 int42gt(int32 arg1, int32 arg2);
extern int32 int42ge(int32 arg1, int32 arg2);
extern int32 keyfirsteq(int16 *arg1, int16 arg2);
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
extern int32 nameeq(NameData *arg1, NameData *arg2);
extern int32 namene(NameData *arg1, NameData *arg2);
extern int32 namelt(NameData *arg1, NameData *arg2);
extern int32 namele(NameData *arg1, NameData *arg2);
extern int32 namegt(NameData *arg1, NameData *arg2);
extern int32 namege(NameData *arg1, NameData *arg2);
extern int namecmp(Name n1, Name n2);
extern int namecpy(Name n1, Name n2);
extern int namecat(Name n1, Name n2);
extern int namestrcpy(Name name, char *str);
extern int namestrcat(Name name, char *str);
extern int namestrcmp(Name name, char *str);
extern uint32 NameComputeLength(Name name);

/* numutils.c */
/* XXX hack.  HP-UX has a ltoa (with different arguments) already. */
#ifdef PORTNAME_hpux
#define ltoa pg_ltoa
#endif /* PORTNAME_hpux */
extern int32 pg_atoi(char *s, int size, int c);
extern void itoa(int i, char *a);
extern void ltoa(int32 l, char *a);
extern int ftoa(double value, char *ascii, int width, int prec1, char format);
extern int atof1(char *str, double *val);

/*
 *	Per-opclass comparison functions for new btrees.  These are
 *	stored in pg_amproc and defined in nbtree/
 */
extern int32		btint2cmp();
extern int32		btint4cmp();
extern int32		btint24cmp();
extern int32		btint42cmp();
extern int32		btfloat4cmp();
extern int32		btfloat8cmp();
extern int32		btoidcmp();
extern int32		btabstimecmp();
extern int32		btcharcmp();
extern int32		btchar16cmp();
extern int32		bttextcmp();

/*
 *	RTree code.
 *	Defined in access/index-rtree/
 */
extern char		*rtinsert();
extern char		*rtdelete();
extern char		*rtgettuple();
extern char		*rtbeginscan();
extern void		rtendscan();
extern void		rtreebuild();
extern void		rtmarkpos();
extern void		rtrestrpos();
extern void		rtrescan();
extern void		rtbuild();

/* support routines for the rtree access method, by opclass */
extern BOX		*rt_box_union();
extern BOX		*rt_box_inter();
extern float		*rt_box_size();
extern float		*rt_bigbox_size();
extern float		*rt_poly_size();
extern POLYGON	*rt_poly_union();
extern POLYGON	*rt_poly_inter();

/* projection utilities */
/* extern char *GetAttributeByName(); 
   extern char *GetAttributeByNum(); ,
 in executor/executor.h*/


extern int32 pqtest();

/* arrayfuncs.c */
#include "utils/array.h"

/* date.c */
extern int32 reltimein(char *timestring);
extern char *reltimeout(int32 timevalue);
extern TimeInterval tintervalin(char *intervalstr);
extern char *tintervalout(TimeInterval interval);
extern TimeInterval mktinterval(AbsoluteTime t1, AbsoluteTime t2);
extern AbsoluteTime timepl(AbsoluteTime t1, RelativeTime t2);
extern AbsoluteTime timemi(AbsoluteTime t1, RelativeTime t2);
/* extern RelativeTime abstimemi(AbsoluteTime t1, AbsoluteTime t2);  static*/
extern int ininterval(AbsoluteTime t, TimeInterval interval);
extern RelativeTime intervalrel(TimeInterval interval);
extern AbsoluteTime timenow(void);
extern int32 reltimeeq(RelativeTime t1, RelativeTime t2);
extern int32 reltimene(RelativeTime t1, RelativeTime t2);
extern int32 reltimelt(RelativeTime t1, RelativeTime t2);
extern int32 reltimegt(RelativeTime t1, RelativeTime t2);
extern int32 reltimele(RelativeTime t1, RelativeTime t2);
extern int32 reltimege(RelativeTime t1, RelativeTime t2);
extern int32 intervaleq(TimeInterval i1, TimeInterval i2);
extern int32 intervalleneq(TimeInterval i, RelativeTime t);
extern int32 intervallenne(TimeInterval i, RelativeTime t);
extern int32 intervallenlt(TimeInterval i, RelativeTime t);
extern int32 intervallengt(TimeInterval i, RelativeTime t);
extern int32 intervallenle(TimeInterval i, RelativeTime t);
extern int32 intervallenge(TimeInterval i, RelativeTime t);
extern int32 intervalct(TimeInterval i1, TimeInterval i2);
extern int32 intervalov(TimeInterval i1, TimeInterval i2);
extern AbsoluteTime intervalstart(TimeInterval i);
extern AbsoluteTime intervalend(TimeInterval i);
extern int isreltime(char *timestring, int *sign, long *quantity, int *unitnr);

/* dt.c */
extern int32 dtin(char *datetime);
extern char *dtout(int32 datetime);

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
extern long float4eq(float32 arg1, float32 arg2);
extern long float4ne(float32 arg1, float32 arg2);
extern long float4lt(float32 arg1, float32 arg2);
extern long float4le(float32 arg1, float32 arg2);
extern long float4gt(float32 arg1, float32 arg2);
extern long float4ge(float32 arg1, float32 arg2);
extern long float8eq(float64 arg1, float64 arg2);
extern long float8ne(float64 arg1, float64 arg2);
extern long float8lt(float64 arg1, float64 arg2);
extern long float8le(float64 arg1, float64 arg2);
extern long float8gt(float64 arg1, float64 arg2);
extern long float8ge(float64 arg1, float64 arg2);
extern float64 ftod(float32 num);
extern float32 dtof(float64 num);
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
extern long float48eq(float32 arg1, float64 arg2);
extern long float48ne(float32 arg1, float64 arg2);
extern long float48lt(float32 arg1, float64 arg2);
extern long float48le(float32 arg1, float64 arg2);
extern long float48gt(float32 arg1, float64 arg2);
extern long float48ge(float32 arg1, float64 arg2);
extern long float84eq(float64 arg1, float32 arg2);
extern long float84ne(float64 arg1, float32 arg2);
extern long float84lt(float64 arg1, float32 arg2);
extern long float84le(float64 arg1, float32 arg2);
extern long float84gt(float64 arg1, float32 arg2);
extern long float84ge(float64 arg1, float32 arg2);

/* geo-ops.c, geo-selfuncs.c */
#include "utils/geo-decls.h"

/* misc.c */
extern bool NullValue(Datum value, bool *isNull);
extern bool NonNullValue(Datum value, bool *isNull);
extern int32 userfntest(int i);

/* not_in.c */
extern bool int4notin(int16 not_in_arg, char *relation_and_attr);
extern bool oidnotin(Oid the_oid, char *compare);
extern int my_varattno(Relation rd, char *a);

/* oid.c */
extern Oid *oid8in(char *oidString);
extern char *oid8out(Oid (*oidArray)[]);
extern Oid oidin(char *s);
extern char *oidout(Oid o);
extern int32 oideq(Oid arg1, Oid arg2);
extern int32 oidne(Oid arg1, Oid arg2);
extern int32 oid8eq(Oid arg1[], Oid arg2[]);

/* regexp.c */
extern bool char2regexeq(uint16 arg1, struct varlena *p);
extern bool char2regexne(uint16 arg1, struct varlena *p);
extern bool char4regexeq(uint32 arg1, struct varlena *p);
extern bool char4regexne(uint32 arg1, struct varlena *p);
extern bool char8regexeq(char *s, struct varlena *p);
extern bool char8regexne(char *s, struct varlena *p);
extern bool char16regexeq(char *s, struct varlena *p);
extern bool char16regexne(char *s, struct varlena *p);
extern bool textregexeq(struct varlena *s, struct varlena *p);
extern bool textregexne(struct varlena *s, struct varlena *p);
extern bool char2icregexeq(uint16 arg1, struct varlena *p);
extern bool char2icregexne(uint16 arg1, struct varlena *p);
extern bool char4icregexeq(uint32 arg1, struct varlena *p);
extern bool char4icregexne(uint32 arg1, struct varlena *p);
extern bool char8icregexeq(char *s, struct varlena *p);
extern bool char8icregexne(char *s, struct varlena *p);
extern bool char16icregexeq(char *s, struct varlena *p);
extern bool char16icregexne(char *s, struct varlena *p);
extern bool nameicregexeq(NameData *s, struct varlena *p);
extern bool nameicregexne(NameData *s, struct varlena *p);
extern bool texticregexeq(struct varlena *s, struct varlena *p);
extern bool texticregexne(struct varlena *s, struct varlena *p);


/* regproc.c */
extern int32 regprocin(char *proname);
extern char *regprocout(RegProcedure proid);
extern Oid RegprocToOid(RegProcedure rp);

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

/* tid.c */
extern ItemPointer tidin(char *str);
extern char *tidout(ItemPointer itemPtr);

/* varlena.c */
extern struct varlena *byteain(char *inputText);
extern struct varlena *shove_bytes(unsigned char *stuff, int len);
extern char *byteaout(struct varlena *vlena);
extern struct varlena *textin(char *inputText);
extern char *textout(struct varlena *vlena);
extern int32 texteq(struct varlena *arg1, struct varlena *arg2);
extern int32 textne(struct varlena *arg1, struct varlena *arg2);
extern int32 text_lt(struct varlena *arg1, struct varlena *arg2);
extern int32 text_le(struct varlena *arg1, struct varlena *arg2);
extern int32 text_gt(struct varlena *arg1, struct varlena *arg2);
extern int32 text_ge(struct varlena *arg1, struct varlena *arg2);
extern int32 byteaGetSize(struct varlena *v);
extern int32 byteaGetByte(struct varlena *v, int32 n);
extern int32 byteaGetBit(struct varlena *v, int32 n);
extern struct varlena *byteaSetByte(struct varlena *v, int32 n, int32 newByte);
extern struct varlena *byteaSetBit(struct varlena *v, int32 n, int32 newBit);

/* acl.c */
#include "utils/acl.h"

#endif	/* BUILTINS_H */
