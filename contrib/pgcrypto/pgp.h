/*
 * pgp.h
 *	  OpenPGP implementation.
 *
 * Copyright (c) 2005 Marko Kreen
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
 * contrib/pgcrypto/pgp.h
 */

#include "lib/stringinfo.h"

#include "mbuf.h"
#include "px.h"

enum PGP_S2K_TYPE
{
	PGP_S2K_SIMPLE = 0,
	PGP_S2K_SALTED = 1,
	PGP_S2K_ISALTED = 3,
};

enum PGP_PKT_TYPE
{
	PGP_PKT_RESERVED = 0,
	PGP_PKT_PUBENCRYPTED_SESSKEY = 1,
	PGP_PKT_SIGNATURE = 2,
	PGP_PKT_SYMENCRYPTED_SESSKEY = 3,
	PGP_PKT_SECRET_KEY = 5,
	PGP_PKT_PUBLIC_KEY = 6,
	PGP_PKT_SECRET_SUBKEY = 7,
	PGP_PKT_COMPRESSED_DATA = 8,
	PGP_PKT_SYMENCRYPTED_DATA = 9,
	PGP_PKT_MARKER = 10,
	PGP_PKT_LITERAL_DATA = 11,
	PGP_PKT_TRUST = 12,
	PGP_PKT_USER_ID = 13,
	PGP_PKT_PUBLIC_SUBKEY = 14,
	PGP_PKT_USER_ATTR = 17,
	PGP_PKT_SYMENCRYPTED_DATA_MDC = 18,
	PGP_PKT_MDC = 19,
	PGP_PKT_PRIV_61 = 61,		/* occurs in gpg secring */
};

enum PGP_PUB_ALGO_TYPE
{
	PGP_PUB_RSA_ENCRYPT_SIGN = 1,
	PGP_PUB_RSA_ENCRYPT = 2,
	PGP_PUB_RSA_SIGN = 3,
	PGP_PUB_ELG_ENCRYPT = 16,
	PGP_PUB_DSA_SIGN = 17,
};

enum PGP_SYMENC_TYPE
{
	PGP_SYM_PLAIN = 0,			/* ?? */
	PGP_SYM_IDEA = 1,			/* obsolete, PGP 2.6 compat */
	PGP_SYM_DES3 = 2,			/* must */
	PGP_SYM_CAST5 = 3,			/* should */
	PGP_SYM_BLOWFISH = 4,
	PGP_SYM_SAFER_SK128 = 5,	/* obsolete */
	PGP_SYM_DES_SK = 6,			/* obsolete */
	PGP_SYM_AES_128 = 7,		/* should */
	PGP_SYM_AES_192 = 8,
	PGP_SYM_AES_256 = 9,
	PGP_SYM_TWOFISH = 10,
};

enum PGP_COMPR_TYPE
{
	PGP_COMPR_NONE = 0,			/* must */
	PGP_COMPR_ZIP = 1,			/* should */
	PGP_COMPR_ZLIB = 2,
	PGP_COMPR_BZIP2 = 3,
};

enum PGP_DIGEST_TYPE
{
	PGP_DIGEST_MD5 = 1,			/* should, deprecated  */
	PGP_DIGEST_SHA1 = 2,		/* must */
	PGP_DIGEST_RIPEMD160 = 3,
	PGP_DIGEST_XSHA = 4,		/* obsolete */
	PGP_DIGEST_MD2 = 5,			/* obsolete */
	PGP_DIGEST_TIGER192 = 6,	/* obsolete */
	PGP_DIGEST_HAVAL5_160 = 7,	/* obsolete */
	PGP_DIGEST_SHA256 = 8,
	PGP_DIGEST_SHA384 = 9,
	PGP_DIGEST_SHA512 = 10,
};

#define PGP_MAX_KEY    (256/8)
#define PGP_MAX_BLOCK  (256/8)
#define PGP_MAX_DIGEST (512/8)
#define PGP_S2K_SALT   8

typedef struct PGP_MPI PGP_MPI;
typedef struct PGP_PubKey PGP_PubKey;
typedef struct PGP_Context PGP_Context;
typedef struct PGP_S2K PGP_S2K;

