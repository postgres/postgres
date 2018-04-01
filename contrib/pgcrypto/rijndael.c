/*	$OpenBSD: rijndael.c,v 1.6 2000/12/09 18:51:34 markus Exp $ */

/* contrib/pgcrypto/rijndael.c */

/* This is an independent implementation of the encryption algorithm:	*/
/*																		*/
/*		   RIJNDAEL by Joan Daemen and Vincent Rijmen					*/
/*																		*/
/* which is a candidate algorithm in the Advanced Encryption Standard	*/
/* programme of the US National Institute of Standards and Technology.  */
/*																		*/
/* Copyright in this implementation is held by Dr B R Gladman but I		*/
/* hereby give permission for its free direct or derivative use subject */
/* to acknowledgment of its origin and compliance with any conditions	*/
/* that the originators of the algorithm place on its exploitation.     */
/*																		*/
/* Dr Brian Gladman (gladman@seven77.demon.co.uk) 14th January 1999		*/

/* Timing data for Rijndael (rijndael.c)

Algorithm: rijndael (rijndael.c)

128 bit key:
Key Setup:	  305/1389 cycles (encrypt/decrypt)
Encrypt:	   374 cycles =    68.4 mbits/sec
Decrypt:	   352 cycles =    72.7 mbits/sec
Mean:		   363 cycles =    70.5 mbits/sec

192 bit key:
Key Setup:	  277/1595 cycles (encrypt/decrypt)
Encrypt:	   439 cycles =    58.3 mbits/sec
Decrypt:	   425 cycles =    60.2 mbits/sec
Mean:		   432 cycles =    59.3 mbits/sec

256 bit key:
Key Setup:	  374/1960 cycles (encrypt/decrypt)
Encrypt:	   502 cycles =    51.0 mbits/sec
Decrypt:	   498 cycles =    51.4 mbits/sec
Mean:		   500 cycles =    51.2 mbits/sec

*/

#include "postgres.h"

#include <sys/param.h>

#include "px.h"
#include "rijndael.h"

#define PRE_CALC_TABLES
#define LARGE_TABLES

static void gen_tabs(void);

/* 3. Basic macros for speeding up generic operations				*/

/* Circular rotate of 32 bit values									*/

#define rotr(x,n)	(((x) >> ((int)(n))) | ((x) << (32 - (int)(n))))
#define rotl(x,n)	(((x) << ((int)(n))) | ((x) >> (32 - (int)(n))))

/* Invert byte order in a 32 bit variable							*/

#define bswap(x)	((rotl((x), 8) & 0x00ff00ff) | (rotr((x), 8) & 0xff00ff00))

/* Extract byte from a 32 bit quantity (little endian notation)		*/

#define byte(x,n)	((u1byte)((x) >> (8 * (n))))

#ifdef WORDS_BIGENDIAN
#define io_swap(x)	bswap(x)
#else
#define io_swap(x)	(x)
#endif

#ifdef PRINT_TABS
#undef PRE_CALC_TABLES
#endif

#ifdef PRE_CALC_TABLES

#include "rijndael.tbl"
#define tab_gen		1
#else							/* !PRE_CALC_TABLES */

static u1byte pow_tab[256];
static u1byte log_tab[256];
static u1byte sbx_tab[256];
static u1byte isb_tab[256];
static u4byte rco_tab[10];
static u4byte ft_tab[4][256];
static u4byte it_tab[4][256];

#ifdef	LARGE_TABLES
static u4byte fl_tab[4][256];
static u4byte il_tab[4][256];
#endif

static u4byte tab_gen = 0;
#endif							/* !PRE_CALC_TABLES */

#define ff_mult(a,b)	((a) && (b) ? pow_tab[(log_tab[a] + log_tab[b]) % 255] : 0)

