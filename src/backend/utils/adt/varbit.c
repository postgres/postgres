/*-------------------------------------------------------------------------
 *
 * varbit.c
 *	  Functions for the SQL datatypes BIT() and BIT VARYING().
 *
 * Code originally contributed by Adriaan Joubert.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/varbit.c,v 1.11 2000/11/07 11:35:16 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/fmgroids.h"
#include "utils/varbit.h"

#define HEXDIG(z)	 ((z)<10 ? ((z)+'0') : ((z)-10+'A'))


/*----------
 *	Prefixes:
 *	 zp    -- zero-padded fixed length bit string
 *	 var   -- varying bit string
 *
 *	attypmod -- contains the length of the bit string in bits, or for
 *			   varying bits the maximum length.
 *
 *	The data structure contains the following elements:
 *	  header  -- length of the whole data structure (incl header)
 *				 in bytes. (as with all varying length datatypes)
 *	  data section -- private data section for the bits data structures
 *		bitlength -- length of the bit string in bits
 *		bitdata   -- bit string, most significant byte first
 *----------
 */

/*
 * zpbit_in -
 *	  converts a char string to the internal representation of a bitstring.
 *		  The length is determined by the number of bits required plus
 *		  VARHDRSZ bytes or from atttypmod.
 */
Datum
zpbit_in(PG_FUNCTION_ARGS)
{
	char	   *s = PG_GETARG_CSTRING(0);
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
	int			bc,
				ipad;
	bits8		x = 0;

	/* Check that the first character is a b or an x */
	if (s[0] == 'b' || s[0] == 'B')
		bit_not_hex = true;
	else if (s[0] == 'x' || s[0] == 'X')
		bit_not_hex = false;
	else
	{
		elog(ERROR, "zpbit_in: %s is not a valid bitstring", s);
		bit_not_hex = false;	/* keep compiler quiet */
	}

	slen = strlen(s) - 1;
	/* Determine bitlength from input string */
	if (bit_not_hex)
		bitlen = slen;
	else
		bitlen = slen * 4;

	/*
	 * Sometimes atttypmod is not supplied. If it is supplied we need to
	 * make sure that the bitstring fits. Note that the number of infered
	 * bits can be larger than the number of actual bits needed, but only
	 * if we are reading a hex string and not by more than 3 bits, as a
	 * hex string gives an accurate length up to 4 bits
	 */
	if (atttypmod <= 0)
		atttypmod = bitlen;
	else if (bit_not_hex ? (bitlen > atttypmod) : (bitlen > atttypmod + 3))
		elog(ERROR, "zpbit_in: bit string too long for bit(%d)",
			 atttypmod);

	len = VARBITTOTALLEN(atttypmod);
	result = (VarBit *) palloc(len);
	/* set to 0 so that *r is always initialised and string is zero-padded */
	memset(result, 0, len);
	VARATT_SIZEP(result) = len;
	VARBITLEN(result) = atttypmod;

	sp = s + 1;
	r = VARBITS(result);
	if (bit_not_hex)
	{
		/* Parse the bit representation of the string */
		/* We know it fits, as bitlen was compared to atttypmod */
		x = BITHIGH;
		for (; *sp; sp++)
		{
			if (*sp == '1')
				*r |= x;
			else if (*sp != '0')
				elog(ERROR, "Cannot parse %c as a binary digit", *sp);
			x >>= 1;
			if (x == 0)
			{
				x = BITHIGH;
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
				elog(ERROR, "Cannot parse %c as a hex digit", *sp);
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

	if (bitlen > atttypmod)
	{
		/* Check that this fitted */
		r = VARBITEND(result) - 1;
		ipad = VARBITPAD(result);

		/*
		 * The bottom ipad bits of the byte pointed to by r need to be
		 * zero
		 */
		if (((*r << (BITS_PER_BYTE - ipad)) & BITMASK) != 0)
			elog(ERROR, "zpbit_in: bit string too long for bit(%d)",
				 atttypmod);
	}

	PG_RETURN_VARBIT_P(result);
}

/* zpbit_out -
 *	  for the time being we print everything as hex strings, as this is likely
 *	  to be more compact than bit strings, and consequently much more efficient
 *	  for long strings
 */
Datum
zpbit_out(PG_FUNCTION_ARGS)
{
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
	 * Go back one step if we printed a hex number that was not part
	 * of the bitstring anymore
	 */
	if (i > len)
		r--;
	*r = '\0';

	PG_RETURN_CSTRING(result);
}

/* zpbit()
 * Converts a bit() type to a specific internal length.
 * len is the bitlength specified in the column definition.
 */
Datum
zpbit(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);
	int32		len = PG_GETARG_INT32(1);
	VarBit	   *result;
	int			rlen;

	/* No work if typmod is invalid or supplied data matches it already */
	if (len <= 0 || len == VARBITLEN(arg))
		PG_RETURN_VARBIT_P(arg);

	rlen = VARBITTOTALLEN(len);
	result = (VarBit *) palloc(rlen);
	/* set to 0 so that result is zero-padded if input is shorter */
	memset(result, 0, rlen);
	VARATT_SIZEP(result) = rlen;
	VARBITLEN(result) = len;

	memcpy(VARBITS(result), VARBITS(arg),
		   Min(VARBITBYTES(result), VARBITBYTES(arg)));

	PG_RETURN_VARBIT_P(result);
}

/* _zpbit()
 * Converts an array of bit() elements to a specific internal length.
 * len is the bitlength specified in the column definition.
 */
Datum
_zpbit(PG_FUNCTION_ARGS)
{
	ArrayType  *v = (ArrayType *) PG_GETARG_VARLENA_P(0);
	int32		len = PG_GETARG_INT32(1);
	FunctionCallInfoData	locfcinfo;
	/*
	 * Since zpbit() is a built-in function, we should only need to
	 * look it up once per run.
	 */
	static FmgrInfo			zpbit_finfo;

	if (zpbit_finfo.fn_oid == InvalidOid)
		fmgr_info(F_ZPBIT, &zpbit_finfo);

	MemSet(&locfcinfo, 0, sizeof(locfcinfo));
	locfcinfo.flinfo = &zpbit_finfo;
	locfcinfo.nargs = 2;
	/* We assume we are "strict" and need not worry about null inputs */
	locfcinfo.arg[0] = PointerGetDatum(v);
	locfcinfo.arg[1] = Int32GetDatum(len);

	return array_map(&locfcinfo, ZPBITOID, ZPBITOID);
}

/*
 * varbit_in -
 *	  converts a string to the internal representation of a bitstring.
 *		This is the same as zpbit_in except that atttypmod is taken as
 *		the maximum length, not the exact length to force the bitstring to.
 */
Datum
varbit_in(PG_FUNCTION_ARGS)
{
	char	   *s = PG_GETARG_CSTRING(0);
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
	int			bc,
				ipad;
	bits8		x = 0;

	/* Check that the first character is a b or an x */
	if (s[0] == 'b' || s[0] == 'B')
		bit_not_hex = true;
	else if (s[0] == 'x' || s[0] == 'X')
		bit_not_hex = false;
	else
	{
		elog(ERROR, "varbit_in: %s is not a valid bitstring", s);
		bit_not_hex = false;	/* keep compiler quiet */
	}

	slen = strlen(s) - 1;
	/* Determine bitlength from input string */
	if (bit_not_hex)
		bitlen = slen;
	else
		bitlen = slen * 4;

	/*
	 * Sometimes atttypmod is not supplied. If it is supplied we need to
	 * make sure that the bitstring fits. Note that the number of infered
	 * bits can be larger than the number of actual bits needed, but only
	 * if we are reading a hex string and not by more than 3 bits, as a
	 * hex string gives an accurate length up to 4 bits
	 */
	if (atttypmod <= 0)
		atttypmod = bitlen;
	else if (bit_not_hex ? (bitlen > atttypmod) : (bitlen > atttypmod + 3))
		elog(ERROR, "varbit_in: bit string too long for bit varying(%d)",
			 atttypmod);

	len = VARBITTOTALLEN(bitlen);
	result = (VarBit *) palloc(len);
	/* set to 0 so that *r is always initialised and string is zero-padded */
	memset(result, 0, len);
	VARATT_SIZEP(result) = len;
	VARBITLEN(result) = Min(bitlen, atttypmod);

	sp = s + 1;
	r = VARBITS(result);
	if (bit_not_hex)
	{
		/* Parse the bit representation of the string */
		/* We know it fits, as bitlen was compared to atttypmod */
		x = BITHIGH;
		for (; *sp; sp++)
		{
			if (*sp == '1')
				*r |= x;
			else if (*sp != '0')
				elog(ERROR, "Cannot parse %c as a binary digit", *sp);
			x >>= 1;
			if (x == 0)
			{
				x = BITHIGH;
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
				elog(ERROR, "Cannot parse %c as a hex digit", *sp);
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

	if (bitlen > atttypmod)
	{
		/* Check that this fitted */
		r = VARBITEND(result) - 1;
		ipad = VARBITPAD(result);

		/*
		 * The bottom ipad bits of the byte pointed to by r need to be
		 * zero
		 */
		if (((*r << (BITS_PER_BYTE - ipad)) & BITMASK) != 0)
			elog(ERROR, "varbit_in: bit string too long for bit varying(%d)",
				 atttypmod);
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
	result = (char *) palloc(len + 2);
	sp = VARBITS(s);
	r = result;
	*r++ = 'B';
	for (i = 0; i < len - BITS_PER_BYTE; i += BITS_PER_BYTE, sp++)
	{
		x = *sp;
		for (k = 0; k < BITS_PER_BYTE; k++)
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
	*r = '\0';

	PG_RETURN_CSTRING(result);
}

/* varbit()
 * Converts a varbit() type to a specific internal length.
 * len is the maximum bitlength specified in the column definition.
 */
Datum
varbit(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);
	int32		len = PG_GETARG_INT32(1);
	VarBit	   *result;
	int			rlen;

	/* No work if typmod is invalid or supplied data matches it already */
	if (len <= 0 || len >= VARBITLEN(arg))
		PG_RETURN_VARBIT_P(arg);

	rlen = VARBITTOTALLEN(len);
	result = (VarBit *) palloc(rlen);
	VARATT_SIZEP(result) = rlen;
	VARBITLEN(result) = len;

	memcpy(VARBITS(result), VARBITS(arg), VARBITBYTES(result));

	PG_RETURN_VARBIT_P(result);
}

/* _varbit()
 * Converts an array of bit() elements to a specific internal length.
 * len is the maximum bitlength specified in the column definition.
 */
Datum
_varbit(PG_FUNCTION_ARGS)
{
	ArrayType  *v = (ArrayType *) PG_GETARG_VARLENA_P(0);
	int32		len = PG_GETARG_INT32(1);
	FunctionCallInfoData	locfcinfo;
	/*
	 * Since varbit() is a built-in function, we should only need to
	 * look it up once per run.
	 */
	static FmgrInfo			varbit_finfo;

	if (varbit_finfo.fn_oid == InvalidOid)
		fmgr_info(F_VARBIT, &varbit_finfo);

	MemSet(&locfcinfo, 0, sizeof(locfcinfo));
	locfcinfo.flinfo = &varbit_finfo;
	locfcinfo.nargs = 2;
	/* We assume we are "strict" and need not worry about null inputs */
	locfcinfo.arg[0] = PointerGetDatum(v);
	locfcinfo.arg[1] = Int32GetDatum(len);

	return array_map(&locfcinfo, VARBITOID, VARBITOID);
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
	if (bitlen1 != bitlen2)
		result = false;
	else
	{
		/* bit strings are always stored in a full number of bytes */
		result = memcmp(VARBITS(arg1), VARBITS(arg2), VARBITBYTES(arg1)) == 0;
	}

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
	if (bitlen1 != bitlen2)
		result = true;
	else
	{
		/* bit strings are always stored in a full number of bytes */
		result = memcmp(VARBITS(arg1), VARBITS(arg2), VARBITBYTES(arg1)) != 0;
	}

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

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
		 * OK, we've got a true substring starting at position s1-1 and
		 * ending at position e1-1
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
		elog(ERROR, "bitand: Cannot AND bitstrings of different sizes");
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
		elog(ERROR, "bitor: Cannot OR bitstrings of different sizes");
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
		elog(ERROR, "bitxor: Cannot XOR bitstrings of different sizes");
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
		*r++ = ~ *p;

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
		memset(r, 0, VARBITBYTES(arg));
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
		memset(r + len, 0, byte_shift);
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
		memset(r, 0, VARBITBYTES(arg));
		PG_RETURN_VARBIT_P(result);
	}

	byte_shift = shft / BITS_PER_BYTE;
	ishift = shft % BITS_PER_BYTE;
	p = VARBITS(arg);

	/* Set the first part of the result to 0 */
	memset(r, 0, byte_shift);
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

/* This is not defined in any standard. We retain the natural ordering of
 * bits here, as it just seems more intuitive. 
 */
Datum
bitfromint4(PG_FUNCTION_ARGS)
{
	int32		a = PG_GETARG_INT32(0);
	VarBit	   *result;
	bits8	   *r;
	int			len;
  
	/* allocate enough space for the bits in an int4 */
	len = VARBITTOTALLEN(sizeof(int4)*BITS_PER_BYTE);
	result = (VarBit *) palloc(len);
	VARATT_SIZEP(result) = len;
	VARBITLEN(result) = sizeof(int4)*BITS_PER_BYTE;
	/* masks and shifts here are just too painful and we know that an int4 has
	 * got 4 bytes
	 */
	r = VARBITS(result);
	r[0] = (bits8) ((a >> (3*BITS_PER_BYTE)) & BITMASK);
	r[1] = (bits8) ((a >> (2*BITS_PER_BYTE)) & BITMASK);
	r[2] = (bits8) ((a >> (1*BITS_PER_BYTE)) & BITMASK);
	r[3] = (bits8) (a & BITMASK);

	PG_RETURN_VARBIT_P(result);
}

Datum
bittoint4(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);
	uint32		result;
	bits8	   *r;

	/* Check that the bit string is not too long */
	if (VARBITLEN(arg) > sizeof(int4)*BITS_PER_BYTE) 
		elog(ERROR, "Bit string is too large to fit in an int4");
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



/* Determines the position of S2 in the bitstring S1 (1-based string).
 * If S2 does not appear in S1 this function returns 0.
 * If S2 is of length 0 this function returns 1.
 */
Datum
bitposition(PG_FUNCTION_ARGS)
{
	VarBit		*substr = PG_GETARG_VARBIT_P(1);
	VarBit		*arg = PG_GETARG_VARBIT_P(0);
	int			substr_length, 
				arg_length,
				i,
				is;
	bits8		*s,				/* pointer into substring */
				*p;				/* pointer into arg */
	bits8		cmp,			/* shifted substring byte to compare */ 
				mask1,          /* mask for substring byte shifted right */
				mask2,          /* mask for substring byte shifted left */
				end_mask,       /* pad mask for last substring byte */
				arg_mask;		/* pad mask for last argument byte */
	bool		is_match;

	/* Get the substring length */
	substr_length = VARBITLEN(substr);
	arg_length = VARBITLEN(arg);

	/* Argument has 0 length or substring longer than argument, return 0 */
	if (arg_length == 0 || substr_length > arg_length)
		PG_RETURN_INT32(0);	
	
	/* 0-length means return 1 */
	if (substr_length == 0)
		PG_RETURN_INT32(1);

	/* Initialise the padding masks */
	end_mask = BITMASK << VARBITPAD(substr);
	arg_mask = BITMASK << VARBITPAD(arg);
	for (i = 0; i < VARBITBYTES(arg) - VARBITBYTES(substr) + 1; i++) 
	{
		for (is = 0; is < BITS_PER_BYTE; is++) {
			is_match = true;
			p = VARBITS(arg) + i;
			mask1 = BITMASK >> is;
			mask2 = ~mask1;
			for (s = VARBITS(substr); 
				 is_match && s < VARBITEND(substr); s++) 
			{
				cmp = *s >> is;
				if (s == VARBITEND(substr) - 1) 
				{
					mask1 &= end_mask >> is;
					if (p == VARBITEND(arg) - 1) {
						/* Check that there is enough of arg left */
						if (mask1 & ~arg_mask) {
							is_match = false;
							break;
						}
						mask1 &= arg_mask;
					}
				}
				is_match = ((cmp ^ *p) & mask1) == 0;
				if (!is_match)
					break;
				/* Move on to the next byte */
				p++;
				if (p == VARBITEND(arg)) {
					mask2 = end_mask << (BITS_PER_BYTE - is);
					is_match = mask2 == 0;
					elog(NOTICE,"S. %d %d em=%2x sm=%2x r=%d",
						 i,is,end_mask,mask2,is_match);
					break;
				}
				cmp = *s << (BITS_PER_BYTE - is);
				if (s == VARBITEND(substr) - 1) 
				{
					mask2 &= end_mask << (BITS_PER_BYTE - is);
					if (p == VARBITEND(arg) - 1) {
						if (mask2 & ~arg_mask) {
							is_match = false;
							break;
						}
						mask2 &= arg_mask;
					}
				}
				is_match = ((cmp ^ *p) & mask2) == 0;
			}
			/* Have we found a match */
			if (is_match)
				PG_RETURN_INT32(i*BITS_PER_BYTE + is + 1);
		}
	}
	PG_RETURN_INT32(0);
}
