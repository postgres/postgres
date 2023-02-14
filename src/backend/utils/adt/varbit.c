/*-------------------------------------------------------------------------
 *
 * varbit.c
 *	  Functions for the SQL datatypes BIT() and BIT VARYING().
 *
 * The data structure contains the following elements:
 *   header  -- length of the whole data structure (incl header)
 *              in bytes (as with all varying length datatypes)
 *   data section -- private data section for the bits data structures
 *     bitlength -- length of the bit string in bits
 *     bitdata   -- bit string, most significant byte first
 *
 * The length of the bitdata vector should always be exactly as many
 * bytes as are needed for the given bitlength.  If the bitlength is
 * not a multiple of 8, the extra low-order padding bits of the last
 * byte must be zeroes.
 *
 * attypmod is defined as the length of the bit string in bits, or for
 * varying bits the maximum length.
 *
 * Code originally contributed by Adriaan Joubert.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/varbit.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "common/int.h"
#include "libpq/pqformat.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "port/pg_bitutils.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/varbit.h"

#define HEXDIG(z)	 ((z)<10 ? ((z)+'0') : ((z)-10+'A'))

/* Mask off any bits that should be zero in the last byte of a bitstring */
#define VARBIT_PAD(vb) \
	do { \
		int32	pad_ = VARBITPAD(vb); \
		Assert(pad_ >= 0 && pad_ < BITS_PER_BYTE); \
		if (pad_ > 0) \
			*(VARBITS(vb) + VARBITBYTES(vb) - 1) &= BITMASK << pad_; \
	} while (0)

/*
 * Many functions work byte-by-byte, so they have a pointer handy to the
 * last-plus-one byte, which saves a cycle or two.
 */
#define VARBIT_PAD_LAST(vb, ptr) \
	do { \
		int32	pad_ = VARBITPAD(vb); \
		Assert(pad_ >= 0 && pad_ < BITS_PER_BYTE); \
		if (pad_ > 0) \
			*((ptr) - 1) &= BITMASK << pad_; \
	} while (0)

/* Assert proper padding of a bitstring */
#ifdef USE_ASSERT_CHECKING
#define VARBIT_CORRECTLY_PADDED(vb) \
	do { \
		int32	pad_ = VARBITPAD(vb); \
		Assert(pad_ >= 0 && pad_ < BITS_PER_BYTE); \
		Assert(pad_ == 0 || \
			   (*(VARBITS(vb) + VARBITBYTES(vb) - 1) & ~(BITMASK << pad_)) == 0); \
	} while (0)
#else
#define VARBIT_CORRECTLY_PADDED(vb) ((void) 0)
#endif

static VarBit *bit_catenate(VarBit *arg1, VarBit *arg2);
static VarBit *bitsubstring(VarBit *arg, int32 s, int32 l,
							bool length_not_specified);
static VarBit *bit_overlay(VarBit *t1, VarBit *t2, int sp, int sl);


/*
 * common code for bittypmodin and varbittypmodin
 */
static int32
anybit_typmodin(ArrayType *ta, const char *typename)
{
	int32		typmod;
	int32	   *tl;
	int			n;

	tl = ArrayGetIntegerTypmods(ta, &n);

	/*
	 * we're not too tense about good error message here because grammar
	 * shouldn't allow wrong number of modifiers for BIT
	 */
	if (n != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid type modifier")));

	if (*tl < 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("length for type %s must be at least 1",
						typename)));
	if (*tl > (MaxAttrSize * BITS_PER_BYTE))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("length for type %s cannot exceed %d",
						typename, MaxAttrSize * BITS_PER_BYTE)));

	typmod = *tl;

	return typmod;
}

/*
 * common code for bittypmodout and varbittypmodout
 */
