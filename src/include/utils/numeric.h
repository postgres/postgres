/*-------------------------------------------------------------------------
 *
 * numeric.h
 *	  Definitions for the exact numeric data type of Postgres
 *
 * Original coding 1998, Jan Wieck.  Heavily revised 2003, Tom Lane.
 *
 * Copyright (c) 1998-2003, PostgreSQL Global Development Group
 *
 * $Id: numeric.h,v 1.17 2003/03/21 01:58:05 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PG_NUMERIC_H_
#define _PG_NUMERIC_H_

/*
 * Hardcoded precision limit - arbitrary, but must be small enough that
 * dscale values will fit in 14 bits.
 */
#define NUMERIC_MAX_PRECISION		1000

/*
 * Internal limits on the scales chosen for calculation results
 */
#define NUMERIC_MAX_DISPLAY_SCALE	NUMERIC_MAX_PRECISION
#define NUMERIC_MIN_DISPLAY_SCALE	0

#define NUMERIC_MAX_RESULT_SCALE	(NUMERIC_MAX_PRECISION * 2)

/*
 * For inherently inexact calculations such as division and square root,
 * we try to get at least this many significant digits; the idea is to
 * deliver a result no worse than float8 would.
 */
#define NUMERIC_MIN_SIG_DIGITS		16


/*
 * Sign values and macros to deal with packing/unpacking n_sign_dscale
 */
#define NUMERIC_SIGN_MASK	0xC000
#define NUMERIC_POS			0x0000
#define NUMERIC_NEG			0x4000
#define NUMERIC_NAN			0xC000
#define NUMERIC_DSCALE_MASK 0x3FFF
#define NUMERIC_SIGN(n)		((n)->n_sign_dscale & NUMERIC_SIGN_MASK)
#define NUMERIC_DSCALE(n)	((n)->n_sign_dscale & NUMERIC_DSCALE_MASK)
#define NUMERIC_IS_NAN(n)	(NUMERIC_SIGN(n) != NUMERIC_POS &&	\
							 NUMERIC_SIGN(n) != NUMERIC_NEG)


/*
 * The Numeric data type stored in the database
 *
 * NOTE: by convention, values in the packed form have been stripped of
 * all leading and trailing zero digits (where a "digit" is of base NBASE).
 * In particular, if the value is zero, there will be no digits at all!
 * The weight is arbitrary in that case, but we normally set it to zero.
 */
typedef struct NumericData
{
	int32		varlen;			/* Variable size (std varlena header) */
	int16		n_weight;		/* Weight of 1st digit	*/
	uint16		n_sign_dscale;	/* Sign + display scale */
	char		n_data[1];		/* Digits (really array of NumericDigit) */
} NumericData;

typedef NumericData *Numeric;

#define NUMERIC_HDRSZ	(sizeof(int32) + sizeof(int16) + sizeof(uint16))


/*
 * fmgr interface macros
 */

#define DatumGetNumeric(X)		  ((Numeric) PG_DETOAST_DATUM(X))
#define DatumGetNumericCopy(X)	  ((Numeric) PG_DETOAST_DATUM_COPY(X))
#define NumericGetDatum(X)		  PointerGetDatum(X)
#define PG_GETARG_NUMERIC(n)	  DatumGetNumeric(PG_GETARG_DATUM(n))
#define PG_GETARG_NUMERIC_COPY(n) DatumGetNumericCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_NUMERIC(x)	  return NumericGetDatum(x)

#endif   /* _PG_NUMERIC_H_ */
