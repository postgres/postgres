/*-------------------------------------------------------------------------
 *
 * varbit.c
 *	  Functions for the SQL datatypes BIT() and BIT VARYING().
 *
 * Code originally contributed by Adriaan Joubert.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/varbit.c,v 1.50.2.1 2007/08/21 02:40:12 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/pqformat.h"
#include "utils/varbit.h"

#define HEXDIG(z)	 ((z)<10 ? ((z)+'0') : ((z)-10+'A'))


/*----------
 *	attypmod -- contains the length of the bit string in bits, or for
 *			   varying bits the maximum length.
 *
 *	The data structure contains the following elements:
 *	  header  -- length of the whole data structure (incl header)
 *				 in bytes. (as with all varying length datatypes)
 *	  data section -- private data section for the bits data structures
 *		bitlength -- length of the bit string in bits
 *		bitdata   -- bit string, most significant byte first
 *
 *	The length of the bitdata vector should always be exactly as many
 *	bytes as are needed for the given bitlength.  If the bitlength is
 *	not a multiple of 8, the extra low-order padding bits of the last
 *	byte must be zeroes.
 *----------
 */

/*
 * bit_in -
 *	  converts a char string to the internal representation of a bitstring.
 *		  The length is determined by the number of bits required plus
 *		  VARHDRSZ bytes or from atttypmod.
 */
Datum
bit_in(PG_FUNCTION_ARGS)
{
	char	   *input_string = PG_GETARG_CSTRING(0);

#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		atttypmod = PG_GETARG_INT32(2);
	VarBit	   *result;			/* The resulting bit string			  */
	char	   *sp;				/* pointer into the character string  */
	bits8	   *r;				/* pointer into the result */
	int			len,			/* Length of the whole data structure */
				bitlen,			/* Number of bits in the bit string   */
				slen;			/* Length of the input string		  */
	bool		bit_not_hex;	/* false = hex string  true = bit string */
	int			bc;
	bits8		x = 0;

	/* Check that the first character is a b or an x */
	if (input_string[0] == 'b' || input_string[0] == 'B')
	{
		bit_not_hex = true;
		sp = input_string + 1;
	}
	else if (input_string[0] == 'x' || input_string[0] == 'X')
	{
		bit_not_hex = false;
		sp = input_string + 1;
	}
	else
	{
		/*
		 * Otherwise it's binary.  This allows things like cast('1001' as bit)
		 * to work transparently.
		 */
		bit_not_hex = true;
		sp = input_string;
	}

	slen = strlen(sp);
	/* Determine bitlength from input string */
	if (bit_not_hex)
		bitlen = slen;
	else
		bitlen = slen * 4;

	/*
	 * Sometimes atttypmod is not supplied. If it is supplied we need to make
	 * sure that the bitstring fits.
	 */
	if (atttypmod <= 0)
		atttypmod = bitlen;
	else if (bitlen != atttypmod)
		ereport(ERROR,
				(errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
				 errmsg("bit string length %d does not match type bit(%d)",
						bitlen, atttypmod)));

	len = VARBITTOTALLEN(atttypmod);
	/* set to 0 so that *r is always initialised and string is zero-padded */
	result = (VarBit *) palloc0(len);
	VARATT_SIZEP(result) = len;
	VARBITLEN(result) = atttypmod;

	r = VARBITS(result);
	if (bit_not_hex)
	{
		/* Parse the bit representation of the string */
		/* We know it fits, as bitlen was compared to atttypmod */
		x = HIGHBIT;
		for (; *sp; sp++)
		{
			if (*sp == '1')
				*r |= x;
			else if (*sp != '0')
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("\"%c\" is not a valid binary digit",
								*sp)));

			x >>= 1;
			if (x == 0)
			{
				x = HIGHBIT;
				r++;
			}
		}
	}
	else
	{
		/* Parse the hex representation of the string */
		for (bc = 0; *sp; sp++)
		{
			if (*sp >= '0' && *sp <= '9')
				x = (bits8) (*sp - '0');
			else if (*sp >= 'A' && *sp <= 'F')
				x = (bits8) (*sp - 'A') + 10;
			else if (*sp >= 'a' && *sp <= 'f')
				x = (bits8) (*sp - 'a') + 10;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("\"%c\" is not a valid hexadecimal digit",
								*sp)));

			if (bc)
			{
				*r++ |= x;
				bc = 0;
			}
			else
			{
				*r = x << 4;
				bc = 1;
			}
		}
	}

	PG_RETURN_VARBIT_P(result);
}