#define f_rn(bo, bi, n, k)								\
	(bo)[n] =  ft_tab[0][byte((bi)[n],0)] ^				\
			 ft_tab[1][byte((bi)[((n) + 1) & 3],1)] ^	\
			 ft_tab[2][byte((bi)[((n) + 2) & 3],2)] ^	\
			 ft_tab[3][byte((bi)[((n) + 3) & 3],3)] ^ *((k) + (n))

#define i_rn(bo, bi, n, k)							\
	(bo)[n] =  it_tab[0][byte((bi)[n],0)] ^				\
			 it_tab[1][byte((bi)[((n) + 3) & 3],1)] ^	\
			 it_tab[2][byte((bi)[((n) + 2) & 3],2)] ^	\
			 it_tab[3][byte((bi)[((n) + 1) & 3],3)] ^ *((k) + (n))

#ifdef LARGE_TABLES

#define ls_box(x)				 \
	( fl_tab[0][byte(x, 0)] ^	 \
	  fl_tab[1][byte(x, 1)] ^	 \
	  fl_tab[2][byte(x, 2)] ^	 \
	  fl_tab[3][byte(x, 3)] )

#define f_rl(bo, bi, n, k)								\
	(bo)[n] =  fl_tab[0][byte((bi)[n],0)] ^				\
			 fl_tab[1][byte((bi)[((n) + 1) & 3],1)] ^	\
			 fl_tab[2][byte((bi)[((n) + 2) & 3],2)] ^	\
			 fl_tab[3][byte((bi)[((n) + 3) & 3],3)] ^ *((k) + (n))

#define i_rl(bo, bi, n, k)								\
	(bo)[n] =  il_tab[0][byte((bi)[n],0)] ^				\
			 il_tab[1][byte((bi)[((n) + 3) & 3],1)] ^	\
			 il_tab[2][byte((bi)[((n) + 2) & 3],2)] ^	\
			 il_tab[3][byte((bi)[((n) + 1) & 3],3)] ^ *((k) + (n))
#else

#define ls_box(x)							 \
	((u4byte)sbx_tab[byte(x, 0)] <<  0) ^	 \
	((u4byte)sbx_tab[byte(x, 1)] <<  8) ^	 \
	((u4byte)sbx_tab[byte(x, 2)] << 16) ^	 \
	((u4byte)sbx_tab[byte(x, 3)] << 24)

#define f_rl(bo, bi, n, k)											\
	(bo)[n] = (u4byte)sbx_tab[byte((bi)[n],0)] ^					\
		rotl(((u4byte)sbx_tab[byte((bi)[((n) + 1) & 3],1)]),  8) ^	\
		rotl(((u4byte)sbx_tab[byte((bi)[((n) + 2) & 3],2)]), 16) ^	\
		rotl(((u4byte)sbx_tab[byte((bi)[((n) + 3) & 3],3)]), 24) ^ *((k) + (n))

#define i_rl(bo, bi, n, k)											\
	(bo)[n] = (u4byte)isb_tab[byte((bi)[n],0)] ^					\
		rotl(((u4byte)isb_tab[byte((bi)[((n) + 3) & 3],1)]),  8) ^	\
		rotl(((u4byte)isb_tab[byte((bi)[((n) + 2) & 3],2)]), 16) ^	\
		rotl(((u4byte)isb_tab[byte((bi)[((n) + 1) & 3],3)]), 24) ^ *((k) + (n))
#endif

