/*-------------------------------------------------------------------------
 *
 * varbit.c
 *	  Functions for the built-in type bit() and varying bit().
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/varbit.c,v 1.2 2000/04/12 17:15:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

/* Include file list stolen from float.c.
 * Can probably get rid of some of these.
 * - thomas 2000-04-07
 */
#include <ctype.h>
#include <errno.h>

#include <float.h>				/* faked on sunos4 */

#include <math.h>

#include "postgres.h"
#ifdef HAVE_LIMITS_H
#include <limits.h>
#ifndef MAXINT
#define MAXINT		  INT_MAX
#endif
#else
#ifdef HAVE_VALUES_H
#include <values.h>
#endif
#endif
#include "fmgr.h"
#include "utils/builtins.h"
#include "access/htup.h"

/*
   Prefixes:
	 zp    -- zero-padded fixed length bit string
	 var   -- varying bit string

   attypmod -- contains the length of the bit string in bits, or for
			   varying bits the maximum length.

   The data structure contains the following elements:
	  header  -- length of the whole data structure (incl header)
				 in bytes. (as with all varying length datatypes)
	  data section -- private data section for the bits data structures
		 bitlength -- lenght of the bit string in bits
	 bitdata   -- least significant byte first string
*/

char *
varbit_out(bits8 *s)
{
	return zpbits_out(s);
}

/*
 * zpbit_in -
 *	  converts a string to the internal representation of a bitstring.
 *		  The length is determined by the number of bits required plus
 *		  VARHDRSZ bytes or from atttypmod.
 *	  (XXX dummy is here because we pass typelem as the second argument
 *		  for array_in. copied this, no idea what it means??)
 */
bits8 *
zpbit_in(char *s, int dummy, int32 atttypmod)
{
	bits8	   *result;			/* the bits string that was read in   */
	char	   *sp;				/* pointer into the character string  */
	bits8	   *r;
	int			len,			/* Length of the whole data structure */
				bitlen,			/* Number of bits in the bit string   */
				slen;			/* Length of the input string		  */
	int			bit_not_hex = 0;/* 0 = hex string  1=bit string		  */
	int			bc,
				ipad;
	bits8		x = 0;


	if (s == NULL)
		return (bits8 *) NULL;

	/* Check that the first character is a b or an x */
	if (s[0] == 'b' || s[0] == 'B')
		bit_not_hex = 1;
	else if (s[0] == 'x' || s[0] == 'X')
		bit_not_hex = 0;
	else
		elog(ERROR, "zpbit_in: %s is not a valid bitstring", s);

	slen = strlen(s) - 1;
	/* Determine bitlength from input string */
	bitlen = slen;
	if (!bit_not_hex)
		bitlen *= 4;

	/*
	 * Sometimes atttypmod is not supplied. If it is supplied we need to
	 * make sure that the bitstring fits. Note that the number of infered
	 * bits can be larger than the number of actual bits needed, but only
	 * if we are reading a hex string and not by more than 3 bits, as a
	 * hex string gives and accurate length upto 4 bits
	 */
	if (atttypmod == -1)
		atttypmod = bitlen;
	else if ((bitlen > atttypmod && bit_not_hex) ||
			 (bitlen > atttypmod + 3 && !bit_not_hex))
		elog(ERROR, "zpbit_in: bit string of size %d cannot be written into bits(%d)",
			 bitlen, atttypmod);


	len = VARBITDATALEN(atttypmod);

	if (len > MaxAttrSize)
		elog(ERROR, "zpbit_in: length of bit() must be less than %ld",
			 (MaxAttrSize - VARHDRSZ - VARBITHDRSZ) * BITSPERBYTE);

	result = (bits8 *) palloc(len);
	/* set to 0 so that *r is always initialised and strin is zero-padded */
	memset(result, 0, len);
	VARSIZE(result) = len;
	VARBITLEN(result) = atttypmod;

	/*
	 * We need to read the bitstring from the end, as we store it least
	 * significant byte first. s points to the byte before the beginning
	 * of the bitstring
	 */
	sp = s + 1;
	r = VARBITS(result);
	if (bit_not_hex)
	{
		/* Parse the bit representation of the string */
		/* We know it fits, as bitlen was compared to atttypmod */
		x = BITHIGH;
		for (bc = 0; sp != s + slen + 1; sp++, bc++)
		{
			if (*sp == '1')
				*r |= x;
			if (bc == 7)
			{
				bc = 0;
				x = BITHIGH;
				r++;
			}
			else
				x >>= 1;
		}
	}
	else
	{
		/* Parse the hex representation of the string */
		for (bc = 0; sp != s + slen + 1; sp++)
		{
			if (*sp >= '0' && *sp <= '9')
				x = (bits8) (*sp - '0');
			else if (*sp >= 'A' && *sp <= 'F')
				x = (bits8) (*sp - 'A') + 10;
			else if (*sp >= 'a' && *sp <= 'f')
				x = (bits8) (*sp - 'a') + 10;
			else
				elog(ERROR, "Cannot parse %c as a hex digit", *sp);
			if (bc)
			{
				bc = 0;
				*r++ |= x;
			}
			else
			{
				bc++;
				*r = x << 4;
			}
		}
	}

	if (bitlen > atttypmod)
	{
		/* Check that this fitted */
		r = (bits8 *) (result + len - 1);
		ipad = VARBITPAD(result);

		/*
		 * The bottom ipad bits of the byte pointed to by r need to be
		 * zero
		 */
		if (((*r << (BITSPERBYTE - ipad)) & BITMASK) > 0)
			elog(ERROR, "zpbit_in: bit string too large for bit(%d) data type",
				 atttypmod);
	}

	return result;
}

