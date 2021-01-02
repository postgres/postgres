/*
 *	hex_decode.h
 *		hex decoding
 *
 *	Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 *	Portions Copyright (c) 1994, Regents of the University of California
 *
 *	src/include/common/hex_decode.h
 */
#ifndef COMMON_HEX_DECODE_H
#define COMMON_HEX_DECODE_H

extern uint64 hex_decode(const char *src, size_t len, char *dst);


#endif							/* COMMON_HEX_DECODE_H */
