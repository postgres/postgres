/*
 * pgcrypto.h
 *		Header file for pgcrypto.
 *
 * Copyright (c) 2000 Marko Kreen
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *	  notice, this list of conditions and the following disclaimer in the
 *	  documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.	IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $Id: pgcrypto.h,v 1.7 2001/10/28 06:25:41 momjian Exp $
 */

#ifndef _PG_CRYPTO_H
#define _PG_CRYPTO_H

/* exported functions */
Datum		pg_digest(PG_FUNCTION_ARGS);
Datum		pg_digest_exists(PG_FUNCTION_ARGS);
Datum		pg_hmac(PG_FUNCTION_ARGS);
Datum		pg_hmac_exists(PG_FUNCTION_ARGS);
Datum		pg_gen_salt(PG_FUNCTION_ARGS);
Datum		pg_gen_salt_rounds(PG_FUNCTION_ARGS);
Datum		pg_crypt(PG_FUNCTION_ARGS);
Datum		pg_encrypt(PG_FUNCTION_ARGS);
Datum		pg_decrypt(PG_FUNCTION_ARGS);
Datum		pg_encrypt_iv(PG_FUNCTION_ARGS);
Datum		pg_decrypt_iv(PG_FUNCTION_ARGS);
Datum		pg_cipher_exists(PG_FUNCTION_ARGS);

#endif