static char *
anybit_typmodout(int32 typmod)
{
	char	   *res = (char *) palloc(64);

	if (typmod >= 0)
		snprintf(res, 64, "(%d)", typmod);
	else
		*res = '\0';

	return res;
}


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
	Node	   *escontext = fcinfo->context;
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

	/*
	 * Determine bitlength from input string.  MaxAllocSize ensures a regular
	 * input is small enough, but we must check hex input.
	 */
	slen = strlen(sp);
	if (bit_not_hex)
		bitlen = slen;
	else
	{
		if (slen > VARBITMAXLEN / 4)
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("bit string length exceeds the maximum allowed (%d)",
							VARBITMAXLEN)));
		bitlen = slen * 4;
	}

	/*
	 * Sometimes atttypmod is not supplied. If it is supplied we need to make
	 * sure that the bitstring fits.
	 */
	if (atttypmod <= 0)
		atttypmod = bitlen;
	else if (bitlen != atttypmod)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
				 errmsg("bit string length %d does not match type bit(%d)",
						bitlen, atttypmod)));

	len = VARBITTOTALLEN(atttypmod);
	/* set to 0 so that *r is always initialised and string is zero-padded */
	result = (VarBit *) palloc0(len);
	SET_VARSIZE(result, len);
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
				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("\"%.*s\" is not a valid binary digit",
								pg_mblen(sp), sp)));

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
				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("\"%.*s\" is not a valid hexadecimal digit",
								pg_mblen(sp), sp)));

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

	/*
	 * This is how one would print a hex string, in case someone wants to
	 * write a formatting function.
	 */
	VarBit	   *s = PG_GETARG_VARBIT_P(0);
	char	   *result,
			   *r;
	bits8	   *sp;
	int			i,
				len,
				bitlen;

	/* Assertion to help catch any bit functions that don't pad correctly */
	VARBIT_CORRECTLY_PADDED(s);

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

	bitlen = pq_getmsgint(buf, sizeof(int32));
	if (bitlen < 0 || bitlen > VARBITMAXLEN)
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
	SET_VARSIZE(result, len);
	VARBITLEN(result) = bitlen;

	pq_copymsgbytes(buf, (char *) VARBITS(result), VARBITBYTES(result));

	/* Make sure last byte is correctly zero-padded */
	VARBIT_PAD(result);

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

/*
 * bit()
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

	/* No work if typmod is invalid or supplied data matches it already */
	if (len <= 0 || len > VARBITMAXLEN || len == VARBITLEN(arg))
		PG_RETURN_VARBIT_P(arg);

	if (!isExplicit)
		ereport(ERROR,
				(errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
				 errmsg("bit string length %d does not match type bit(%d)",
						VARBITLEN(arg), len)));

	rlen = VARBITTOTALLEN(len);
	/* set to 0 so that string is zero-padded */
	result = (VarBit *) palloc0(rlen);
	SET_VARSIZE(result, rlen);
	VARBITLEN(result) = len;

	memcpy(VARBITS(result), VARBITS(arg),
		   Min(VARBITBYTES(result), VARBITBYTES(arg)));

	/*
	 * Make sure last byte is zero-padded if needed.  This is useless but safe
	 * if source data was shorter than target length (we assume the last byte
	 * of the source data was itself correctly zero-padded).
	 */
	VARBIT_PAD(result);

	PG_RETURN_VARBIT_P(result);
}

Datum
bittypmodin(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);

	PG_RETURN_INT32(anybit_typmodin(ta, "bit"));
}

Datum
bittypmodout(PG_FUNCTION_ARGS)
{
	int32		typmod = PG_GETARG_INT32(0);

	PG_RETURN_CSTRING(anybit_typmodout(typmod));
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
	Node	   *escontext = fcinfo->context;
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

	/*
	 * Determine bitlength from input string.  MaxAllocSize ensures a regular
	 * input is small enough, but we must check hex input.
	 */
	slen = strlen(sp);
	if (bit_not_hex)
		bitlen = slen;
	else
	{
		if (slen > VARBITMAXLEN / 4)
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("bit string length exceeds the maximum allowed (%d)",
							VARBITMAXLEN)));
		bitlen = slen * 4;
	}

	/*
	 * Sometimes atttypmod is not supplied. If it is supplied we need to make
	 * sure that the bitstring fits.
	 */
	if (atttypmod <= 0)
		atttypmod = bitlen;
	else if (bitlen > atttypmod)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
				 errmsg("bit string too long for type bit varying(%d)",
						atttypmod)));

	len = VARBITTOTALLEN(bitlen);
	/* set to 0 so that *r is always initialised and string is zero-padded */
	result = (VarBit *) palloc0(len);
	SET_VARSIZE(result, len);
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
				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("\"%.*s\" is not a valid binary digit",
								pg_mblen(sp), sp)));

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
				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("\"%.*s\" is not a valid hexadecimal digit",
								pg_mblen(sp), sp)));

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