Datum
bit_out(PG_FUNCTION_ARGS)
{
#if 1
	/* same as varbit output */
	return varbit_out(fcinfo);
#else
/* This is how one would print a hex string, in case someone wants to
   write a formatting function. */
	VarBit	   *s = PG_GETARG_VARBIT_P(0);
	char	   *result,
			   *r;
	bits8	   *sp;
	int			i,
				len,
				bitlen;

	bitlen = VARBITLEN(s);
	len = (bitlen + 3) / 4;
	result = (char *) palloc(len + 2);
	sp = VARBITS(s);
	r = result;
	*r++ = 'X';
	/* we cheat by knowing that we store full bytes zero padded */
	for (i = 0; i < len; i += 2, sp++)
	{
		*r++ = HEXDIG((*sp) >> 4);
		*r++ = HEXDIG((*sp) & 0xF);
	}

	/*
	 * Go back one step if we printed a hex number that was not part of the
	 * bitstring anymore
	 */
	if (i > len)
		r--;
	*r = '\0';

	PG_RETURN_CSTRING(result);
#endif
}

/*
 *		bit_recv			- converts external binary format to bit
 */
Datum
bit_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		atttypmod = PG_GETARG_INT32(2);
	VarBit	   *result;
	int			len,
				bitlen;
	int			ipad;
	bits8		mask;

	bitlen = pq_getmsgint(buf, sizeof(int32));
	if (bitlen < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid length in external bit string")));

	/*
	 * Sometimes atttypmod is not supplied. If it is supplied we need to make
	 * sure that the bitstring fits.
	 */
	if (atttypmod > 0 && bitlen != atttypmod)
		ereport(ERROR,
				(errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
				 errmsg("bit string length %d does not match type bit(%d)",
						bitlen, atttypmod)));

	len = VARBITTOTALLEN(bitlen);
	result = (VarBit *) palloc(len);
	VARATT_SIZEP(result) = len;
	VARBITLEN(result) = bitlen;

	pq_copymsgbytes(buf, (char *) VARBITS(result), VARBITBYTES(result));

	/* Make sure last byte is zero-padded if needed */
	ipad = VARBITPAD(result);
	if (ipad > 0)
	{
		mask = BITMASK << ipad;
		*(VARBITS(result) + VARBITBYTES(result) - 1) &= mask;
	}

	PG_RETURN_VARBIT_P(result);
}

/*
 *		bit_send			- converts bit to binary format
 */
Datum
bit_send(PG_FUNCTION_ARGS)
{
	/* Exactly the same as varbit_send, so share code */
	return varbit_send(fcinfo);
}

/* bit()
 * Converts a bit() type to a specific internal length.
 * len is the bitlength specified in the column definition.
 *
 * If doing implicit cast, raise error when source data is wrong length.
 * If doing explicit cast, silently truncate or zero-pad to specified length.
 */
Datum
bit(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);
	int32		len = PG_GETARG_INT32(1);
	bool		isExplicit = PG_GETARG_BOOL(2);
	VarBit	   *result;
	int			rlen;
	int			ipad;
	bits8		mask;

	/* No work if typmod is invalid or supplied data matches it already */
	if (len <= 0 || len == VARBITLEN(arg))
		PG_RETURN_VARBIT_P(arg);

	if (!isExplicit)
		ereport(ERROR,
				(errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
				 errmsg("bit string length %d does not match type bit(%d)",
						VARBITLEN(arg), len)));

	rlen = VARBITTOTALLEN(len);
	/* set to 0 so that string is zero-padded */
	result = (VarBit *) palloc0(rlen);
	VARATT_SIZEP(result) = rlen;
	VARBITLEN(result) = len;

	memcpy(VARBITS(result), VARBITS(arg),
		   Min(VARBITBYTES(result), VARBITBYTES(arg)));

	/*
	 * Make sure last byte is zero-padded if needed.  This is useless but safe
	 * if source data was shorter than target length (we assume the last byte
	 * of the source data was itself correctly zero-padded).
	 */
	ipad = VARBITPAD(result);
	if (ipad > 0)
	{
		mask = BITMASK << ipad;
		*(VARBITS(result) + VARBITBYTES(result) - 1) &= mask;
	}

	PG_RETURN_VARBIT_P(result);
}

/*
 * varbit_in -
 *	  converts a string to the internal representation of a bitstring.
 *		This is the same as bit_in except that atttypmod is taken as
 *		the maximum length, not the exact length to force the bitstring to.
 */
