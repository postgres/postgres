/*
 * pg_crc.h
 *
 * PostgreSQL 64-bit CRC support
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_crc.h,v 1.9 2003/08/04 02:40:15 momjian Exp $
 */
#ifndef PG_CRC_H
#define PG_CRC_H

/*
 * If we have a 64-bit integer type, then a 64-bit CRC looks just like the
 * usual sort of implementation.  (See Ross Williams' excellent introduction
 * A PAINLESS GUIDE TO CRC ERROR DETECTION ALGORITHMS, available from
 * ftp://ftp.rocksoft.com/papers/crc_v3.txt or several other net sites.)
 * If we have no working 64-bit type, then fake it with two 32-bit registers.
 *
 * The present implementation is a normal (not "reflected", in Williams'
 * terms) 64-bit CRC, using initial all-ones register contents and a final
 * bit inversion.  The chosen polynomial is borrowed from the DLT1 spec
 * (ECMA-182, available from http://www.ecma.ch/ecma1/STAND/ECMA-182.HTM):
 *
 * x^64 + x^62 + x^57 + x^55 + x^54 + x^53 + x^52 + x^47 + x^46 + x^45 +
 * x^40 + x^39 + x^38 + x^37 + x^35 + x^33 + x^32 + x^31 + x^29 + x^27 +
 * x^24 + x^23 + x^22 + x^21 + x^19 + x^17 + x^13 + x^12 + x^10 + x^9 +
 * x^7 + x^4 + x + 1
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
typedef struct crc64
{
	uint32		crc0;
	uint32		crc1;
} crc64;

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
		__crc1 = crc_table1[__tab_index] ^ ((__crc1 << 8) | (__crc0 >> 24)); \
		__crc0 = crc_table0[__tab_index] ^ (__crc0 << 8); \
	} \
	(crc).crc0 = __crc0; \
	(crc).crc1 = __crc1; \
} while (0)

/* Check for equality of two CRCs */
#define EQ_CRC64(c1,c2)  ((c1).crc0 == (c2).crc0 && (c1).crc1 == (c2).crc1)

/* Constant table for CRC calculation */
extern const uint32 crc_table0[];
extern const uint32 crc_table1[];

#else							/* int64 works */

typedef struct crc64
{
	uint64		crc0;
} crc64;

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
		__crc0 = crc_table[__tab_index] ^ (__crc0 << 8); \
	} \
	(crc).crc0 = __crc0; \
} while (0)

/* Check for equality of two CRCs */
#define EQ_CRC64(c1,c2)  ((c1).crc0 == (c2).crc0)

/* Constant table for CRC calculation */
extern const uint64 crc_table[];
#endif   /* INT64_IS_BUSTED */

#endif   /* PG_CRC_H */
