/* contrib/pgcrypto/blf.h */
/*
 * PuTTY is copyright 1997-2007 Simon Tatham.
 *
 * Portions copyright Robert de Bath, Joris van Rantwijk, Delian
 * Delchev, Andreas Schultz, Jeroen Massar, Wez Furlong, Nicolas Barry,
 * Justin Bradford, Ben Harris, Malcolm Smith, Ahmad Khalifa, Markus
 * Kuhn, and CORE SDI S.A.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE
 * FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

typedef struct
{
	uint32		S0[256],
				S1[256],
				S2[256],
				S3[256],
				P[18];
	uint32		iv0,
				iv1;			/* for CBC mode */
} BlowfishContext;

void		blowfish_setkey(BlowfishContext *ctx, const uint8 *key, short keybytes);
void		blowfish_setiv(BlowfishContext *ctx, const uint8 *iv);
void		blowfish_encrypt_cbc(uint8 *blk, int len, BlowfishContext *ctx);
void		blowfish_decrypt_cbc(uint8 *blk, int len, BlowfishContext *ctx);
void		blowfish_encrypt_ecb(uint8 *blk, int len, BlowfishContext *ctx);
void		blowfish_decrypt_ecb(uint8 *blk, int len, BlowfishContext *ctx);
