/*
 * pg_crc.h
 *
 * PostgreSQL CRC support
 *
 * See Ross Williams' excellent introduction
 * A PAINLESS GUIDE TO CRC ERROR DETECTION ALGORITHMS, available from
 * ftp://ftp.rocksoft.com/papers/crc_v3.txt or several other net sites.
 *
 * We use a normal (not "reflected", in Williams' terms) CRC, using initial
 * all-ones register contents and a final bit inversion.
 *
 * The 64-bit variant is not used as of PostgreSQL 8.1, but we retain the
 * code for possible future use.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/pg_crc.h,v 1.17 2008/01/01 19:45:59 momjian Exp $
 */
#ifndef PG_CRC_H
#define PG_CRC_H


typedef uint32 pg_crc32;

/* Initialize a CRC accumulator */
#define INIT_CRC32(crc) ((crc) = 0xFFFFFFFF)

/* Finish a CRC calculation */
#define FIN_CRC32(crc)	((crc) ^= 0xFFFFFFFF)

/* Accumulate some (more) bytes into a CRC */
#define COMP_CRC32(crc, data, len)	\
do { \
	unsigned char *__data = (unsigned char *) (data); \
	uint32		__len = (len); \
\
	while (__len-- > 0) \
	{ \
		int		__tab_index = ((int) ((crc) >> 24) ^ *__data++) & 0xFF; \
		(crc) = pg_crc32_table[__tab_index] ^ ((crc) << 8); \
	} \
} while (0)

/* Check for equality of two CRCs */
#define EQ_CRC32(c1,c2)  ((c1) == (c2))

/* Constant table for CRC calculation */
extern const uint32 pg_crc32_table[];


#ifdef PROVIDE_64BIT_CRC

/*
 * If we have a 64-bit integer type, then a 64-bit CRC looks just like the
 * usual sort of implementation.  If we have no working 64-bit type, then
 * fake it with two 32-bit registers.  (Note: experience has shown that the
 * two-32-bit-registers code is as fast as, or even much faster than, the
 * 64-bit code on all but true 64-bit machines.  INT64_IS_BUSTED is therefore
 * probably the wrong control symbol to use to select the implementation.)
 */

#ifdef INT64_IS_BUSTED

/*
 * crc0 represents the LSBs of the 64-bit value, crc1 the MSBs.  Note that
 * with crc0 placed first, the output of 32-bit and 64-bit implementations
 * will be bit-compatible only on little-endian architectures.	If it were
 * important to make the two possible implementations bit-compatible on
 * all machines, we could do a configure test to decide how to order the
 * two fields, but it seems not worth the trouble.
 */
typedef struct pg_crc64
{
	uint32		crc0;
	uint32		crc1;
}	pg_crc64;

/* Initialize a CRC accumulator */
#define INIT_CRC64(crc) ((crc).crc0 = 0xffffffff, (crc).crc1 = 0xffffffff)

/* Finish a CRC calculation */
#define FIN_CRC64(crc)	((crc).crc0 ^= 0xffffffff, (crc).crc1 ^= 0xffffffff)

/* Accumulate some (more) bytes into a CRC */
#define COMP_CRC64(crc, data, len)	\
do { \
	uint32		__crc0 = (crc).crc0; \
	uint32		__crc1 = (crc).crc1; \
	unsigned char *__data = (unsigned char *) (data); \
	uint32		__len = (len); \
\
	while (__len-- > 0) \
	{ \
		int		__tab_index = ((int) (__crc1 >> 24) ^ *__data++) & 0xFF; \
		__crc1 = pg_crc64_table1[__tab_index] ^ ((__crc1 << 8) | (__crc0 >> 24)); \
		__crc0 = pg_crc64_table0[__tab_index] ^ (__crc0 << 8); \
	} \
	(crc).crc0 = __crc0; \
	(crc).crc1 = __crc1; \
} while (0)

/* Check for equality of two CRCs */
#define EQ_CRC64(c1,c2)  ((c1).crc0 == (c2).crc0 && (c1).crc1 == (c2).crc1)

/* Constant table for CRC calculation */
extern const uint32 pg_crc64_table0[];
extern const uint32 pg_crc64_table1[];
#else							/* int64 works */

typedef struct pg_crc64
{
	uint64		crc0;
}	pg_crc64;

/* Initialize a CRC accumulator */
#define INIT_CRC64(crc) ((crc).crc0 = UINT64CONST(0xffffffffffffffff))

/* Finish a CRC calculation */
#define FIN_CRC64(crc)	((crc).crc0 ^= UINT64CONST(0xffffffffffffffff))

/* Accumulate some (more) bytes into a CRC */
#define COMP_CRC64(crc, data, len)	\
do { \
	uint64		__crc0 = (crc).crc0; \
	unsigned char *__data = (unsigned char *) (data); \
	uint32		__len = (len); \
\
	while (__len-- > 0) \
	{ \
		int		__tab_index = ((int) (__crc0 >> 56) ^ *__data++) & 0xFF; \
		__crc0 = pg_crc64_table[__tab_index] ^ (__crc0 << 8); \
	} \
	(crc).crc0 = __crc0; \
} while (0)

/* Check for equality of two CRCs */
#define EQ_CRC64(c1,c2)  ((c1).crc0 == (c2).crc0)

/* Constant table for CRC calculation */
extern const uint64 pg_crc64_table[];
#endif   /* INT64_IS_BUSTED */
#endif   /* PROVIDE_64BIT_CRC */

#endif   /* PG_CRC_H */
