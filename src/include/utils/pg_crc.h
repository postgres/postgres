/*
 * pg_crc.h
 *
 * PostgreSQL CRC support
 *
 * See Ross Williams' excellent introduction
 * A PAINLESS GUIDE TO CRC ERROR DETECTION ALGORITHMS, available from
 * http://www.ross.net/crc/ or several other net sites.
 *
 * We have three slightly different variants of a 32-bit CRC calculation:
 * CRC-32C (Castagnoli polynomial), CRC-32 (Ethernet polynomial), and a legacy
 * CRC-32 version that uses the lookup table in a funny way. They all consist
 * of four macros:
 *
 * INIT_<variant>(crc)
 *		Initialize a CRC accumulator
 *
 * COMP_<variant>(crc, data, len)
 *		Accumulate some (more) bytes into a CRC
 *
 * FIN_<variant>(crc)
 *		Finish a CRC calculation
 *
 * EQ_<variant>(c1, c2)
 *		Check for equality of two CRCs.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
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

/*
 * CRC calculation using the CRC-32C (Castagnoli) polynomial.
 *
 * We use all-ones as the initial register contents and final bit inversion.
 * This is the same algorithm used e.g. in iSCSI. See RFC 3385 for more
 * details on the choice of polynomial.
 */
#define INIT_CRC32C(crc) ((crc) = 0xFFFFFFFF)
#define FIN_CRC32C(crc)	((crc) ^= 0xFFFFFFFF)
#define COMP_CRC32C(crc, data, len)	\
	COMP_CRC32_NORMAL_TABLE(crc, data, len, pg_crc32c_table)
#define EQ_CRC32C(c1, c2) ((c1) == (c2))

/*
 * CRC-32, the same used e.g. in Ethernet.
 *
 * This is currently only used in ltree and hstore contrib modules. It uses
 * the same lookup table as the legacy algorithm below. New code should
 * use the Castagnoli version instead.
 */
#define INIT_TRADITIONAL_CRC32(crc) ((crc) = 0xFFFFFFFF)
#define FIN_TRADITIONAL_CRC32(crc)	((crc) ^= 0xFFFFFFFF)
#define COMP_TRADITIONAL_CRC32(crc, data, len)	\
	COMP_CRC32_NORMAL_TABLE(crc, data, len, pg_crc32_table)
#define EQ_TRADITIONAL_CRC32(c1, c2) ((c1) == (c2))

/*
 * The CRC algorithm used for WAL et al in pre-9.5 versions.
 *
 * This closely resembles the normal CRC-32 algorithm, but is subtly
 * different. Using Williams' terms, we use the "normal" table, but with
 * "reflected" code. That's bogus, but it was like that for years before
 * anyone noticed. It does not correspond to any polynomial in a normal CRC
 * algorithm, so it's not clear what the error-detection properties of this
 * algorithm actually are.
 *
 * We still need to carry this around because it is used in a few on-disk
 * structures that need to be pg_upgradeable. It should not be used in new
 * code.
 */
#define INIT_LEGACY_CRC32(crc) ((crc) = 0xFFFFFFFF)
#define FIN_LEGACY_CRC32(crc)	((crc) ^= 0xFFFFFFFF)
#define COMP_LEGACY_CRC32(crc, data, len)	\
	COMP_CRC32_REFLECTED_TABLE(crc, data, len, pg_crc32_table)
#define EQ_LEGACY_CRC32(c1, c2) ((c1) == (c2))

/*
 * Common code for CRC computation using a lookup table.
 */
#define COMP_CRC32_NORMAL_TABLE(crc, data, len, table)			  \
do {															  \
	const unsigned char *__data = (const unsigned char *) (data); \
	uint32		__len = (len); \
\
	while (__len-- > 0) \
	{ \
		int		__tab_index = ((int) (crc) ^ *__data++) & 0xFF; \
		(crc) = table[__tab_index] ^ ((crc) >> 8); \
	} \
} while (0)

#define COMP_CRC32_REFLECTED_TABLE(crc, data, len, table) \
do {															  \
	const unsigned char *__data = (const unsigned char *) (data); \
	uint32		__len = (len); \
\
	while (__len-- > 0) \
	{ \
		int		__tab_index = ((int) ((crc) >> 24) ^ *__data++) & 0xFF;	\
		(crc) = table[__tab_index] ^ ((crc) << 8); \
	} \
} while (0)

/* Constant tables for CRC-32C and CRC-32 polynomials */
extern CRCDLLIMPORT const uint32 pg_crc32c_table[];
extern CRCDLLIMPORT const uint32 pg_crc32_table[];

#endif   /* PG_CRC_H */
