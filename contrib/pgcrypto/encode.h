/*
 * pg_encode.h
 *		encode.c
 * 
 * Copyright (c) 2001 Marko Kreen
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $Id: encode.h,v 1.1 2001/01/24 03:46:16 momjian Exp $
 */

#ifndef __PG_ENCODE_H
#define __PG_ENCODE_H

/* exported functions */
Datum encode(PG_FUNCTION_ARGS);
Datum decode(PG_FUNCTION_ARGS);

typedef struct _pg_coding pg_coding;
struct _pg_coding {
	char *name;
	uint (*encode_len)(uint dlen);
	uint (*decode_len)(uint dlen);
	uint (*encode)(uint8 *data, uint dlen, uint8 *res);
	uint (*decode)(uint8 *data, uint dlen, uint8 *res);
};

/* They are for outside usage in C code, if needed */
uint hex_encode(uint8 *src, uint len, uint8 *dst);
uint hex_decode(uint8 *src, uint len, uint8 *dst);
uint b64_encode(uint8 *src, uint len, uint8 *dst);
uint b64_decode(uint8 *src, uint len, uint8 *dst);

uint hex_enc_len(uint srclen);
uint hex_dec_len(uint srclen);
uint b64_enc_len(uint srclen);
uint b64_dec_len(uint srclen);

#endif /* __PG_ENCODE_H */

