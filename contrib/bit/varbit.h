#include <stdlib.h>
#include <ctype.h>
#include <errno.h>

#include <float.h>				/* faked on sunos4 */

#include <math.h>

#include "postgres.h"
#ifdef HAVE_LIMITS_H
#include <limits.h>
#ifndef MAXINT
#define MAXINT			  INT_MAX
#endif
#else
#ifdef HAVE_VALUES_H
#include <values.h>
#endif
#endif
#include "fmgr.h"
#include "utils/timestamp.h"
#include "utils/builtins.h"


#define HEXDIG(z)	 (z)<10 ? ((z)+'0') : ((z)-10+'A')

/* Modeled on struct varlena from postgres.h, bu data type is bits8 */
struct varbita
{
	int32		vl_len;
	bits8		vl_dat[1];
};

#define BITSPERBYTE		8
#define VARBITHDRSZ		sizeof(int32)
/* Number of bits in this bit string */
#define VARBITLEN(PTR)		(((struct varbita *)VARDATA(PTR))->vl_len)
/* Pointer tp the first byte containing bit string data */
#define VARBITS(PTR)		(((struct varbita *)VARDATA(PTR))->vl_dat)
/* Number of bytes in the data section of a bit string */
#define VARBITBYTES(PTR)	(VARSIZE(PTR) - VARHDRSZ - VARBITHDRSZ)
/* Padding of the bit string at the end */
#define VARBITPAD(PTR)		(VARBITBYTES(PTR)*BITSPERBYTE - VARBITLEN(PTR))
/* Number of bytes needed to store a bit string of a given length */
#define VARBITDATALEN(BITLEN)	(BITLEN/BITSPERBYTE + \
				  (BITLEN%BITSPERBYTE > 0 ? 1 : 0) + \
					VARHDRSZ + VARBITHDRSZ)
/* pointer beyond the end of the bit string (like end() in STL containers) */
#define VARBITEND(PTR)		((bits8 *) (PTR + VARSIZE(PTR)))
/* Mask that will cover exactly one byte, i.e. BITSPERBYTE bits */
#define BITMASK			0xFF
#define BITHIGH					0x80


bits8	   *zpbitin(char *s, int dummy, int32 atttypmod);
char	   *zpbitout(bits8 *s);
char	   *zpbitsout(bits8 *s);
bits8	   *varbitin(char *s, int dummy, int32 atttypmod);
bool		biteq(bits8 *arg1, bits8 *arg2);
bool		bitne(bits8 *arg1, bits8 *arg2);
bool		bitge(bits8 *arg1, bits8 *arg2);
bool		bitgt(bits8 *arg1, bits8 *arg2);
bool		bitle(bits8 *arg1, bits8 *arg2);
bool		bitlt(bits8 *arg1, bits8 *arg2);
int			bitcmp(bits8 *arg1, bits8 *arg2);
bits8	   *bitand(bits8 *arg1, bits8 *arg2);
bits8	   *bitor(bits8 *arg1, bits8 *arg2);
bits8	   *bitxor(bits8 *arg1, bits8 *arg2);
bits8	   *bitnot(bits8 *arg);
bits8	   *bitshiftright(bits8 *arg, int shft);
bits8	   *bitshiftleft(bits8 *arg, int shft);
bits8	   *bitcat(bits8 *arg1, bits8 *arg2);
bits8	   *bitsubstr(bits8 *arg, int32 s, int32 l);
