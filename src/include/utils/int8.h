/*-------------------------------------------------------------------------
 *
 * int8.h
 *	  Declarations for operations on 64-bit integers.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: int8.h,v 1.21 2000/05/28 17:56:20 tgl Exp $
 *
 * NOTES
 * These data types are supported on all 64-bit architectures, and may
 *	be supported through libraries on some 32-bit machines. If your machine
 *	is not currently supported, then please try to make it so, then post
 *	patches to the postgresql.org hackers mailing list.
 *
 * This code was written for and originally appeared in the contrib
 *	directory as a user-defined type.
 * - thomas 1998-06-08
 *
 *-------------------------------------------------------------------------
 */
#ifndef INT8_H
#define INT8_H


extern int64 *int8in(char *str);
extern char *int8out(int64 *val);

extern bool int8eq(int64 *val1, int64 *val2);
extern bool int8ne(int64 *val1, int64 *val2);
extern bool int8lt(int64 *val1, int64 *val2);
extern bool int8gt(int64 *val1, int64 *val2);
extern bool int8le(int64 *val1, int64 *val2);
extern bool int8ge(int64 *val1, int64 *val2);

extern bool int84eq(int64 *val1, int32 val2);
extern bool int84ne(int64 *val1, int32 val2);
extern bool int84lt(int64 *val1, int32 val2);
extern bool int84gt(int64 *val1, int32 val2);
extern bool int84le(int64 *val1, int32 val2);
extern bool int84ge(int64 *val1, int32 val2);

extern bool int48eq(int32 val1, int64 *val2);
extern bool int48ne(int32 val1, int64 *val2);
extern bool int48lt(int32 val1, int64 *val2);
extern bool int48gt(int32 val1, int64 *val2);
extern bool int48le(int32 val1, int64 *val2);
extern bool int48ge(int32 val1, int64 *val2);

extern int64 *int8um(int64 *val);
extern int64 *int8pl(int64 *val1, int64 *val2);
extern int64 *int8mi(int64 *val1, int64 *val2);
extern int64 *int8mul(int64 *val1, int64 *val2);
extern int64 *int8div(int64 *val1, int64 *val2);
extern int64 *int8abs(int64 *val1);
extern int64 *int8fac(int64 *val1);
extern int64 *int8mod(int64 *val1, int64 *val2);
extern int64 *int8larger(int64 *val1, int64 *val2);
extern int64 *int8smaller(int64 *val1, int64 *val2);

extern int64 *int84pl(int64 *val1, int32 val2);
extern int64 *int84mi(int64 *val1, int32 val2);
extern int64 *int84mul(int64 *val1, int32 val2);
extern int64 *int84div(int64 *val1, int32 val2);

extern int64 *int48pl(int32 val1, int64 *val2);
extern int64 *int48mi(int32 val1, int64 *val2);
extern int64 *int48mul(int32 val1, int64 *val2);
extern int64 *int48div(int32 val1, int64 *val2);

extern int64 *int48(int32 val);
extern int32 int84(int64 *val);

#ifdef NOT_USED
extern int16 int82(int64 *val);

#endif

extern float64 i8tod(int64 *val);
extern int64 *dtoi8(float64 val);

extern text *int8_text(int64 *val);
extern int64 *text_int8(text *str);

#endif	 /* INT8_H */