static void
gen_tabs(void)
{
#ifndef PRE_CALC_TABLES
	u4byte		i,
				t;
	u1byte		p,
				q;

	/* log and power tables for GF(2**8) finite field with	*/
	/* 0x11b as modular polynomial - the simplest primitive	*/
	/* root is 0x11, used here to generate the tables		*/

	for (i = 0, p = 1; i < 256; ++i)
	{
		pow_tab[i] = (u1byte) p;
		log_tab[p] = (u1byte) i;

		p = p ^ (p << 1) ^ (p & 0x80 ? 0x01b : 0);
	}

	log_tab[1] = 0;
	p = 1;

	for (i = 0; i < 10; ++i)
	{
		rco_tab[i] = p;

		p = (p << 1) ^ (p & 0x80 ? 0x1b : 0);
	}

	/* note that the affine byte transformation matrix in	*/
	/* rijndael specification is in big endian format with	*/
	/* bit 0 as the most significant bit. In the remainder	*/
	/* of the specification the bits are numbered from the	*/
	/* least significant end of a byte.                     */

	for (i = 0; i < 256; ++i)
	{
		p = (i ? pow_tab[255 - log_tab[i]] : 0);
		q = p;
		q = (q >> 7) | (q << 1);
		p ^= q;
		q = (q >> 7) | (q << 1);
		p ^= q;
		q = (q >> 7) | (q << 1);
		p ^= q;
		q = (q >> 7) | (q << 1);
		p ^= q ^ 0x63;
		sbx_tab[i] = (u1byte) p;
		isb_tab[p] = (u1byte) i;
	}

	for (i = 0; i < 256; ++i)
	{
		p = sbx_tab[i];

#ifdef	LARGE_TABLES

		t = p;
		fl_tab[0][i] = t;
		fl_tab[1][i] = rotl(t, 8);
		fl_tab[2][i] = rotl(t, 16);
		fl_tab[3][i] = rotl(t, 24);
#endif
		t = ((u4byte) ff_mult(2, p)) |
			((u4byte) p << 8) |
			((u4byte) p << 16) |
			((u4byte) ff_mult(3, p) << 24);

		ft_tab[0][i] = t;
		ft_tab[1][i] = rotl(t, 8);
		ft_tab[2][i] = rotl(t, 16);
		ft_tab[3][i] = rotl(t, 24);

		p = isb_tab[i];

#ifdef	LARGE_TABLES

		t = p;
		il_tab[0][i] = t;
		il_tab[1][i] = rotl(t, 8);
		il_tab[2][i] = rotl(t, 16);
		il_tab[3][i] = rotl(t, 24);
#endif
		t = ((u4byte) ff_mult(14, p)) |
			((u4byte) ff_mult(9, p) << 8) |
			((u4byte) ff_mult(13, p) << 16) |
			((u4byte) ff_mult(11, p) << 24);

		it_tab[0][i] = t;
		it_tab[1][i] = rotl(t, 8);
		it_tab[2][i] = rotl(t, 16);
		it_tab[3][i] = rotl(t, 24);
	}

	tab_gen = 1;
#endif							/* !PRE_CALC_TABLES */
}


#define star_x(x) (((x) & 0x7f7f7f7f) << 1) ^ ((((x) & 0x80808080) >> 7) * 0x1b)

#define imix_col(y,x)		\
do { \
	u	= star_x(x);		\
	v	= star_x(u);		\
	w	= star_x(v);		\
	t	= w ^ (x);			\
   (y)	= u ^ v ^ w;		\
   (y) ^= rotr(u ^ t,  8) ^ \
		  rotr(v ^ t, 16) ^ \
		  rotr(t,24);		\
} while (0)

/* initialise the key schedule from the user supplied key	*/

#define loop4(i)									\
do {   t = ls_box(rotr(t,  8)) ^ rco_tab[i];		   \
	t ^= e_key[4 * i];	   e_key[4 * i + 4] = t;	\
	t ^= e_key[4 * i + 1]; e_key[4 * i + 5] = t;	\
	t ^= e_key[4 * i + 2]; e_key[4 * i + 6] = t;	\
	t ^= e_key[4 * i + 3]; e_key[4 * i + 7] = t;	\
} while (0)

#define loop6(i)									\
do {   t = ls_box(rotr(t,  8)) ^ rco_tab[i];		   \
	t ^= e_key[6 * (i)];	   e_key[6 * (i) + 6] = t;	\
	t ^= e_key[6 * (i) + 1]; e_key[6 * (i) + 7] = t;	\
	t ^= e_key[6 * (i) + 2]; e_key[6 * (i) + 8] = t;	\
	t ^= e_key[6 * (i) + 3]; e_key[6 * (i) + 9] = t;	\
	t ^= e_key[6 * (i) + 4]; e_key[6 * (i) + 10] = t;	\
	t ^= e_key[6 * (i) + 5]; e_key[6 * (i) + 11] = t;	\
} while (0)