Datum
varbit_in(PG_FUNCTION_ARGS)
{
	char	   *input_string = PG_GETARG_CSTRING(0);

#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		atttypmod = PG_GETARG_INT32(2);
	VarBit	   *result;			/* The resulting bit string			  */
	char	   *sp;				/* pointer into the character string  */
	bits8	   *r;				/* pointer into the result */
	int			len,			/* Length of the whole data structure */
				bitlen,			/* Number of bits in the bit string   */
				slen;			/* Length of the input string		  */
	bool		bit_not_hex;	/* false = hex string  true = bit string */
	int			bc;
	bits8		x = 0;

	/* Check that the first character is a b or an x */
	if (input_string[0] == 'b' || input_string[0] == 'B')
	{
		bit_not_hex = true;
		sp = input_string + 1;
	}
	else if (input_string[0] == 'x' || input_string[0] == 'X')
	{
		bit_not_hex = false;
		sp = input_string + 1;
	}
	else
	{
		bit_not_hex = true;
		sp = input_string;
	}

	slen = strlen(sp);
	/* Determine bitlength from input string */
	if (bit_not_hex)
		bitlen = slen;
	else
		bitlen = slen * 4;

	/*
	 * Sometimes atttypmod is not supplied. If it is supplied we need to make
	 * sure that the bitstring fits.
	 */
	if (atttypmod <= 0)
		atttypmod = bitlen;
	else if (bitlen > atttypmod)
		ereport(ERROR,
				(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
				 errmsg("bit string too long for type bit varying(%d)",
						atttypmod)));

	len = VARBITTOTALLEN(bitlen);
	/* set to 0 so that *r is always initialised and string is zero-padded */
	result = (VarBit *) palloc0(len);
	VARATT_SIZEP(result) = len;
	VARBITLEN(result) = Min(bitlen, atttypmod);

	r = VARBITS(result);
	if (bit_not_hex)
	{
		/* Parse the bit representation of the string */
		/* We know it fits, as bitlen was compared to atttypmod */
		x = HIGHBIT;
		for (; *sp; sp++)
		{
			if (*sp == '1')
				*r |= x;
			else if (*sp != '0')
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("\"%c\" is not a valid binary digit",
								*sp)));

			x >>= 1;
			if (x == 0)
			{
				x = HIGHBIT;
				r++;
			}
		}
	}
	else
	{
		/* Parse the hex representation of the string */
		for (bc = 0; *sp; sp++)
		{
			if (*sp >= '0' && *sp <= '9')
				x = (bits8) (*sp - '0');
			else if (*sp >= 'A' && *sp <= 'F')
				x = (bits8) (*sp - 'A') + 10;
			else if (*sp >= 'a' && *sp <= 'f')
				x = (bits8) (*sp - 'a') + 10;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("\"%c\" is not a valid hexadecimal digit",
								*sp)));

			if (bc)
			{
				*r++ |= x;
				bc = 0;
			}
			else
			{
				*r = x << 4;
				bc = 1;
			}
		}
	}

	PG_RETURN_VARBIT_P(result);
}

/* varbit_out -
 *	  Prints the string as bits to preserve length accurately
 */
Datum
varbit_out(PG_FUNCTION_ARGS)
{
	VarBit	   *s = PG_GETARG_VARBIT_P(0);
	char	   *result,
			   *r;
	bits8	   *sp;
	bits8		x;
	int			i,
				k,
				len;

	len = VARBITLEN(s);
	result = (char *) palloc(len + 1);
	sp = VARBITS(s);
	r = result;
	for (i = 0; i <= len - BITS_PER_BYTE; i += BITS_PER_BYTE, sp++)
	{
		/* print full bytes */
		x = *sp;
		for (k = 0; k < BITS_PER_BYTE; k++)
		{
			*r++ = IS_HIGHBIT_SET(x) ? '1' : '0';
			x <<= 1;
		}
	}
	if (i < len)
	{
		/* print the last partial byte */
		x = *sp;
		for (k = i; k < len; k++)
		{
			*r++ = IS_HIGHBIT_SET(x) ? '1' : '0';
			x <<= 1;
		}
	}
	*r = '\0';

	PG_RETURN_CSTRING(result);
}

/*
 *		varbit_recv			- converts external binary format to varbit
 *
 * External format is the bitlen as an int32, then the byte array.
 */
Datum
varbit_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		atttypmod = PG_GETARG_INT32(2);
	VarBit	   *result;
	int			len,
				bitlen;
	int			ipad;
	bits8		mask;

	bitlen = pq_getmsgint(buf, sizeof(int32));
	if (bitlen < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid length in external bit string")));

	/*
	 * Sometimes atttypmod is not supplied. If it is supplied we need to make
	 * sure that the bitstring fits.
	 */
	if (atttypmod > 0 && bitlen > atttypmod)
		ereport(ERROR,
				(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
				 errmsg("bit string too long for type bit varying(%d)",
						atttypmod)));

	len = VARBITTOTALLEN(bitlen);
	result = (VarBit *) palloc(len);
	VARATT_SIZEP(result) = len;
	VARBITLEN(result) = bitlen;

	pq_copymsgbytes(buf, (char *) VARBITS(result), VARBITBYTES(result));

	/* Make sure last byte is zero-padded if needed */
	ipad = VARBITPAD(result);
	if (ipad > 0)
	{
		mask = BITMASK << ipad;
		*(VARBITS(result) + VARBITBYTES(result) - 1) &= mask;
	}

	PG_RETURN_VARBIT_P(result);
}