/*
 * varbit_out -
 *	  Prints the string as bits to preserve length accurately
 *
 * XXX varbit_recv() and hex input to varbit_in() can load a value that this
 * cannot emit.  Consider using hex output for such values.
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

	/* Assertion to help catch any bit functions that don't pad correctly */
	VARBIT_CORRECTLY_PADDED(s);

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

	bitlen = pq_getmsgint(buf, sizeof(int32));
	if (bitlen < 0 || bitlen > VARBITMAXLEN)
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
	SET_VARSIZE(result, len);
	VARBITLEN(result) = bitlen;

	pq_copymsgbytes(buf, (char *) VARBITS(result), VARBITBYTES(result));

	/* Make sure last byte is correctly zero-padded */
	VARBIT_PAD(result);

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
	pq_sendint32(&buf, VARBITLEN(s));
	pq_sendbytes(&buf, VARBITS(s), VARBITBYTES(s));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * varbit_support()
 *
 * Planner support function for the varbit() length coercion function.
 *
 * Currently, the only interesting thing we can do is flatten calls that set
 * the new maximum length >= the previous maximum length.  We can ignore the
 * isExplicit argument, since that only affects truncation cases.
 */
Datum
varbit_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestSimplify))
	{
		SupportRequestSimplify *req = (SupportRequestSimplify *) rawreq;
		FuncExpr   *expr = req->fcall;
		Node	   *typmod;

		Assert(list_length(expr->args) >= 2);

		typmod = (Node *) lsecond(expr->args);

		if (IsA(typmod, Const) && !((Const *) typmod)->constisnull)
		{
			Node	   *source = (Node *) linitial(expr->args);
			int32		new_typmod = DatumGetInt32(((Const *) typmod)->constvalue);
			int32		old_max = exprTypmod(source);
			int32		new_max = new_typmod;

			/* Note: varbit() treats typmod 0 as invalid, so we do too */
			if (new_max <= 0 || (old_max > 0 && old_max <= new_max))
				ret = relabel_to_typmod(source, new_typmod);
		}
	}

	PG_RETURN_POINTER(ret);
}

/*
 * varbit()
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
	SET_VARSIZE(result, rlen);
	VARBITLEN(result) = len;

	memcpy(VARBITS(result), VARBITS(arg), VARBITBYTES(result));

	/* Make sure last byte is correctly zero-padded */
	VARBIT_PAD(result);

	PG_RETURN_VARBIT_P(result);
}

Datum
varbittypmodin(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);

	PG_RETURN_INT32(anybit_typmodin(ta, "varbit"));
}

Datum
varbittypmodout(PG_FUNCTION_ARGS)
{
	int32		typmod = PG_GETARG_INT32(0);

	PG_RETURN_CSTRING(anybit_typmodout(typmod));
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

/*
 * bit_cmp
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

/*
 * bitcat
 * Concatenation of bit strings
 */
Datum
bitcat(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *arg2 = PG_GETARG_VARBIT_P(1);

	PG_RETURN_VARBIT_P(bit_catenate(arg1, arg2));
}

static VarBit *
bit_catenate(VarBit *arg1, VarBit *arg2)
{
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

	if (bitlen1 > VARBITMAXLEN - bitlen2)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("bit string length exceeds the maximum allowed (%d)",
						VARBITMAXLEN)));
	bytelen = VARBITTOTALLEN(bitlen1 + bitlen2);

	result = (VarBit *) palloc(bytelen);
	SET_VARSIZE(result, bytelen);
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

	/* The pad bits should be already zero at this point */

	return result;
}

