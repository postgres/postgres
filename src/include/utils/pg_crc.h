/*
 * pg_crc.h
 *
 * PostgreSQL CRC support
 *
 * See Ross Williams' excellent introduction
 * A PAINLESS GUIDE TO CRC ERROR DETECTION ALGORITHMS, available from
 * http://www.ross.net/crc/ or several other net sites.
 *
 * We use a normal (not "reflected", in Williams' terms) CRC, using initial
 * all-ones register contents and a final bit inversion.
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/pg_crc.h
 */
#ifndef PG_CRC_H
#define PG_CRC_H

/* ugly hack to let this be used in frontend and backend code on Cygwin */
#ifdef FRONTEND
#define CRCDLLIMPORT
#else
#define CRCDLLIMPORT PGDLLIMPORT
#endif

typedef uint32 pg_crc32;

/* Initialize a CRC accumulator */
#define INIT_CRC32(crc) ((crc) = 0xFFFFFFFF)

/* Finish a CRC calculation */
#define FIN_CRC32(crc)	((crc) ^= 0xFFFFFFFF)

/* Accumulate some (more) bytes into a CRC */
#define COMP_CRC32(crc, data, len)	\
do { \
	const unsigned char *__data = (const unsigned char *) (data); \
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
extern CRCDLLIMPORT const uint32 pg_crc32_table[];

#endif   /* PG_CRC_H */