/*
 *		varbit_send			- converts varbit to binary format
 */
Datum
varbit_send(PG_FUNCTION_ARGS)
{
	VarBit	   *s = PG_GETARG_VARBIT_P(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint(&buf, VARBITLEN(s), sizeof(int32));
	pq_sendbytes(&buf, (char *) VARBITS(s), VARBITBYTES(s));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/* varbit()
 * Converts a varbit() type to a specific internal length.
 * len is the maximum bitlength specified in the column definition.
 *
 * If doing implicit cast, raise error when source data is too long.
 * If doing explicit cast, silently truncate to max length.
 */
Datum
varbit(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);
	int32		len = PG_GETARG_INT32(1);
	bool		isExplicit = PG_GETARG_BOOL(2);
	VarBit	   *result;
	int			rlen;
	int			ipad;
	bits8		mask;

	/* No work if typmod is invalid or supplied data matches it already */
	if (len <= 0 || len >= VARBITLEN(arg))
		PG_RETURN_VARBIT_P(arg);

	if (!isExplicit)
		ereport(ERROR,
				(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
				 errmsg("bit string too long for type bit varying(%d)",
						len)));

	rlen = VARBITTOTALLEN(len);
	result = (VarBit *) palloc(rlen);
	VARATT_SIZEP(result) = rlen;
	VARBITLEN(result) = len;

	memcpy(VARBITS(result), VARBITS(arg), VARBITBYTES(result));

	/* Make sure last byte is zero-padded if needed */
	ipad = VARBITPAD(result);
	if (ipad > 0)
	{
		mask = BITMASK << ipad;
		*(VARBITS(result) + VARBITBYTES(result) - 1) &= mask;
	}

	PG_RETURN_VARBIT_P(result);
}


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
 *
 * Note: btree indexes need these routines not to leak memory; therefore,
 * be careful to free working copies of toasted datums.  Most places don't
 * need to be so careful.
 */

/* bit_cmp
 *
 * Compares two bitstrings and returns <0, 0, >0 depending on whether the first
 * string is smaller, equal, or bigger than the second. All bits are considered
 * and additional zero bits may make one string smaller/larger than the other,
 * even if their zero-padded values would be the same.
 */
static int32
bit_cmp(VarBit *arg1, VarBit *arg2)
{
	int			bitlen1,
				bytelen1,
				bitlen2,
				bytelen2;
	int32		cmp;

	bytelen1 = VARBITBYTES(arg1);
	bytelen2 = VARBITBYTES(arg2);

	cmp = memcmp(VARBITS(arg1), VARBITS(arg2), Min(bytelen1, bytelen2));
	if (cmp == 0)
	{
		bitlen1 = VARBITLEN(arg1);
		bitlen2 = VARBITLEN(arg2);
		if (bitlen1 != bitlen2)
			cmp = (bitlen1 < bitlen2) ? -1 : 1;
	}
	return cmp;
}

Datum
biteq(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *arg2 = PG_GETARG_VARBIT_P(1);
	bool		result;
	int			bitlen1,
				bitlen2;

	bitlen1 = VARBITLEN(arg1);
	bitlen2 = VARBITLEN(arg2);

	/* fast path for different-length inputs */
	if (bitlen1 != bitlen2)
		result = false;
	else
		result = (bit_cmp(arg1, arg2) == 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
bitne(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *arg2 = PG_GETARG_VARBIT_P(1);
	bool		result;
	int			bitlen1,
				bitlen2;

	bitlen1 = VARBITLEN(arg1);
	bitlen2 = VARBITLEN(arg2);

	/* fast path for different-length inputs */
	if (bitlen1 != bitlen2)
		result = true;
	else
		result = (bit_cmp(arg1, arg2) != 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
bitlt(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *arg2 = PG_GETARG_VARBIT_P(1);
	bool		result;

	result = (bit_cmp(arg1, arg2) < 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
bitle(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *arg2 = PG_GETARG_VARBIT_P(1);
	bool		result;

	result = (bit_cmp(arg1, arg2) <= 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
bitgt(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *arg2 = PG_GETARG_VARBIT_P(1);
	bool		result;

	result = (bit_cmp(arg1, arg2) > 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
bitge(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *arg2 = PG_GETARG_VARBIT_P(1);
	bool		result;

	result = (bit_cmp(arg1, arg2) >= 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
bitcmp(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *arg2 = PG_GETARG_VARBIT_P(1);
	int32		result;

	result = bit_cmp(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_INT32(result);
}

/* bitcat
 * Concatenation of bit strings
 */
Datum
bitcat(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *arg2 = PG_GETARG_VARBIT_P(1);
	VarBit	   *result;
	int			bitlen1,
				bitlen2,
				bytelen,
				bit1pad,
				bit2shift;
	bits8	   *pr,
			   *pa;

	bitlen1 = VARBITLEN(arg1);
	bitlen2 = VARBITLEN(arg2);

	bytelen = VARBITTOTALLEN(bitlen1 + bitlen2);

	result = (VarBit *) palloc(bytelen);
	VARATT_SIZEP(result) = bytelen;
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
		/* We need to shift all the bits to fit */
		bit2shift = BITS_PER_BYTE - bit1pad;
		pr = VARBITS(result) + VARBITBYTES(arg1) - 1;
		for (pa = VARBITS(arg2); pa < VARBITEND(arg2); pa++)
		{
			*pr |= ((*pa >> bit2shift) & BITMASK);
			pr++;
			if (pr < VARBITEND(result))
				*pr = (*pa << bit1pad) & BITMASK;
		}
	}

	PG_RETURN_VARBIT_P(result);
}

/* bitsubstr
 * retrieve a substring from the bit string.
 * Note, s is 1-based.
 * SQL draft 6.10 9)
 */
Datum
bitsubstr(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);
	int32		s = PG_GETARG_INT32(1);
	int32		l = PG_GETARG_INT32(2);
	VarBit	   *result;
	int			bitlen,
				rbitlen,
				len,
				ipad = 0,
				ishift,
				i;
	int			e,
				s1,
				e1;
	bits8		mask,
			   *r,
			   *ps;

	bitlen = VARBITLEN(arg);
	/* If we do not have an upper bound, set bitlen */
	if (l == -1)
		l = bitlen;
	e = s + l;
	s1 = Max(s, 1);
	e1 = Min(e, bitlen + 1);
	if (s1 > bitlen || e1 < 1)
	{
		/* Need to return a zero-length bitstring */
		len = VARBITTOTALLEN(0);
		result = (VarBit *) palloc(len);
		VARATT_SIZEP(result) = len;
		VARBITLEN(result) = 0;
	}
	else
	{
		/*
		 * OK, we've got a true substring starting at position s1-1 and ending
		 * at position e1-1
		 */
		rbitlen = e1 - s1;
		len = VARBITTOTALLEN(rbitlen);
		result = (VarBit *) palloc(len);
		VARATT_SIZEP(result) = len;
		VARBITLEN(result) = rbitlen;
		len -= VARHDRSZ + VARBITHDRSZ;
		/* Are we copying from a byte boundary? */
		if ((s1 - 1) % BITS_PER_BYTE == 0)
		{
			/* Yep, we are copying bytes */
			memcpy(VARBITS(result), VARBITS(arg) + (s1 - 1) / BITS_PER_BYTE,
				   len);
		}
		else
		{
			/* Figure out how much we need to shift the sequence by */
			ishift = (s1 - 1) % BITS_PER_BYTE;
			r = VARBITS(result);
			ps = VARBITS(arg) + (s1 - 1) / BITS_PER_BYTE;
			for (i = 0; i < len; i++)
			{
				*r = (*ps << ishift) & BITMASK;
				if ((++ps) < VARBITEND(arg))
					*r |= *ps >> (BITS_PER_BYTE - ishift);
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

	PG_RETURN_VARBIT_P(result);
}

/* bitlength, bitoctetlength
 * Return the length of a bit string
 */
Datum
bitlength(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);

	PG_RETURN_INT32(VARBITLEN(arg));
}

Datum
bitoctetlength(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);

	PG_RETURN_INT32(VARBITBYTES(arg));
}

/* bitand
 * perform a logical AND on two bit strings.
 */
Datum
bitand(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *arg2 = PG_GETARG_VARBIT_P(1);
	VarBit	   *result;
	int			len,
				bitlen1,
				bitlen2,
				i;
	bits8	   *p1,
			   *p2,
			   *r;

	bitlen1 = VARBITLEN(arg1);
	bitlen2 = VARBITLEN(arg2);
	if (bitlen1 != bitlen2)
		ereport(ERROR,
				(errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
				 errmsg("cannot AND bit strings of different sizes")));

	len = VARSIZE(arg1);
	result = (VarBit *) palloc(len);
	VARATT_SIZEP(result) = len;
	VARBITLEN(result) = bitlen1;

	p1 = VARBITS(arg1);
	p2 = VARBITS(arg2);
	r = VARBITS(result);
	for (i = 0; i < VARBITBYTES(arg1); i++)
		*r++ = *p1++ & *p2++;

	/* Padding is not needed as & of 0 pad is 0 */

	PG_RETURN_VARBIT_P(result);
}

/* bitor
 * perform a logical OR on two bit strings.
 */
Datum
bitor(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *arg2 = PG_GETARG_VARBIT_P(1);
	VarBit	   *result;
	int			len,
				bitlen1,
				bitlen2,
				i;
	bits8	   *p1,
			   *p2,
			   *r;
	bits8		mask;

	bitlen1 = VARBITLEN(arg1);
	bitlen2 = VARBITLEN(arg2);
	if (bitlen1 != bitlen2)
		ereport(ERROR,
				(errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
				 errmsg("cannot OR bit strings of different sizes")));
	len = VARSIZE(arg1);
	result = (VarBit *) palloc(len);
	VARATT_SIZEP(result) = len;
	VARBITLEN(result) = bitlen1;

	p1 = VARBITS(arg1);
	p2 = VARBITS(arg2);
	r = VARBITS(result);
	for (i = 0; i < VARBITBYTES(arg1); i++)
		*r++ = *p1++ | *p2++;

	/* Pad the result */
	mask = BITMASK << VARBITPAD(result);
	if (mask)
	{
		r--;
		*r &= mask;
	}

	PG_RETURN_VARBIT_P(result);
}

/* bitxor
 * perform a logical XOR on two bit strings.
 */
Datum
bitxor(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *arg2 = PG_GETARG_VARBIT_P(1);
	VarBit	   *result;
	int			len,
				bitlen1,
				bitlen2,
				i;
	bits8	   *p1,
			   *p2,
			   *r;
	bits8		mask;

	bitlen1 = VARBITLEN(arg1);
	bitlen2 = VARBITLEN(arg2);
	if (bitlen1 != bitlen2)
		ereport(ERROR,
				(errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
				 errmsg("cannot XOR bit strings of different sizes")));

	len = VARSIZE(arg1);
	result = (VarBit *) palloc(len);
	VARATT_SIZEP(result) = len;
	VARBITLEN(result) = bitlen1;

	p1 = VARBITS(arg1);
	p2 = VARBITS(arg2);
	r = VARBITS(result);
	for (i = 0; i < VARBITBYTES(arg1); i++)
		*r++ = *p1++ ^ *p2++;

	/* Pad the result */
	mask = BITMASK << VARBITPAD(result);
	if (mask)
	{
		r--;
		*r &= mask;
	}

	PG_RETURN_VARBIT_P(result);
}

/* bitnot
 * perform a logical NOT on a bit string.
 */
Datum
bitnot(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);
	VarBit	   *result;
	bits8	   *p,
			   *r;
	bits8		mask;

	result = (VarBit *) palloc(VARSIZE(arg));
	VARATT_SIZEP(result) = VARSIZE(arg);
	VARBITLEN(result) = VARBITLEN(arg);

	p = VARBITS(arg);
	r = VARBITS(result);
	for (; p < VARBITEND(arg); p++)
		*r++ = ~*p;

	/* Pad the result */
	mask = BITMASK << VARBITPAD(result);
	if (mask)
	{
		r--;
		*r &= mask;
	}

	PG_RETURN_VARBIT_P(result);
}

/* bitshiftleft
 * do a left shift (i.e. towards the beginning of the string)
 */
Datum
bitshiftleft(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);
	int32		shft = PG_GETARG_INT32(1);
	VarBit	   *result;
	int			byte_shift,
				ishift,
				len;
	bits8	   *p,
			   *r;

	/* Negative shift is a shift to the right */
	if (shft < 0)
		PG_RETURN_DATUM(DirectFunctionCall2(bitshiftright,
											VarBitPGetDatum(arg),
											Int32GetDatum(-shft)));

	result = (VarBit *) palloc(VARSIZE(arg));
	VARATT_SIZEP(result) = VARSIZE(arg);
	VARBITLEN(result) = VARBITLEN(arg);
	r = VARBITS(result);

	/* If we shifted all the bits out, return an all-zero string */
	if (shft >= VARBITLEN(arg))
	{
		MemSet(r, 0, VARBITBYTES(arg));
		PG_RETURN_VARBIT_P(result);
	}

	byte_shift = shft / BITS_PER_BYTE;
	ishift = shft % BITS_PER_BYTE;
	p = VARBITS(arg) + byte_shift;

	if (ishift == 0)
	{
		/* Special case: we can do a memcpy */
		len = VARBITBYTES(arg) - byte_shift;
		memcpy(r, p, len);
		MemSet(r + len, 0, byte_shift);
	}
	else
	{
		for (; p < VARBITEND(arg); r++)
		{
			*r = *p << ishift;
			if ((++p) < VARBITEND(arg))
				*r |= *p >> (BITS_PER_BYTE - ishift);
		}
		for (; r < VARBITEND(result); r++)
			*r = 0;
	}

	PG_RETURN_VARBIT_P(result);
}

/* bitshiftright
 * do a right shift (i.e. towards the end of the string)
 */
Datum
bitshiftright(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);
	int32		shft = PG_GETARG_INT32(1);
	VarBit	   *result;
	int			byte_shift,
				ishift,
				len;
	bits8	   *p,
			   *r;

	/* Negative shift is a shift to the left */
	if (shft < 0)
		PG_RETURN_DATUM(DirectFunctionCall2(bitshiftleft,
											VarBitPGetDatum(arg),
											Int32GetDatum(-shft)));

	result = (VarBit *) palloc(VARSIZE(arg));
	VARATT_SIZEP(result) = VARSIZE(arg);
	VARBITLEN(result) = VARBITLEN(arg);
	r = VARBITS(result);

	/* If we shifted all the bits out, return an all-zero string */
	if (shft >= VARBITLEN(arg))
	{
		MemSet(r, 0, VARBITBYTES(arg));
		PG_RETURN_VARBIT_P(result);
	}

	byte_shift = shft / BITS_PER_BYTE;
	ishift = shft % BITS_PER_BYTE;
	p = VARBITS(arg);

	/* Set the first part of the result to 0 */
	MemSet(r, 0, byte_shift);
	r += byte_shift;

	if (ishift == 0)
	{
		/* Special case: we can do a memcpy */
		len = VARBITBYTES(arg) - byte_shift;
		memcpy(r, p, len);
	}
	else
	{
		if (r < VARBITEND(result))
			*r = 0;				/* initialize first byte */
		for (; r < VARBITEND(result); p++)
		{
			*r |= *p >> ishift;
			if ((++r) < VARBITEND(result))
				*r = (*p << (BITS_PER_BYTE - ishift)) & BITMASK;
		}
	}

	PG_RETURN_VARBIT_P(result);
}

/*
 * This is not defined in any standard. We retain the natural ordering of
 * bits here, as it just seems more intuitive.
 */
Datum
bitfromint4(PG_FUNCTION_ARGS)
{
	int32		a = PG_GETARG_INT32(0);
	int32		typmod = PG_GETARG_INT32(1);
	VarBit	   *result;
	bits8	   *r;
	int			rlen;
	int			destbitsleft,
				srcbitsleft;

	if (typmod <= 0)
		typmod = 1;				/* default bit length */

	rlen = VARBITTOTALLEN(typmod);
	result = (VarBit *) palloc(rlen);
	VARATT_SIZEP(result) = rlen;
	VARBITLEN(result) = typmod;

	r = VARBITS(result);
	destbitsleft = typmod;
	srcbitsleft = 32;
	/* drop any input bits that don't fit */
	srcbitsleft = Min(srcbitsleft, destbitsleft);
	/* sign-fill any excess bytes in output */
	while (destbitsleft >= srcbitsleft + 8)
	{
		*r++ = (bits8) ((a < 0) ? BITMASK : 0);
		destbitsleft -= 8;
	}
	/* store first fractional byte */
	if (destbitsleft > srcbitsleft)
	{
		*r++ = (bits8) ((a >> (srcbitsleft - 8)) & BITMASK);
		destbitsleft -= 8;
	}
	/* Now srcbitsleft and destbitsleft are the same, need not track both */
	/* store whole bytes */
	while (destbitsleft >= 8)
	{
		*r++ = (bits8) ((a >> (destbitsleft - 8)) & BITMASK);
		destbitsleft -= 8;
	}
	/* store last fractional byte */
	if (destbitsleft > 0)
		*r = (bits8) ((a << (8 - destbitsleft)) & BITMASK);

	PG_RETURN_VARBIT_P(result);
}

Datum
bittoint4(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);
	uint32		result;
	bits8	   *r;

	/* Check that the bit string is not too long */
	if (VARBITLEN(arg) > sizeof(result) * BITS_PER_BYTE)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	result = 0;
	for (r = VARBITS(arg); r < VARBITEND(arg); r++)
	{
		result <<= BITS_PER_BYTE;
		result |= *r;
	}
	/* Now shift the result to take account of the padding at the end */
	result >>= VARBITPAD(arg);

	PG_RETURN_INT32(result);
}

Datum
bitfromint8(PG_FUNCTION_ARGS)
{
	int64		a = PG_GETARG_INT64(0);
	int32		typmod = PG_GETARG_INT32(1);
	VarBit	   *result;
	bits8	   *r;
	int			rlen;
	int			destbitsleft,
				srcbitsleft;

	if (typmod <= 0)
		typmod = 1;				/* default bit length */

	rlen = VARBITTOTALLEN(typmod);
	result = (VarBit *) palloc(rlen);
	VARATT_SIZEP(result) = rlen;
	VARBITLEN(result) = typmod;

	r = VARBITS(result);
	destbitsleft = typmod;
#ifndef INT64_IS_BUSTED
	srcbitsleft = 64;
#else
	srcbitsleft = 32;			/* don't try to shift more than 32 */
#endif
	/* drop any input bits that don't fit */
	srcbitsleft = Min(srcbitsleft, destbitsleft);
	/* sign-fill any excess bytes in output */
	while (destbitsleft >= srcbitsleft + 8)
	{
		*r++ = (bits8) ((a < 0) ? BITMASK : 0);
		destbitsleft -= 8;
	}
	/* store first fractional byte */
	if (destbitsleft > srcbitsleft)
	{
		*r++ = (bits8) ((a >> (srcbitsleft - 8)) & BITMASK);
		destbitsleft -= 8;
	}
	/* Now srcbitsleft and destbitsleft are the same, need not track both */
	/* store whole bytes */
	while (destbitsleft >= 8)
	{
		*r++ = (bits8) ((a >> (destbitsleft - 8)) & BITMASK);
		destbitsleft -= 8;
	}
	/* store last fractional byte */
	if (destbitsleft > 0)
		*r = (bits8) ((a << (8 - destbitsleft)) & BITMASK);

	PG_RETURN_VARBIT_P(result);
}

Datum
bittoint8(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);
	uint64		result;
	bits8	   *r;

	/* Check that the bit string is not too long */
	if (VARBITLEN(arg) > sizeof(result) * BITS_PER_BYTE)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));

	result = 0;
	for (r = VARBITS(arg); r < VARBITEND(arg); r++)
	{
		result <<= BITS_PER_BYTE;
		result |= *r;
	}
	/* Now shift the result to take account of the padding at the end */
	result >>= VARBITPAD(arg);

	PG_RETURN_INT64(result);
}


/* Determines the position of S2 in the bitstring S1 (1-based string).
 * If S2 does not appear in S1 this function returns 0.
 * If S2 is of length 0 this function returns 1.
 * Compatible in usage with POSITION() functions for other data types.
 */
Datum
bitposition(PG_FUNCTION_ARGS)
{
	VarBit	   *str = PG_GETARG_VARBIT_P(0);
	VarBit	   *substr = PG_GETARG_VARBIT_P(1);
	int			substr_length,
				str_length,
				i,
				is;
	bits8	   *s,				/* pointer into substring */
			   *p;				/* pointer into str */
	bits8		cmp,			/* shifted substring byte to compare */
				mask1,			/* mask for substring byte shifted right */
				mask2,			/* mask for substring byte shifted left */
				end_mask,		/* pad mask for last substring byte */
				str_mask;		/* pad mask for last string byte */
	bool		is_match;

	/* Get the substring length */
	substr_length = VARBITLEN(substr);
	str_length = VARBITLEN(str);

	/* String has zero length or substring longer than string, return 0 */
	if ((str_length == 0) || (substr_length > str_length))
		PG_RETURN_INT32(0);

	/* zero-length substring means return 1 */
	if (substr_length == 0)
		PG_RETURN_INT32(1);

	/* Initialise the padding masks */
	end_mask = BITMASK << VARBITPAD(substr);
	str_mask = BITMASK << VARBITPAD(str);
	for (i = 0; i < VARBITBYTES(str) - VARBITBYTES(substr) + 1; i++)
	{
		for (is = 0; is < BITS_PER_BYTE; is++)
		{
			is_match = true;
			p = VARBITS(str) + i;
			mask1 = BITMASK >> is;
			mask2 = ~mask1;
			for (s = VARBITS(substr);
				 is_match && s < VARBITEND(substr); s++)
			{
				cmp = *s >> is;
				if (s == VARBITEND(substr) - 1)
				{
					mask1 &= end_mask >> is;
					if (p == VARBITEND(str) - 1)
					{
						/* Check that there is enough of str left */
						if (mask1 & ~str_mask)
						{
							is_match = false;
							break;
						}
						mask1 &= str_mask;
					}
				}
				is_match = ((cmp ^ *p) & mask1) == 0;
				if (!is_match)
					break;
				/* Move on to the next byte */
				p++;
				if (p == VARBITEND(str))
				{
					mask2 = end_mask << (BITS_PER_BYTE - is);
					is_match = mask2 == 0;
#if 0
					elog(DEBUG4, "S. %d %d em=%2x sm=%2x r=%d",
						 i, is, end_mask, mask2, is_match);
#endif
					break;
				}
				cmp = *s << (BITS_PER_BYTE - is);
				if (s == VARBITEND(substr) - 1)
				{
					mask2 &= end_mask << (BITS_PER_BYTE - is);
					if (p == VARBITEND(str) - 1)
					{
						if (mask2 & ~str_mask)
						{
							is_match = false;
							break;
						}
						mask2 &= str_mask;
					}
				}
				is_match = ((cmp ^ *p) & mask2) == 0;
			}
			/* Have we found a match? */
			if (is_match)
				PG_RETURN_INT32(i * BITS_PER_BYTE + is + 1);
		}
	}
	PG_RETURN_INT32(0);
}