struct PGP_S2K
{
	uint8		mode;
	uint8		digest_algo;
	uint8		salt[8];
	uint8		iter;			/* encoded (one-octet) count */
	/* calculated: */
	uint8		key[PGP_MAX_KEY];
	uint8		key_len;
};


struct PGP_Context
{
	/*
	 * parameters
	 */
	PGP_S2K		s2k;
	int			s2k_mode;
	int			s2k_count;		/* 4-byte decoded count */
	int			s2k_digest_algo;
	int			s2k_cipher_algo;
	int			cipher_algo;
	int			compress_algo;
	int			compress_level;
	int			disable_mdc;
	int			use_sess_key;
	int			text_mode;
	int			convert_crlf;
	int			unicode_mode;

	/*
	 * internal variables
	 */
	int			mdc_checked;
	int			corrupt_prefix; /* prefix failed RFC 4880 "quick check" */
	int			unsupported_compr;	/* has bzip2 compression */
	int			unexpected_binary;	/* binary data seen in text_mode */
	int			in_mdc_pkt;
	int			use_mdcbuf_filter;
	PX_MD	   *mdc_ctx;

	PGP_PubKey *pub_key;		/* ctx owns it */
	const uint8 *sym_key;		/* ctx does not own it */
	int			sym_key_len;

	/*
	 * read or generated data
	 */
	uint8		sess_key[PGP_MAX_KEY];
	unsigned	sess_key_len;
};

/* from RFC 4880 3.7.1.3 */
#define s2k_decode_count(cval) \
	(((unsigned) 16 + (cval & 15)) << ((cval >> 4) + 6))

struct PGP_MPI
{
	uint8	   *data;
	int			bits;
	int			bytes;
};

struct PGP_PubKey
{
	uint8		ver;
	uint8		time[4];
	uint8		algo;

	/* public part */
	union
	{
		struct
		{
			PGP_MPI    *p;
			PGP_MPI    *g;
			PGP_MPI    *y;
		}			elg;
		struct
		{
			PGP_MPI    *n;
			PGP_MPI    *e;
		}			rsa;
		struct
		{
			PGP_MPI    *p;
			PGP_MPI    *q;
			PGP_MPI    *g;
			PGP_MPI    *y;
		}			dsa;
	}			pub;

	/* secret part */
	union
	{
		struct
		{
			PGP_MPI    *x;
		}			elg;
		struct
		{
			PGP_MPI    *d;
			PGP_MPI    *p;
			PGP_MPI    *q;
			PGP_MPI    *u;
		}			rsa;
		struct
		{
			PGP_MPI    *x;
		}			dsa;
	}			sec;

	uint8		key_id[8];
	int			can_encrypt;
};

int			pgp_init(PGP_Context **ctx_p);
int			pgp_encrypt(PGP_Context *ctx, MBuf *src, MBuf *dst);
int			pgp_decrypt(PGP_Context *ctx, MBuf *msrc, MBuf *mdst);
int			pgp_free(PGP_Context *ctx);

int			pgp_get_digest_code(const char *name);
int			pgp_get_cipher_code(const char *name);
const char *pgp_get_digest_name(int code);

int			pgp_set_cipher_algo(PGP_Context *ctx, const char *name);
int			pgp_set_s2k_mode(PGP_Context *ctx, int mode);
int			pgp_set_s2k_count(PGP_Context *ctx, int count);
int			pgp_set_s2k_cipher_algo(PGP_Context *ctx, const char *name);
int			pgp_set_s2k_digest_algo(PGP_Context *ctx, const char *name);
int			pgp_set_convert_crlf(PGP_Context *ctx, int doit);
int			pgp_disable_mdc(PGP_Context *ctx, int disable);
int			pgp_set_sess_key(PGP_Context *ctx, int use);
int			pgp_set_compress_algo(PGP_Context *ctx, int algo);
int			pgp_set_compress_level(PGP_Context *ctx, int level);
int			pgp_set_text_mode(PGP_Context *ctx, int mode);
int			pgp_set_unicode_mode(PGP_Context *ctx, int mode);
int			pgp_get_unicode_mode(PGP_Context *ctx);