/*
 * bitsubstr
 * retrieve a substring from the bit string.
 * Note, s is 1-based.
 * SQL draft 6.10 9)
 */
Datum
bitsubstr(PG_FUNCTION_ARGS)
{
	PG_RETURN_VARBIT_P(bitsubstring(PG_GETARG_VARBIT_P(0),
									PG_GETARG_INT32(1),
									PG_GETARG_INT32(2),
									false));
}

Datum
bitsubstr_no_len(PG_FUNCTION_ARGS)
{
	PG_RETURN_VARBIT_P(bitsubstring(PG_GETARG_VARBIT_P(0),
									PG_GETARG_INT32(1),
									-1, true));
}

static VarBit *
bitsubstring(VarBit *arg, int32 s, int32 l, bool length_not_specified)
{
	VarBit	   *result;
	int			bitlen,
				rbitlen,
				len,
				ishift,
				i;
	int32		e,
				s1,
				e1;
	bits8	   *r,
			   *ps;

	bitlen = VARBITLEN(arg);
	s1 = Max(s, 1);
	/* If we do not have an upper bound, use end of string */
	if (length_not_specified)
	{
		e1 = bitlen + 1;
	}
	else if (l < 0)
	{
		/* SQL99 says to throw an error for E < S, i.e., negative length */
		ereport(ERROR,
				(errcode(ERRCODE_SUBSTRING_ERROR),
				 errmsg("negative substring length not allowed")));
		e1 = -1;				/* silence stupider compilers */
	}
	else if (pg_add_s32_overflow(s, l, &e))
	{
		/*
		 * L could be large enough for S + L to overflow, in which case the
		 * substring must run to end of string.
		 */
		e1 = bitlen + 1;
	}
	else
	{
		e1 = Min(e, bitlen + 1);
	}
	if (s1 > bitlen || e1 <= s1)
	{
		/* Need to return a zero-length bitstring */
		len = VARBITTOTALLEN(0);
		result = (VarBit *) palloc(len);
		SET_VARSIZE(result, len);
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
		SET_VARSIZE(result, len);
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

		/* Make sure last byte is correctly zero-padded */
		VARBIT_PAD(result);
	}

	return result;
}

/*
 * bitoverlay
 *	Replace specified substring of first string with second
 *
 * The SQL standard defines OVERLAY() in terms of substring and concatenation.
 * This code is a direct implementation of what the standard says.
 */
Datum
bitoverlay(PG_FUNCTION_ARGS)
{
	VarBit	   *t1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *t2 = PG_GETARG_VARBIT_P(1);
	int			sp = PG_GETARG_INT32(2);	/* substring start position */
	int			sl = PG_GETARG_INT32(3);	/* substring length */

	PG_RETURN_VARBIT_P(bit_overlay(t1, t2, sp, sl));
}

Datum
bitoverlay_no_len(PG_FUNCTION_ARGS)
{
	VarBit	   *t1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *t2 = PG_GETARG_VARBIT_P(1);
	int			sp = PG_GETARG_INT32(2);	/* substring start position */
	int			sl;

	sl = VARBITLEN(t2);			/* defaults to length(t2) */
	PG_RETURN_VARBIT_P(bit_overlay(t1, t2, sp, sl));
}

static VarBit *
bit_overlay(VarBit *t1, VarBit *t2, int sp, int sl)
{
	VarBit	   *result;
	VarBit	   *s1;
	VarBit	   *s2;
	int			sp_pl_sl;

	/*
	 * Check for possible integer-overflow cases.  For negative sp, throw a
	 * "substring length" error because that's what should be expected
	 * according to the spec's definition of OVERLAY().
	 */
	if (sp <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_SUBSTRING_ERROR),
				 errmsg("negative substring length not allowed")));
	if (pg_add_s32_overflow(sp, sl, &sp_pl_sl))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	s1 = bitsubstring(t1, 1, sp - 1, false);
	s2 = bitsubstring(t1, sp_pl_sl, -1, true);
	result = bit_catenate(s1, t2);
	result = bit_catenate(result, s2);

	return result;
}