/* zpbit_out -
 *	  for the time being we print everything as hex strings, as this is likely
 *	  to be more compact than bit strings, and consequently much more efficient
 *	  for long strings
 */
char *
zpbit_out(bits8 *s)
{
	char	   *result,
			   *r;
	bits8	   *sp;
	int			i,
				len,
				bitlen;

	if (s == NULL)
	{
		result = (char *) palloc(2);
		result[0] = '-';
		result[1] = '\0';
	}
	else
	{
		bitlen = VARBITLEN(s);
		len = bitlen / 4 + (bitlen % 4 > 0 ? 1 : 0);
		result = (char *) palloc(len + 4);
		sp = VARBITS(s);
		r = result;
		*r++ = 'X';
		*r++ = '\'';
		/* we cheat by knowing that we store full bytes zero padded */
		for (i = 0; i < len; i += 2, sp++)
		{
			*r++ = HEXDIG((*sp) >> 4);
			*r++ = HEXDIG((*sp) & 0xF);
		}

		/*
		 * Go back one step if we printed a hex number that was not part
		 * of the bitstring anymore
		 */
		if (i == len + 1)
			r--;
		*r++ = '\'';
		*r = '\0';
	}
	return result;
}

/* zpbits_out -
 *	  Prints the string a bits
 */
char *
zpbits_out(bits8 *s)
{
	char	   *result,
			   *r;
	bits8	   *sp;
	bits8		x;
	int			i,
				k,
				len;

	if (s == NULL)
	{
		result = (char *) palloc(2);
		result[0] = '-';
		result[1] = '\0';
	}
	else
	{
		len = VARBITLEN(s);
		result = (char *) palloc(len + 4);
		sp = VARBITS(s);
		r = result;
		*r++ = 'B';
		*r++ = '\'';
		for (i = 0; i < len - BITSPERBYTE; i += BITSPERBYTE, sp++)
		{
			x = *sp;
			for (k = 0; k < BITSPERBYTE; k++)
			{
				*r++ = (x & BITHIGH) ? '1' : '0';
				x <<= 1;
			}
		}
		x = *sp;
		for (k = i; k < len; k++)
		{
			*r++ = (x & BITHIGH) ? '1' : '0';
			x <<= 1;
		}
		*r++ = '\'';
		*r = '\0';
	}
	return result;
}


