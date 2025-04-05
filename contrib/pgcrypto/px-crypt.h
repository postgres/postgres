/*
 * px-crypt.h
 *		Header file for px_crypt().
 *
 * Copyright (c) 2001 Marko Kreen
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
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * contrib/pgcrypto/px-crypt.h
 */

#ifndef _PX_CRYPT_H
#define _PX_CRYPT_H

/* max room for result */
#define PX_MAX_CRYPT  128

/* max salt returned by gen_salt() */
#define PX_MAX_SALT_LEN		128

/* default rounds for xdes salt */
/* NetBSD bin/passwd/local_passwd.c has (29 * 25)*/
#define PX_XDES_ROUNDS		(29 * 25)

/* default for blowfish salt */
#define PX_BF_ROUNDS		6

/* Maximum salt string length of shacrypt.  */
#define PX_SHACRYPT_SALT_MAX_LEN 16

/* SHA buffer length */
#define PX_SHACRYPT_DIGEST_MAX_LEN 64

/* calculated buffer size of a buffer to store a shacrypt salt string */
#define PX_SHACRYPT_SALT_BUF_LEN (3 + 7 + 10 + PX_SHACRYPT_SALT_MAX_LEN + 1)

/*
 * calculated buffer size of a buffer to store complete result of a shacrypt
 * digest including salt
 */
#define PX_SHACRYPT_BUF_LEN (PX_SHACRYPT_SALT_BUF_LEN + 86 + 1)

/* Default number of rounds of shacrypt if not explicitly specified.  */
#define PX_SHACRYPT_ROUNDS_DEFAULT 5000

/* Minimum number of rounds of shacrypt.  */
#define PX_SHACRYPT_ROUNDS_MIN 1000

/* Maximum number of rounds of shacrypt.  */
#define PX_SHACRYPT_ROUNDS_MAX 999999999

/*
 * main interface
 */
char	   *px_crypt(const char *psw, const char *salt, char *buf, unsigned len);
int			px_gen_salt(const char *salt_type, char *buf, int rounds);

/*
 * internal functions
 */

/* crypt-gensalt.c */
char	   *_crypt_gensalt_traditional_rn(unsigned long count,
										  const char *input, int size, char *output, int output_size);
char	   *_crypt_gensalt_extended_rn(unsigned long count,
									   const char *input, int size, char *output, int output_size);
char	   *_crypt_gensalt_md5_rn(unsigned long count,
								  const char *input, int size, char *output, int output_size);
char	   *_crypt_gensalt_blowfish_rn(unsigned long count,
									   const char *input, int size, char *output, int output_size);
char	   *_crypt_gensalt_sha256_rn(unsigned long count,
									 const char *input, int size, char *output, int output_size);
char	   *_crypt_gensalt_sha512_rn(unsigned long count,
									 const char *input, int size, char *output, int output_size);

/* disable 'extended DES crypt' */
/* #define DISABLE_XDES */

/* crypt-blowfish.c */
char	   *_crypt_blowfish_rn(const char *key, const char *setting,
							   char *output, int size);

/* crypt-des.c */
char	   *px_crypt_des(const char *key, const char *setting);

/* crypt-md5.c */
char	   *px_crypt_md5(const char *pw, const char *salt,
						 char *passwd, unsigned dstlen);

/* crypt-sha.c */
char	   *px_crypt_shacrypt(const char *pw, const char *salt, char *passwd, unsigned dstlen);

#endif							/* _PX_CRYPT_H */