/*
 * bit_count
 *
 * Returns the number of bits set in a bit string.
 */
Datum
bit_bit_count(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);

	PG_RETURN_INT64(pg_popcount((char *) VARBITS(arg), VARBITBYTES(arg)));
}

/*
 * bitlength, bitoctetlength
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

/*
 * bit_and
 * perform a logical AND on two bit strings.
 */
Datum
bit_and(PG_FUNCTION_ARGS)
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
	SET_VARSIZE(result, len);
	VARBITLEN(result) = bitlen1;

	p1 = VARBITS(arg1);
	p2 = VARBITS(arg2);
	r = VARBITS(result);
	for (i = 0; i < VARBITBYTES(arg1); i++)
		*r++ = *p1++ & *p2++;

	/* Padding is not needed as & of 0 pads is 0 */

	PG_RETURN_VARBIT_P(result);
}

/*
 * bit_or
 * perform a logical OR on two bit strings.
 */
Datum
bit_or(PG_FUNCTION_ARGS)
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
				 errmsg("cannot OR bit strings of different sizes")));
	len = VARSIZE(arg1);
	result = (VarBit *) palloc(len);
	SET_VARSIZE(result, len);
	VARBITLEN(result) = bitlen1;

	p1 = VARBITS(arg1);
	p2 = VARBITS(arg2);
	r = VARBITS(result);
	for (i = 0; i < VARBITBYTES(arg1); i++)
		*r++ = *p1++ | *p2++;

	/* Padding is not needed as | of 0 pads is 0 */

	PG_RETURN_VARBIT_P(result);
}

/*
 * bitxor
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

	bitlen1 = VARBITLEN(arg1);
	bitlen2 = VARBITLEN(arg2);
	if (bitlen1 != bitlen2)
		ereport(ERROR,
				(errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
				 errmsg("cannot XOR bit strings of different sizes")));

	len = VARSIZE(arg1);
	result = (VarBit *) palloc(len);
	SET_VARSIZE(result, len);
	VARBITLEN(result) = bitlen1;

	p1 = VARBITS(arg1);
	p2 = VARBITS(arg2);
	r = VARBITS(result);
	for (i = 0; i < VARBITBYTES(arg1); i++)
		*r++ = *p1++ ^ *p2++;

	/* Padding is not needed as ^ of 0 pads is 0 */

	PG_RETURN_VARBIT_P(result);
}

/*
 * bitnot
 * perform a logical NOT on a bit string.
 */
Datum
bitnot(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);
	VarBit	   *result;
	bits8	   *p,
			   *r;

	result = (VarBit *) palloc(VARSIZE(arg));
	SET_VARSIZE(result, VARSIZE(arg));
	VARBITLEN(result) = VARBITLEN(arg);

	p = VARBITS(arg);
	r = VARBITS(result);
	for (; p < VARBITEND(arg); p++)
		*r++ = ~*p;

	/* Must zero-pad the result, because extra bits are surely 1's here */
	VARBIT_PAD_LAST(result, r);

	PG_RETURN_VARBIT_P(result);
}

/*
 * bitshiftleft
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
	{
		/* Prevent integer overflow in negation */
		if (shft < -VARBITMAXLEN)
			shft = -VARBITMAXLEN;
		PG_RETURN_DATUM(DirectFunctionCall2(bitshiftright,
											VarBitPGetDatum(arg),
											Int32GetDatum(-shft)));
	}

	result = (VarBit *) palloc(VARSIZE(arg));
	SET_VARSIZE(result, VARSIZE(arg));
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

	/* The pad bits should be already zero at this point */

	PG_RETURN_VARBIT_P(result);
}