/*
 * varbit_in -
 *	  converts a string to the internal representation of a bitstring.
*/
bits8 *
varbit_in(char *s, int dummy, int32 atttypmod)
{
	bits8	   *result;			/* The resulting bit string			  */
	char	   *sp;				/* pointer into the character string  */
	bits8	   *r;
	int			len,			/* Length of the whole data structure */
				bitlen,			/* Number of bits in the bit string   */
				slen;			/* Length of the input string		  */
	int			bit_not_hex = 0;
	int			bc,
				ipad;
	bits8		x = 0;


	if (s == NULL)
		return (bits8 *) NULL;

	/* Check that the first character is a b or an x */
	if (s[0] == 'b' || s[0] == 'B')
		bit_not_hex = 1;
	else if (s[0] == 'x' || s[0] == 'X')
		bit_not_hex = 0;
	else
		elog(ERROR, "zpbit_in: %s is not a valid bitstring", s);

	slen = strlen(s) - 1;
	/* Determine bitlength from input string */
	bitlen = slen;
	if (!bit_not_hex)
		bitlen *= 4;

	/*
	 * Sometimes atttypmod is not supplied. If it is supplied we need to
	 * make sure that the bitstring fits. Note that the number of infered
	 * bits can be larger than the number of actual bits needed, but only
	 * if we are reading a hex string and not by more than 3 bits, as a
	 * hex string gives and accurate length upto 4 bits
	 */
	if (atttypmod > -1)
		if ((bitlen > atttypmod && bit_not_hex) ||
			(bitlen > atttypmod + 3 && !bit_not_hex))
			elog(ERROR, "varbit_in: bit string of size %d cannot be written into varying bits(%d)",
				 bitlen, atttypmod);


	len = VARBITDATALEN(bitlen);

	if (len > MaxAttrSize)
		elog(ERROR, "varbit_in: length of bit() must be less than %ld",
			 (MaxAttrSize - VARHDRSZ - VARBITHDRSZ) * BITSPERBYTE);

	result = (bits8 *) palloc(len);
	/* set to 0 so that *r is always initialised and strin is zero-padded */
	memset(result, 0, len);
	VARSIZE(result) = len;
	VARBITLEN(result) = bitlen;

	/*
	 * We need to read the bitstring from the end, as we store it least
	 * significant byte first. s points to the byte before the beginning
	 * of the bitstring
	 */
	sp = s + 1;
	r = VARBITS(result);
	if (bit_not_hex)
	{
		/* Parse the bit representation of the string */
		x = BITHIGH;
		for (bc = 0; sp != s + slen + 1; sp++, bc++)
		{
			if (*sp == '1')
				*r |= x;
			if (bc == 7)
			{
				bc = 0;
				x = BITHIGH;
				r++;
			}
			else
				x >>= 1;
		}
	}
	else
	{
		for (bc = 0; sp != s + slen + 1; sp++)
		{
			if (*sp >= '0' && *sp <= '9')
				x = (bits8) (*sp - '0');
			else if (*sp >= 'A' && *sp <= 'F')
				x = (bits8) (*sp - 'A') + 10;
			else if (*sp >= 'a' && *sp <= 'f')
				x = (bits8) (*sp - 'a') + 10;
			else
				elog(ERROR, "Cannot parse %c as a hex digit", *sp);
			if (bc)
			{
				bc = 0;
				*r++ |= x;
			}
			else
			{
				bc++;
				*r = x << 4;
			}
		}
	}

	if (bitlen > atttypmod)
	{
		/* Check that this fitted */
		r = (bits8 *) (result + len - 1);
		ipad = VARBITPAD(result);

		/*
		 * The bottom ipad bits of the byte pointed to by r need to be
		 * zero
		 */
		if (((*r << (BITSPERBYTE - ipad)) & BITMASK) > 0)
			elog(ERROR, "varbit_in: bit string too large for varying bit(%d) data type",
				 atttypmod);
	}

	return result;
}

/*
  the zpbit_out routines are fine for varying bits as well
*/


