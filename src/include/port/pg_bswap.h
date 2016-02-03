/*-------------------------------------------------------------------------
 *
 * pg_bswap.h
 *	  Byte swapping.
 *
 * Macros for reversing the byte order of 32-bit and 64-bit unsigned integers.
 * For example, 0xAABBCCDD becomes 0xDDCCBBAA.  These are just wrappers for
 * built-in functions provided by the compiler where support exists.
 *
 * Note that the GCC built-in functions __builtin_bswap32() and
 * __builtin_bswap64() are documented as accepting single arguments of type
 * uint32_t and uint64_t respectively (these are also the respective return
 * types).  Use caution when using these wrapper macros with signed integers.
 *
 * Copyright (c) 2015-2016, PostgreSQL Global Development Group
 *
 * src/include/port/pg_bswap.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_BSWAP_H
#define PG_BSWAP_H

#ifdef HAVE__BUILTIN_BSWAP32
#define BSWAP32(x) __builtin_bswap32(x)
#else
#define BSWAP32(x) (((x << 24) & 0xff000000) | \
					((x << 8) & 0x00ff0000) | \
					((x >> 8) & 0x0000ff00) | \
					((x >> 24) & 0x000000ff))
#endif   /* HAVE__BUILTIN_BSWAP32 */

#ifdef HAVE__BUILTIN_BSWAP64
#define BSWAP64(x) __builtin_bswap64(x)
#else
#define BSWAP64(x) (((x << 56) & 0xff00000000000000UL) | \
					((x << 40) & 0x00ff000000000000UL) | \
					((x << 24) & 0x0000ff0000000000UL) | \
					((x << 8) & 0x000000ff00000000UL) | \
					((x >> 8) & 0x00000000ff000000UL) | \
					((x >> 24) & 0x0000000000ff0000UL) | \
					((x >> 40) & 0x000000000000ff00UL) | \
					((x >> 56) & 0x00000000000000ffUL))
#endif   /* HAVE__BUILTIN_BSWAP64 */

/*
 * Rearrange the bytes of a Datum from big-endian order into the native byte
 * order.  On big-endian machines, this does nothing at all.  Note that the C
 * type Datum is an unsigned integer type on all platforms.
 *
 * One possible application of the DatumBigEndianToNative() macro is to make
 * bitwise comparisons cheaper.  A simple 3-way comparison of Datums
 * transformed by the macro (based on native, unsigned comparisons) will return
 * the same result as a memcmp() of the corresponding original Datums, but can
 * be much cheaper.  It's generally safe to do this on big-endian systems
 * without any special transformation occurring first.
 */
#ifdef WORDS_BIGENDIAN
#define		DatumBigEndianToNative(x)	(x)
#else							/* !WORDS_BIGENDIAN */
#if SIZEOF_DATUM == 8
#define		DatumBigEndianToNative(x)	BSWAP64(x)
#else							/* SIZEOF_DATUM != 8 */
#define		DatumBigEndianToNative(x)	BSWAP32(x)
#endif   /* SIZEOF_DATUM == 8 */
#endif   /* WORDS_BIGENDIAN */

#endif   /* PG_BSWAP_H */