#define loop8(i)									\
do {   t = ls_box(rotr(t,  8)) ^ rco_tab[i];		   \
	t ^= e_key[8 * (i)];	 e_key[8 * (i) + 8] = t;	\
	t ^= e_key[8 * (i) + 1]; e_key[8 * (i) + 9] = t;	\
	t ^= e_key[8 * (i) + 2]; e_key[8 * (i) + 10] = t;	\
	t ^= e_key[8 * (i) + 3]; e_key[8 * (i) + 11] = t;	\
	t  = e_key[8 * (i) + 4] ^ ls_box(t);				\
	e_key[8 * (i) + 12] = t;							\
	t ^= e_key[8 * (i) + 5]; e_key[8 * (i) + 13] = t;	\
	t ^= e_key[8 * (i) + 6]; e_key[8 * (i) + 14] = t;	\
	t ^= e_key[8 * (i) + 7]; e_key[8 * (i) + 15] = t;	\
} while (0)

rijndael_ctx *
rijndael_set_key(rijndael_ctx *ctx, const u4byte *in_key, const u4byte key_len,
				 int encrypt)
{
	u4byte		i,
				t,
				u,
				v,
				w;
	u4byte	   *e_key = ctx->e_key;
	u4byte	   *d_key = ctx->d_key;

	ctx->decrypt = !encrypt;

	if (!tab_gen)
		gen_tabs();

	ctx->k_len = (key_len + 31) / 32;

	e_key[0] = io_swap(in_key[0]);
	e_key[1] = io_swap(in_key[1]);
	e_key[2] = io_swap(in_key[2]);
	e_key[3] = io_swap(in_key[3]);

	switch (ctx->k_len)
	{
		case 4:
			t = e_key[3];
			for (i = 0; i < 10; ++i)
				loop4(i);
			break;

		case 6:
			e_key[4] = io_swap(in_key[4]);
			t = e_key[5] = io_swap(in_key[5]);
			for (i = 0; i < 8; ++i)
				loop6(i);
			break;

		case 8:
			e_key[4] = io_swap(in_key[4]);
			e_key[5] = io_swap(in_key[5]);
			e_key[6] = io_swap(in_key[6]);
			t = e_key[7] = io_swap(in_key[7]);
			for (i = 0; i < 7; ++i)
				loop8(i);
			break;
	}

	if (!encrypt)
	{
		d_key[0] = e_key[0];
		d_key[1] = e_key[1];
		d_key[2] = e_key[2];
		d_key[3] = e_key[3];

		for (i = 4; i < 4 * ctx->k_len + 24; ++i)
			imix_col(d_key[i], e_key[i]);
	}

	return ctx;
}

/* encrypt a block of text	*/

#define f_nround(bo, bi, k) \
do { \
	f_rn(bo, bi, 0, k);		\
	f_rn(bo, bi, 1, k);		\
	f_rn(bo, bi, 2, k);		\
	f_rn(bo, bi, 3, k);		\
	k += 4;					\
} while (0)

#define f_lround(bo, bi, k) \
do { \
	f_rl(bo, bi, 0, k);		\
	f_rl(bo, bi, 1, k);		\
	f_rl(bo, bi, 2, k);		\
	f_rl(bo, bi, 3, k);		\
} while (0)