/*
 * Comparison operators
 *
 * We only need one set of comparison operators for bitstrings, as the lengths
 * are stored in the same way for zero-padded and varying bit strings.
 *
 * Note that the standard is not unambiguous about the comparison between
 * zero-padded bit strings and varying bitstrings. If the same value is written
 * into a zero padded bitstring as into a varying bitstring, but the zero
 * padded bitstring has greater length, it will be bigger.
 *
 * Zeros from the beginning of a bitstring cannot simply be ignored, as they
 * may be part of a bit string and may be significant.
 */

bool
biteq(bits8 *arg1, bits8 *arg2)
{
	int			bitlen1,
				bitlen2;

	if (!PointerIsValid(arg1) || !PointerIsValid(arg2))
		return (bool) 0;
	bitlen1 = VARBITLEN(arg1);
	bitlen2 = VARBITLEN(arg2);
	if (bitlen1 != bitlen2)
		return (bool) 0;

	/* bit strings are always stored in a full number of bytes */
	return memcmp((void *) VARBITS(arg1), (void *) VARBITS(arg2),
				  VARBITBYTES(arg1)) == 0;
}

bool
bitne(bits8 *arg1, bits8 *arg2)
{
	int			bitlen1,
				bitlen2;

	if (!PointerIsValid(arg1) || !PointerIsValid(arg2))
		return (bool) 0;
	bitlen1 = VARBITLEN(arg1);
	bitlen2 = VARBITLEN(arg2);
	if (bitlen1 != bitlen2)
		return (bool) 1;

	/* bit strings are always stored in a full number of bytes */
	return memcmp((void *) VARBITS(arg1), (void *) VARBITS(arg2),
				  VARBITBYTES(arg1)) != 0;
}

/* bitcmp
 *
 * Compares two bitstrings and returns -1, 0, 1 depending on whether the first
 * string is smaller, equal, or bigger than the second. All bits are considered
 * and additional zero bits may make one string smaller/larger than the other,
 * even if their zero-padded values would be the same.
 *	 Anything is equal to undefined.
 */
int
bitcmp(bits8 *arg1, bits8 *arg2)
{
	int			bitlen1,
				bytelen1,
				bitlen2,
				bytelen2;
	int			cmp;

	if (!PointerIsValid(arg1) || !PointerIsValid(arg2))
		return (bool) 0;
	bytelen1 = VARBITBYTES(arg1);
	bytelen2 = VARBITBYTES(arg2);

	cmp = memcmp(VARBITS(arg1), VARBITS(arg2), Min(bytelen1, bytelen2));
	if (cmp == 0)
	{
		bitlen1 = VARBITLEN(arg1);
		bitlen2 = VARBITLEN(arg2);
		if (bitlen1 != bitlen2)
			return bitlen1 < bitlen2 ? -1 : 1;
	}
	return cmp;
}

bool
bitlt(bits8 *arg1, bits8 *arg2)
{
	return (bool) (bitcmp(arg1, arg2) == -1);
}

bool
bitle(bits8 *arg1, bits8 *arg2)
{
	return (bool) (bitcmp(arg1, arg2) <= 0);
}

bool
bitge(bits8 *arg1, bits8 *arg2)
{
	return (bool) (bitcmp(arg1, arg2) >= 0);
}

bool
bitgt(bits8 *arg1, bits8 *arg2)
{
	return (bool) (bitcmp(arg1, arg2) == 1);
}

/* bitcat
 * Concatenation of bit strings
 */
