#include "postgres.h"

typedef bits8  *VarBit;
typedef uint32 BitIndex;

#define HEXDIG(z)    (z)<10 ? ((z)+'0') : ((z)-10+'A')


#define BITSPERBYTE		8
#define VARBITHDRSZ		sizeof(int32)
/* Number of bits in this bit string */
#define VARBITLEN(PTR)		(((struct varlena *)VARDATA(PTR))->vl_len)
/* Pointer tp the first byte containing bit string data */
#define VARBITS(PTR)		(((struct varlena *)VARDATA(PTR))->vl_dat)
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
#define BITHIGH                 0x80


char * zpbitin(char *s, int dummy,  int32 atttypmod);
char * zpbitout(char *s);
char * zpbitsout(char *s);
char * varbitin(char *s, int dummy,  int32 atttypmod);
bool biteq (char *arg1, char *arg2);
bool bitne (char *arg1, char *arg2);
bool bitge (char *arg1, char *arg2);
bool bitgt (char *arg1, char *arg2);
bool bitle (char *arg1, char *arg2);
bool bitlt (char *arg1, char *arg2);
int bitcmp (char *arg1, char *arg2);
char * bitand (char * arg1, char * arg2);
char * bitor (char * arg1, char * arg2);
char * bitxor (char * arg1, char * arg2);
char * bitnot (char * arg);
char * bitshiftright (char * arg, int shft);
char * bitshiftleft (char * arg, int shft);
char * bitcat (char *arg1, char *arg2);
char * bitsubstr (char *arg, int32 s, int32 l);
