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
 * ARE DISCLAIMED.	IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $Id: px-crypt.h,v 1.5 2001/11/05 17:46:23 momjian Exp $
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

/*
 * main interface
 */
char	   *px_crypt(const char *psw, const char *salt, char *buf, unsigned buflen);
unsigned	px_gen_salt(const char *salt_type, char *dst, int rounds);

/*
 * internal functions
 */

/* misc.c */
extern void px_crypt_to64(char *s, unsigned long v, int n);
extern char px_crypt_a64[];

/* avoid conflicts with system libs */
#define _crypt_to64 px_crypt_to64
#define _crypt_a64 px_crypt_a64

/* crypt-gensalt.c */
char *_crypt_gensalt_traditional_rn(unsigned long count,
			 const char *input, int size, char *output, int output_size);
char *_crypt_gensalt_extended_rn(unsigned long count,
			 const char *input, int size, char *output, int output_size);
char *_crypt_gensalt_md5_rn(unsigned long count,
			 const char *input, int size, char *output, int output_size);
char *_crypt_gensalt_blowfish_rn(unsigned long count,
			 const char *input, int size, char *output, int output_size);

#ifndef PX_SYSTEM_CRYPT

/* disable 'extended DES crypt' */
/* #define DISABLE_XDES */

/* crypt-blowfish.c */
char *_crypt_blowfish_rn(const char *key, const char *setting,
				   char *output, int size);

/* crypt-des.c */
char	   *px_crypt_des(const char *key, const char *setting);

/* crypt-md5.c */
char *px_crypt_md5(const char *pw, const char *salt,
			 char *dst, unsigned dstlen);
#endif   /* !PX_SYSTEM_CRYPT */

#endif   /* _PX_CRYPT_H */