bits8 *
bitcat(bits8 *arg1, bits8 *arg2)
{
	int			bitlen1,
				bitlen2,
				bytelen,
				bit1pad,
				bit2shift;
	bits8	   *result;
	bits8	   *pr,
			   *pa;

	if (!PointerIsValid(arg1) || !PointerIsValid(arg2))
		return NULL;

	bitlen1 = VARBITLEN(arg1);
	bitlen2 = VARBITLEN(arg2);

	bytelen = VARBITDATALEN(bitlen1 + bitlen2);

	result = (bits8 *) palloc(bytelen * sizeof(bits8));
	VARSIZE(result) = bytelen;
	VARBITLEN(result) = bitlen1 + bitlen2;
	/* Copy the first bitstring in */
	memcpy(VARBITS(result), VARBITS(arg1), VARBITBYTES(arg1));
	/* Copy the second bit string */
	bit1pad = VARBITPAD(arg1);
	if (bit1pad == 0)
	{
		memcpy(VARBITS(result) + VARBITBYTES(arg1), VARBITS(arg2),
			   VARBITBYTES(arg2));
	}
	else if (bitlen2 > 0)
	{
		/* We need to shift all the results to fit */
		bit2shift = BITSPERBYTE - bit1pad;
		pa = VARBITS(arg2);
		pr = VARBITS(result) + VARBITBYTES(arg1) - 1;
		for (; pa < VARBITEND(arg2); pa++)
		{
			*pr |= ((*pa >> bit2shift) & BITMASK);
			pr++;
			if (pr < VARBITEND(result))
				*pr = (*pa << bit1pad) & BITMASK;
		}
	}

	return result;
}

/* bitsubstr
 * retrieve a substring from the bit string.
 * Note, s is 1-based.
 * SQL draft 6.10 9)
 */
bits8 *
bitsubstr(bits8 *arg, int32 s, int32 l)
{
	int			bitlen,
				rbitlen,
				len,
				ipad = 0,
				ishift,
				i;
	int			e,
				s1,
				e1;
	bits8	   *result;
	bits8		mask,
			   *r,
			   *ps;

	if (!PointerIsValid(arg))
		return NULL;

	bitlen = VARBITLEN(arg);
	e = s + l;
	s1 = Max(s, 1);
	e1 = Min(e, bitlen + 1);
	if (s1 > bitlen || e1 < 1)
	{
		/* Need to return a null string */
		len = VARBITDATALEN(0);
		result = (bits8 *) palloc(len);
		VARBITLEN(result) = 0;
		VARSIZE(result) = len;
	}
	else
	{

		/*
		 * OK, we've got a true substring starting at position s1-1 and
		 * ending at position e1-1
		 */
		rbitlen = e1 - s1;
		len = VARBITDATALEN(rbitlen);
		result = (bits8 *) palloc(len);
		VARBITLEN(result) = rbitlen;
		VARSIZE(result) = len;
		len -= VARHDRSZ + VARBITHDRSZ;
		/* Are we copying from a byte boundary? */
		if ((s1 - 1) % BITSPERBYTE == 0)
		{
			/* Yep, we are copying bytes */
			memcpy(VARBITS(result), VARBITS(arg) + (s1 - 1) / BITSPERBYTE, len);
		}
		else
		{
			/* Figure out how much we need to shift the sequence by */
			ishift = (s1 - 1) % BITSPERBYTE;
			r = VARBITS(result);
			ps = VARBITS(arg) + (s1 - 1) / BITSPERBYTE;
			for (i = 0; i < len; i++)
			{
				*r = (*ps << ishift) & BITMASK;
				if ((++ps) < VARBITEND(arg))
					*r |= *ps >> (BITSPERBYTE - ishift);
				r++;
			}
		}
		/* Do we need to pad at the end? */
		ipad = VARBITPAD(result);
		if (ipad > 0)
		{
			mask = BITMASK << ipad;
			*(VARBITS(result) + len - 1) &= mask;
		}
	}

	return result;
}

/* bitand
 * perform a logical AND on two bit strings. The result is automatically
 * truncated to the shorter bit string
 */
bits8 *
bitand(bits8 *arg1, bits8 *arg2)
{
	int			len,
				i;
	bits8	   *result;
	bits8	   *p1,
			   *p2,
			   *r;

	if (!PointerIsValid(arg1) || !PointerIsValid(arg2))
		return (bool) 0;

	len = Min(VARSIZE(arg1), VARSIZE(arg2));
	result = (bits8 *) palloc(len);
	VARSIZE(result) = len;
	VARBITLEN(result) = Min(VARBITLEN(arg1), VARBITLEN(arg2));

	p1 = (bits8 *) VARBITS(arg1);
	p2 = (bits8 *) VARBITS(arg2);
	r = (bits8 *) VARBITS(result);
	for (i = 0; i < Min(VARBITBYTES(arg1), VARBITBYTES(arg2)); i++)
		*r++ = *p1++ & *p2++;

	/* Padding is not needed as & of 0 pad is 0 */

	return result;
}