/*
 * bitshiftright
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
	{
		/* Prevent integer overflow in negation */
		if (shft < -VARBITMAXLEN)
			shft = -VARBITMAXLEN;
		PG_RETURN_DATUM(DirectFunctionCall2(bitshiftleft,
											VarBitPGetDatum(arg),
											Int32GetDatum(-shft)));
	}

	result = (VarBit *) palloc(VARSIZE(arg));
	SET_VARSIZE(result, VARSIZE(arg));
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
		r += len;
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

	/* We may have shifted 1's into the pad bits, so fix that */
	VARBIT_PAD_LAST(result, r);

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

	if (typmod <= 0 || typmod > VARBITMAXLEN)
		typmod = 1;				/* default bit length */

	rlen = VARBITTOTALLEN(typmod);
	result = (VarBit *) palloc(rlen);
	SET_VARSIZE(result, rlen);
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
		unsigned int val = (unsigned int) (a >> (destbitsleft - 8));

		/* Force sign-fill in case the compiler implements >> as zero-fill */
		if (a < 0)
			val |= ((unsigned int) -1) << (srcbitsleft + 8 - destbitsleft);
		*r++ = (bits8) (val & BITMASK);
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

	if (typmod <= 0 || typmod > VARBITMAXLEN)
		typmod = 1;				/* default bit length */

	rlen = VARBITTOTALLEN(typmod);
	result = (VarBit *) palloc(rlen);
	SET_VARSIZE(result, rlen);
	VARBITLEN(result) = typmod;

	r = VARBITS(result);
	destbitsleft = typmod;
	srcbitsleft = 64;
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
		unsigned int val = (unsigned int) (a >> (destbitsleft - 8));

		/* Force sign-fill in case the compiler implements >> as zero-fill */
		if (a < 0)
			val |= ((unsigned int) -1) << (srcbitsleft + 8 - destbitsleft);
		*r++ = (bits8) (val & BITMASK);
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


/*
 * Determines the position of S2 in the bitstring S1 (1-based string).
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


/*
 * bitsetbit
 *
 * Given an instance of type 'bit' creates a new one with
 * the Nth bit set to the given value.
 *
 * The bit location is specified left-to-right in a zero-based fashion
 * consistent with the other get_bit and set_bit functions, but
 * inconsistent with the standard substring, position, overlay functions
 */
Datum
bitsetbit(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	int32		n = PG_GETARG_INT32(1);
	int32		newBit = PG_GETARG_INT32(2);
	VarBit	   *result;
	int			len,
				bitlen;
	bits8	   *r,
			   *p;
	int			byteNo,
				bitNo;

	bitlen = VARBITLEN(arg1);
	if (n < 0 || n >= bitlen)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("bit index %d out of valid range (0..%d)",
						n, bitlen - 1)));

	/*
	 * sanity check!
	 */
	if (newBit != 0 && newBit != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("new bit must be 0 or 1")));

	len = VARSIZE(arg1);
	result = (VarBit *) palloc(len);
	SET_VARSIZE(result, len);
	VARBITLEN(result) = bitlen;

	p = VARBITS(arg1);
	r = VARBITS(result);

	memcpy(r, p, VARBITBYTES(arg1));

	byteNo = n / BITS_PER_BYTE;
	bitNo = BITS_PER_BYTE - 1 - (n % BITS_PER_BYTE);

	/*
	 * Update the byte.
	 */
	if (newBit == 0)
		r[byteNo] &= (~(1 << bitNo));
	else
		r[byteNo] |= (1 << bitNo);

	PG_RETURN_VARBIT_P(result);
}

/*
 * bitgetbit
 *
 * returns the value of the Nth bit of a bit array (0 or 1).
 *
 * The bit location is specified left-to-right in a zero-based fashion
 * consistent with the other get_bit and set_bit functions, but
 * inconsistent with the standard substring, position, overlay functions
 */
Datum
bitgetbit(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	int32		n = PG_GETARG_INT32(1);
	int			bitlen;
	bits8	   *p;
	int			byteNo,
				bitNo;

	bitlen = VARBITLEN(arg1);
	if (n < 0 || n >= bitlen)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("bit index %d out of valid range (0..%d)",
						n, bitlen - 1)));

	p = VARBITS(arg1);

	byteNo = n / BITS_PER_BYTE;
	bitNo = BITS_PER_BYTE - 1 - (n % BITS_PER_BYTE);

	if (p[byteNo] & (1 << bitNo))
		PG_RETURN_INT32(1);
	else
		PG_RETURN_INT32(0);
}