int			pgp_set_symkey(PGP_Context *ctx, const uint8 *key, int len);
int			pgp_set_pubkey(PGP_Context *ctx, MBuf *keypkt,
						   const uint8 *key, int key_len, int pubtype);

int			pgp_get_keyid(MBuf *pgp_data, char *dst);

/* internal functions */

int			pgp_load_digest(int code, PX_MD **res);
int			pgp_load_cipher(int code, PX_Cipher **res);
int			pgp_get_cipher_key_size(int code);
int			pgp_get_cipher_block_size(int code);

int			pgp_s2k_fill(PGP_S2K *s2k, int mode, int digest_algo, int count);
int			pgp_s2k_read(PullFilter *src, PGP_S2K *s2k);
int			pgp_s2k_process(PGP_S2K *s2k, int cipher, const uint8 *key, int key_len);

typedef struct PGP_CFB PGP_CFB;
int			pgp_cfb_create(PGP_CFB **ctx_p, int algo,
						   const uint8 *key, int key_len, int resync, uint8 *iv);
void		pgp_cfb_free(PGP_CFB *ctx);
int			pgp_cfb_encrypt(PGP_CFB *ctx, const uint8 *data, int len, uint8 *dst);
int			pgp_cfb_decrypt(PGP_CFB *ctx, const uint8 *data, int len, uint8 *dst);

void		pgp_armor_encode(const uint8 *src, unsigned len, StringInfo dst,
							 int num_headers, char **keys, char **values);
int			pgp_armor_decode(const uint8 *src, int len, StringInfo dst);
int			pgp_extract_armor_headers(const uint8 *src, unsigned len,
									  int *nheaders, char ***keys, char ***values);

int			pgp_compress_filter(PushFilter **res, PGP_Context *ctx, PushFilter *dst);
int			pgp_decompress_filter(PullFilter **res, PGP_Context *ctx, PullFilter *src);

int			pgp_key_alloc(PGP_PubKey **pk_p);
void		pgp_key_free(PGP_PubKey *pk);
int			_pgp_read_public_key(PullFilter *pkt, PGP_PubKey **pk_p);

int			pgp_parse_pubenc_sesskey(PGP_Context *ctx, PullFilter *pkt);
int			pgp_create_pkt_reader(PullFilter **pf_p, PullFilter *src, int len,
								  int pkttype, PGP_Context *ctx);
int			pgp_parse_pkt_hdr(PullFilter *src, uint8 *tag, int *len_p,
							  int allow_ctx);

int			pgp_skip_packet(PullFilter *pkt);
int			pgp_expect_packet_end(PullFilter *pkt);

int			pgp_write_pubenc_sesskey(PGP_Context *ctx, PushFilter *dst);
int			pgp_create_pkt_writer(PushFilter *dst, int tag, PushFilter **res_p);

int			pgp_mpi_alloc(int bits, PGP_MPI **mpi);
int			pgp_mpi_create(uint8 *data, int bits, PGP_MPI **mpi);
int			pgp_mpi_free(PGP_MPI *mpi);
int			pgp_mpi_read(PullFilter *src, PGP_MPI **mpi);
int			pgp_mpi_write(PushFilter *dst, PGP_MPI *n);
int			pgp_mpi_hash(PX_MD *md, PGP_MPI *n);
unsigned	pgp_mpi_cksum(unsigned cksum, PGP_MPI *n);

int			pgp_elgamal_encrypt(PGP_PubKey *pk, PGP_MPI *_m,
								PGP_MPI **c1_p, PGP_MPI **c2_p);
int			pgp_elgamal_decrypt(PGP_PubKey *pk, PGP_MPI *_c1, PGP_MPI *_c2,
								PGP_MPI **msg_p);
int			pgp_rsa_encrypt(PGP_PubKey *pk, PGP_MPI *_m, PGP_MPI **c_p);
int			pgp_rsa_decrypt(PGP_PubKey *pk, PGP_MPI *_c, PGP_MPI **m_p);

extern struct PullFilterOps pgp_decrypt_filter;