/* bitor
 * perform a logical OR on two bit strings. The result is automatically
 * truncated to the shorter bit string.
 */
bits8 *
bitor(bits8 *arg1, bits8 *arg2)
{
	int			len,
				i;
	bits8	   *result;
	bits8	   *p1,
			   *p2,
			   *r;
	bits8		mask;

	if (!PointerIsValid(arg1) || !PointerIsValid(arg2))
		return (bool) 0;

	len = Min(VARSIZE(arg1), VARSIZE(arg2));
	result = (bits8 *) palloc(len);
	VARSIZE(result) = len;
	VARBITLEN(result) = Min(VARBITLEN(arg1), VARBITLEN(arg2));

	p1 = (bits8 *) VARBITS(arg1);
	p2 = (bits8 *) VARBITS(arg2);
	r = (bits8 *) VARBITS(result);
	for (i = 0; i < Min(VARBITBYTES(arg1), VARBITBYTES(arg2)); i++)
		*r++ = *p1++ | *p2++;

	/* Pad the result */
	mask = BITMASK << VARBITPAD(result);
	*r &= mask;

	return result;
}

/* bitxor
 * perform a logical XOR on two bit strings. The result is automatically
 * truncated to the shorter bit string.
 */
bits8 *
bitxor(bits8 *arg1, bits8 *arg2)
{
	int			len,
				i;
	bits8	   *result;
	bits8	   *p1,
			   *p2,
			   *r;
	bits8		mask;

	if (!PointerIsValid(arg1) || !PointerIsValid(arg2))
		return (bool) 0;

	len = Min(VARSIZE(arg1), VARSIZE(arg2));
	result = (bits8 *) palloc(len);
	VARSIZE(result) = len;
	VARBITLEN(result) = Min(VARBITLEN(arg1), VARBITLEN(arg2));

	p1 = (bits8 *) VARBITS(arg1);
	p2 = (bits8 *) VARBITS(arg2);
	r = (bits8 *) VARBITS(result);
	for (i = 0; i < Min(VARBITBYTES(arg1), VARBITBYTES(arg2)); i++)
		*r++ = *p1++ ^ *p2++;

	/* Pad the result */
	mask = BITMASK << VARBITPAD(result);
	*r &= mask;

	return result;
}

/* bitnot
 * perform a logical NOT on a bit strings.
 */
bits8 *
bitnot(bits8 *arg)
{
	bits8	   *result;
	bits8	   *p,
			   *r;
	bits8		mask;

	if (!PointerIsValid(arg))
		return (bool) 0;

	result = (bits8 *) palloc(VARSIZE(arg));
	VARSIZE(result) = VARSIZE(arg);
	VARBITLEN(result) = VARBITLEN(arg);

	p = (bits8 *) VARBITS(arg);
	r = (bits8 *) VARBITS(result);
	for (; p < VARBITEND(arg); p++, r++)
		*r = ~*p;

	/* Pad the result */
	mask = BITMASK << VARBITPAD(result);
	*r &= mask;

	return result;
}

/* bitshiftleft
 * do a left shift (i.e. to the beginning of the string) of the bit string
 */