void
rijndael_encrypt(rijndael_ctx *ctx, const u4byte *in_blk, u4byte *out_blk)
{
	u4byte		k_len = ctx->k_len;
	u4byte	   *e_key = ctx->e_key;
	u4byte		b0[4],
				b1[4],
			   *kp;

	b0[0] = io_swap(in_blk[0]) ^ e_key[0];
	b0[1] = io_swap(in_blk[1]) ^ e_key[1];
	b0[2] = io_swap(in_blk[2]) ^ e_key[2];
	b0[3] = io_swap(in_blk[3]) ^ e_key[3];

	kp = e_key + 4;

	if (k_len > 6)
	{
		f_nround(b1, b0, kp);
		f_nround(b0, b1, kp);
	}

	if (k_len > 4)
	{
		f_nround(b1, b0, kp);
		f_nround(b0, b1, kp);
	}

	f_nround(b1, b0, kp);
	f_nround(b0, b1, kp);
	f_nround(b1, b0, kp);
	f_nround(b0, b1, kp);
	f_nround(b1, b0, kp);
	f_nround(b0, b1, kp);
	f_nround(b1, b0, kp);
	f_nround(b0, b1, kp);
	f_nround(b1, b0, kp);
	f_lround(b0, b1, kp);

	out_blk[0] = io_swap(b0[0]);
	out_blk[1] = io_swap(b0[1]);
	out_blk[2] = io_swap(b0[2]);
	out_blk[3] = io_swap(b0[3]);
}

/* decrypt a block of text	*/

#define i_nround(bo, bi, k) \
do { \
	i_rn(bo, bi, 0, k);		\
	i_rn(bo, bi, 1, k);		\
	i_rn(bo, bi, 2, k);		\
	i_rn(bo, bi, 3, k);		\
	k -= 4;					\
} while (0)

#define i_lround(bo, bi, k) \
do { \
	i_rl(bo, bi, 0, k);		\
	i_rl(bo, bi, 1, k);		\
	i_rl(bo, bi, 2, k);		\
	i_rl(bo, bi, 3, k);		\
} while (0)

void
rijndael_decrypt(rijndael_ctx *ctx, const u4byte *in_blk, u4byte *out_blk)
{
	u4byte		b0[4],
				b1[4],
			   *kp;
	u4byte		k_len = ctx->k_len;
	u4byte	   *e_key = ctx->e_key;
	u4byte	   *d_key = ctx->d_key;

	b0[0] = io_swap(in_blk[0]) ^ e_key[4 * k_len + 24];
	b0[1] = io_swap(in_blk[1]) ^ e_key[4 * k_len + 25];
	b0[2] = io_swap(in_blk[2]) ^ e_key[4 * k_len + 26];
	b0[3] = io_swap(in_blk[3]) ^ e_key[4 * k_len + 27];

	kp = d_key + 4 * (k_len + 5);

	if (k_len > 6)
	{
		i_nround(b1, b0, kp);
		i_nround(b0, b1, kp);
	}

	if (k_len > 4)
	{
		i_nround(b1, b0, kp);
		i_nround(b0, b1, kp);
	}

	i_nround(b1, b0, kp);
	i_nround(b0, b1, kp);
	i_nround(b1, b0, kp);
	i_nround(b0, b1, kp);
	i_nround(b1, b0, kp);
	i_nround(b0, b1, kp);
	i_nround(b1, b0, kp);
	i_nround(b0, b1, kp);
	i_nround(b1, b0, kp);
	i_lround(b0, b1, kp);

	out_blk[0] = io_swap(b0[0]);
	out_blk[1] = io_swap(b0[1]);
	out_blk[2] = io_swap(b0[2]);
	out_blk[3] = io_swap(b0[3]);
}

/*
 * conventional interface
 *
 * ATM it hopes all data is 4-byte aligned - which
 * should be true for PX.  -marko
 */

void
aes_set_key(rijndael_ctx *ctx, const uint8 *key, unsigned keybits, int enc)
{
	uint32	   *k;

	k = (uint32 *) key;
	rijndael_set_key(ctx, k, keybits, enc);
}

