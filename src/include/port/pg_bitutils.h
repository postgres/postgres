/*------------------------------------------------------------------------ -
 *
 * pg_bitutils.h
 *	  miscellaneous functions for bit-wise operations.
  *
 *
 * Portions Copyright(c) 2019, PostgreSQL Global Development Group
 *
 * src/include/port/pg_bitutils.h
 *
 *------------------------------------------------------------------------ -
 */

#ifndef PG_BITUTILS_H
#define PG_BITUTILS_H

extern int (*pg_popcount32) (uint32 word);
extern int (*pg_popcount64) (uint64 word);
extern int (*pg_rightmost_one32) (uint32 word);
extern int (*pg_rightmost_one64) (uint64 word);
extern int (*pg_leftmost_one32) (uint32 word);
extern int (*pg_leftmost_one64) (uint64 word);

extern uint64 pg_popcount(const char *buf, int bytes);

#endif							/* PG_BITUTILS_H */