bits8 *
bitshiftleft(bits8 *arg, int shft)
{
	int			byte_shift,
				ishift,
				len;
	bits8	   *result;
	bits8	   *p,
			   *r;

	if (!PointerIsValid(arg))
		return (bool) 0;

	/* Negative shift is a shift to the right */
	if (shft < 0)
		return bitshiftright(arg, -shft);

	result = (bits8 *) palloc(VARSIZE(arg));
	VARSIZE(result) = VARSIZE(arg);
	VARBITLEN(result) = VARBITLEN(arg);
	r = (bits8 *) VARBITS(result);

	byte_shift = shft / BITSPERBYTE;
	ishift = shft % BITSPERBYTE;
	p = ((bits8 *) VARBITS(arg)) + byte_shift;

	if (ishift == 0)
	{
		/* Special case: we can do a memcpy */
		len = VARBITBYTES(arg) - byte_shift;
		memcpy(r, p, len);
		memset(r + len, 0, byte_shift);
	}
	else
	{
		for (; p < VARBITEND(arg); r++)
		{
			*r = *p << ishift;
			if ((++p) < VARBITEND(arg))
				*r |= *p >> (BITSPERBYTE - ishift);
		}
		for (; r < VARBITEND(result); r++)
			*r = (bits8) 0;
	}

	return result;
}

/* bitshiftright
 * do a right shift (i.e. to the beginning of the string) of the bit string
 */
bits8 *
bitshiftright(bits8 *arg, int shft)
{
	int			byte_shift,
				ishift,
				len;
	bits8	   *result;
	bits8	   *p,
			   *r;

	if (!PointerIsValid(arg))
		return (bits8 *) 0;

	/* Negative shift is a shift to the left */
	if (shft < 0)
		return bitshiftleft(arg, -shft);

	result = (bits8 *) palloc(VARSIZE(arg));
	VARSIZE(result) = VARSIZE(arg);
	VARBITLEN(result) = VARBITLEN(arg);
	r = (bits8 *) VARBITS(result);

	byte_shift = shft / BITSPERBYTE;
	ishift = shft % BITSPERBYTE;
	p = (bits8 *) VARBITS(arg);

	/* Set the first part of the result to 0 */
	memset(r, 0, byte_shift);

	if (ishift == 0)
	{
		/* Special case: we can do a memcpy */
		len = VARBITBYTES(arg) - byte_shift;
		memcpy(r + byte_shift, p, len);
	}
	else
	{
		r += byte_shift;
		*r = 0;					/* Initialise first byte */
		for (; r < VARBITEND(result); p++)
		{
			*r |= *p >> ishift;
			if ((++r) < VARBITEND(result))
				*r = (*p << (BITSPERBYTE - ishift)) & BITMASK;
		}
	}

	return result;
}

bool
varbiteq(bits8 *arg1, bits8 *arg2)
{
	return biteq(arg1, arg2);
}

bool
varbitne(bits8 *arg1, bits8 *arg2)
{
	return bitne(arg1, arg2);
}

bool
varbitge(bits8 *arg1, bits8 *arg2)
{
	return bitge(arg1, arg2);
}

bool
varbitgt(bits8 *arg1, bits8 *arg2)
{
	return bitgt(arg1, arg2);
}

bool
varbitle(bits8 *arg1, bits8 *arg2)
{
	return bitle(arg1, arg2);
}

bool
varbitlt(bits8 *arg1, bits8 *arg2)
{
	return bitlt(arg1, arg2);
}

int
varbitcmp(bits8 *arg1, bits8 *arg2)
{
	return bitcmp(arg1, arg2);
}

bits8 *
varbitand(bits8 *arg1, bits8 *arg2)
{
	return bitand(arg1, arg2);
}

bits8 *
varbitor(bits8 *arg1, bits8 *arg2)
{
	return bitor(arg1, arg2);
}

bits8 *
varbitxor(bits8 *arg1, bits8 *arg2)
{
	return bitxor(arg1, arg2);
}

bits8 *
varbitnot(bits8 *arg)
{
	return bitnot(arg);
}

bits8 *
varbitshiftright(bits8 *arg, int shft)
{
	return bitshiftright(arg, shft);
}

bits8 *
varbitshiftleft(bits8 *arg, int shft)
{
	return bitshiftleft(arg, shft);
}

bits8 *
varbitcat(bits8 *arg1, bits8 *arg2)
{
	return bitcat(arg1, arg2);
}

bits8 *
varbitsubstr(bits8 *arg, int32 s, int32 l)
{
	return bitsubstr(arg, s, l);
}
