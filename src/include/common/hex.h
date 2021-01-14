/*------------------------------------------------------------------------
 *
 *	hex.h
 *	  Encoding and decoding routines for hex strings.
 *
 *	Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 *	Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		  src/include/common/hex.h
 *
 *------------------------------------------------------------------------
 */

#ifndef COMMON_HEX_H
#define COMMON_HEX_H

extern uint64 pg_hex_decode(const char *src, size_t srclen,
							char *dst, size_t dstlen);
extern uint64 pg_hex_encode(const char *src, size_t srclen,
							char *dst, size_t dstlen);
extern uint64 pg_hex_enc_len(size_t srclen);
extern uint64 pg_hex_dec_len(size_t srclen);

#endif							/* COMMON_HEX_H */