void
aes_ecb_encrypt(rijndael_ctx *ctx, uint8 *data, unsigned len)
{
	unsigned	bs = 16;
	uint32	   *d;

	while (len >= bs)
	{
		d = (uint32 *) data;
		rijndael_encrypt(ctx, d, d);

		len -= bs;
		data += bs;
	}
}

void
aes_ecb_decrypt(rijndael_ctx *ctx, uint8 *data, unsigned len)
{
	unsigned	bs = 16;
	uint32	   *d;

	while (len >= bs)
	{
		d = (uint32 *) data;
		rijndael_decrypt(ctx, d, d);

		len -= bs;
		data += bs;
	}
}

void
aes_cbc_encrypt(rijndael_ctx *ctx, uint8 *iva, uint8 *data, unsigned len)
{
	uint32	   *iv = (uint32 *) iva;
	uint32	   *d = (uint32 *) data;
	unsigned	bs = 16;

	while (len >= bs)
	{
		d[0] ^= iv[0];
		d[1] ^= iv[1];
		d[2] ^= iv[2];
		d[3] ^= iv[3];

		rijndael_encrypt(ctx, d, d);

		iv = d;
		d += bs / 4;
		len -= bs;
	}
}

void
aes_cbc_decrypt(rijndael_ctx *ctx, uint8 *iva, uint8 *data, unsigned len)
{
	uint32	   *d = (uint32 *) data;
	unsigned	bs = 16;
	uint32		buf[4],
				iv[4];

	memcpy(iv, iva, bs);
	while (len >= bs)
	{
		buf[0] = d[0];
		buf[1] = d[1];
		buf[2] = d[2];
		buf[3] = d[3];

		rijndael_decrypt(ctx, buf, d);

		d[0] ^= iv[0];
		d[1] ^= iv[1];
		d[2] ^= iv[2];
		d[3] ^= iv[3];

		iv[0] = buf[0];
		iv[1] = buf[1];
		iv[2] = buf[2];
		iv[3] = buf[3];
		d += 4;
		len -= bs;
	}
}

/*
 * pre-calculate tables.
 *
 * On i386 lifts 17k from .bss to .rodata
 * and avoids 1k code and setup time.
 *	  -marko
 */
#ifdef PRINT_TABS

static void
show256u8(char *name, uint8 *data)
{
	int			i;

	printf("static const u1byte  %s[256] = {\n  ", name);
	for (i = 0; i < 256;)
	{
		printf("%u", pow_tab[i++]);
		if (i < 256)
			printf(i % 16 ? ", " : ",\n  ");
	}
	printf("\n};\n\n");
}


static void
show4x256u32(char *name, uint32 data[4][256])
{
	int			i,
				j;

	printf("static const u4byte  %s[4][256] = {\n{\n  ", name);
	for (i = 0; i < 4; i++)
	{
		for (j = 0; j < 256;)
		{
			printf("0x%08x", data[i][j]);
			j++;
			if (j < 256)
				printf(j % 4 ? ", " : ",\n  ");
		}
		printf(i < 3 ? "\n}, {\n  " : "\n}\n");
	}
	printf("};\n\n");
}

int
main()
{
	int			i;
	char	   *hdr = "/* Generated by rijndael.c */\n\n";

	gen_tabs();

	printf(hdr);
	show256u8("pow_tab", pow_tab);
	show256u8("log_tab", log_tab);
	show256u8("sbx_tab", sbx_tab);
	show256u8("isb_tab", isb_tab);

	show4x256u32("ft_tab", ft_tab);
	show4x256u32("it_tab", it_tab);
#ifdef LARGE_TABLES
	show4x256u32("fl_tab", fl_tab);
	show4x256u32("il_tab", il_tab);
#endif
	printf("static const u4byte rco_tab[10] = {\n  ");
	for (i = 0; i < 10; i++)
	{
		printf("0x%08x", rco_tab[i]);
		if (i < 9)
			printf(", ");
		if (i == 4)
			printf("\n  ");
	}
	printf("\n};\n\n");
	return 0;
}

#endif
