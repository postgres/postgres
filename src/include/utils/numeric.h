/* ----------
 * numeric.h
 *
 *	Definitions for the exact numeric data type of Postgres
 *
 *	1998 Jan Wieck
 *
 * $Header: /cvsroot/pgsql/src/include/utils/numeric.h,v 1.9 2000/04/12 17:16:55 momjian Exp $
 *
 * ----------
 */

#ifndef _PG_NUMERIC_H_
#define _PG_NUMERIC_H_

/* ----------
 * The hardcoded limits and defaults of the numeric data type
 * ----------
 */
#define NUMERIC_MAX_PRECISION		1000
#define NUMERIC_DEFAULT_PRECISION	30
#define NUMERIC_DEFAULT_SCALE		6


/* ----------
 * Internal limits on the scales chosen for calculation results
 * ----------
 */
#define NUMERIC_MAX_DISPLAY_SCALE	NUMERIC_MAX_PRECISION
#define NUMERIC_MIN_DISPLAY_SCALE	(NUMERIC_DEFAULT_SCALE + 4)

#define NUMERIC_MAX_RESULT_SCALE	(NUMERIC_MAX_PRECISION * 2)
#define NUMERIC_MIN_RESULT_SCALE	(NUMERIC_DEFAULT_PRECISION + 4)


/* ----------
 * Sign values and macros to deal with packing/unpacking n_sign_dscale
 * ----------
 */
#define NUMERIC_SIGN_MASK	0xC000
#define NUMERIC_POS			0x0000
#define NUMERIC_NEG			0x4000
#define NUMERIC_NAN			0xC000
#define NUMERIC_DSCALE_MASK 0x3FFF
#define NUMERIC_SIGN(n)		((n)->n_sign_dscale & NUMERIC_SIGN_MASK)
#define NUMERIC_DSCALE(n)	((n)->n_sign_dscale & NUMERIC_DSCALE_MASK)
#define NUMERIC_IS_NAN(n)	(NUMERIC_SIGN(n) != NUMERIC_POS &&			\
								NUMERIC_SIGN(n) != NUMERIC_NEG)


/* ----------
 * The Numeric data type stored in the database
 *
 * NOTE: by convention, values in the packed form have been stripped of
 * all leading and trailing zeroes (except there will be a trailing zero
 * in the last byte, if the number of digits is odd).  In particular,
 * if the value is zero, there will be no digits at all!  The weight is
 * arbitrary in this case, but we normally set it to zero.
 * ----------
 */
typedef struct NumericData
{
	int32		varlen;			/* Variable size		*/
	int16		n_weight;		/* Weight of 1st digit	*/
	uint16		n_rscale;		/* Result scale			*/
	uint16		n_sign_dscale;	/* Sign + display scale */
	unsigned char n_data[1];	/* Digit data (2 decimal digits/byte) */
} NumericData;
typedef NumericData *Numeric;

#define NUMERIC_HDRSZ	(sizeof(int32) + sizeof(uint16) * 3)


#endif	 /* _PG_NUMERIC_H_ */
