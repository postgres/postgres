/* ----------
 * numeric.h
 *
 *	Definitions for the exact numeric data type of Postgres
 *
 *	1998 Jan Wieck
 *
 * $Header: /cvsroot/pgsql/src/include/utils/numeric.h,v 1.4 1999/02/13 23:22:26 momjian Exp $
 *
 * ----------
 */

#ifndef _PG_NUMERIC_H_
#define _PG_NUMERIC_H_

#include "postgres.h"


/* ----------
 * The hardcoded limits and defaults of the numeric data type
 * ----------
 */
#define NUMERIC_MAX_PRECISION		1000
#define NUMERIC_DEFAULT_PRECISION	30
#define NUMERIC_DEFAULT_SCALE		6

#define	NUMERIC_MAX_DISPLAY_SCALE	NUMERIC_MAX_PRECISION
#define	NUMERIC_MIN_DISPLAY_SCALE	NUMERIC_DEFAULT_SCALE + 4

#define	NUMERIC_MAX_RESULT_SCALE	(NUMERIC_MAX_PRECISION * 2)
#define NUMERIC_MIN_RESULT_SCALE	(NUMERIC_DEFAULT_PRECISION + 4)

#define NUMERIC_UNPACKED_DATASIZE	(NUMERIC_MAX_PRECISION * 2 + 4)


/* ----------
 * Sign values and macros to deal with n_sign_dscale
 * ----------
 */
#define NUMERIC_SIGN_MASK	0xC000
#define	NUMERIC_POS			0x0000
#define NUMERIC_NEG			0x4000
#define NUMERIC_NAN			0xC000
#define NUMERIC_SIGN(n)		((n)->n_sign_dscale & NUMERIC_SIGN_MASK)
#define NUMERIC_DSCALE(n)	((n)->n_sign_dscale & ~NUMERIC_SIGN_MASK)
#define NUMERIC_IS_NAN(n)	(NUMERIC_SIGN(n) != NUMERIC_POS && 			\
								NUMERIC_SIGN(n) != NUMERIC_NEG)


/* ----------
 * The Numeric data type stored in the database
 * ----------
 */
typedef struct NumericData {
	int32			varlen;			/* Variable size		*/
	int16			n_weight;		/* Weight of 1st digit	*/
	uint16			n_rscale;		/* Result scale			*/
	uint16			n_sign_dscale;	/* Sign + display scale	*/
	unsigned char	n_data[1];		/* Digit data			*/
} NumericData;
typedef NumericData *Numeric;

#define NUMERIC_HDRSZ	(sizeof(int32) + sizeof(uint16) * 3)


#endif /* _PG_NUMERIC_H_ */

