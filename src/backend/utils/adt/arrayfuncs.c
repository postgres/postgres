/*-------------------------------------------------------------------------
 *
 * arrayfuncs.c
 *	  Support functions for arrays.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/arrayfuncs.c,v 1.69 2001/01/24 19:43:12 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/tupmacs.h"
#include "catalog/catalog.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/memutils.h"
#include "utils/syscache.h"


/*
 * An array has the following internal structure:
 *	  <nbytes>		- total number of bytes
 *	  <ndim>		- number of dimensions of the array
 *	  <flags>		- bit mask of flags
 *	  <dim>			- size of each array axis
 *	  <dim_lower>	- lower boundary of each dimension
 *	  <actual data> - whatever is the stored data
 * The actual data starts on a MAXALIGN boundary.
 *
 * NOTE: it is important that array elements of toastable datatypes NOT be
 * toasted, since the tupletoaster won't know they are there.  (We could
 * support compressed toasted items; only out-of-line items are dangerous.
 * However, it seems preferable to store such items uncompressed and allow
 * the toaster to compress the whole array as one input.)
 */


/* ----------
 * Local definitions
 * ----------
 */
#ifndef MIN
#define MIN(a,b) (((a)<(b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a,b) (((a)>(b)) ? (a) : (b))
#endif

#define ASSGN	 "="

#define RETURN_NULL(type)  do { *isNull = true; return (type) 0; } while (0)


static int	ArrayCount(char *str, int *dim, int typdelim);
static Datum *ReadArrayStr(char *arrayStr, int nitems, int ndim, int *dim,
			  FmgrInfo *inputproc, Oid typelem, int32 typmod,
			  char typdelim, int typlen, bool typbyval,
			  char typalign, int *nbytes);
static void CopyArrayEls(char *p, Datum *values, int nitems,
						 bool typbyval, int typlen, char typalign,
						 bool freedata);
static void system_cache_lookup(Oid element_type, bool input, int *typlen,
								bool *typbyval, char *typdelim, Oid *typelem,
								Oid *proc, char *typalign);
static Datum ArrayCast(char *value, bool byval, int len);
static int	ArrayCastAndSet(Datum src, bool typbyval, int typlen, char *dest);
static int	array_nelems_size(char *ptr, int eltsize, int nitems);
static char *array_seek(char *ptr, int eltsize, int nitems);
static int	array_copy(char *destptr, int eltsize, int nitems, char *srcptr);
static int	array_slice_size(int ndim, int *dim, int *lb, char *arraydataptr,
							 int eltsize, int *st, int *endp);
static void array_extract_slice(int ndim, int *dim, int *lb,
								char *arraydataptr, int eltsize,
								int *st, int *endp, char *destPtr);
static void array_insert_slice(int ndim, int *dim, int *lb,
							   char *origPtr, int origdatasize,
							   char *destPtr, int eltsize,
							   int *st, int *endp, char *srcPtr);


/*---------------------------------------------------------------------
 * array_in :
 *		  converts an array from the external format in "string" to
 *		  its internal format.
 * return value :
 *		  the internal representation of the input array
 *--------------------------------------------------------------------
 */
Datum
array_in(PG_FUNCTION_ARGS)
{
	char	   *string = PG_GETARG_CSTRING(0); /* external form */
	Oid			element_type = PG_GETARG_OID(1); /* type of an array element */
	int32		typmod = PG_GETARG_INT32(2); /* typmod for array elements */
	int			typlen;
	bool		typbyval;
	char		typdelim;
	Oid			typinput;
	Oid			typelem;
	char	   *string_save,
			   *p;
	FmgrInfo	inputproc;
	int			i,
				nitems;
	int32		nbytes;
	Datum	   *dataPtr;
	ArrayType  *retval;
	int			ndim,
				dim[MAXDIM],
				lBound[MAXDIM];
	char		typalign;

	/* Get info about element type, including its input conversion proc */
	system_cache_lookup(element_type, true, &typlen, &typbyval, &typdelim,
						&typelem, &typinput, &typalign);
	fmgr_info(typinput, &inputproc);

	/* Make a modifiable copy of the input */
	/* XXX why are we allocating an extra 2 bytes here? */
	string_save = (char *) palloc(strlen(string) + 3);
	strcpy(string_save, string);

	/*
	 * If the input string starts with dimension info, read and use that.
	 * Otherwise, we require the input to be in curly-brace style, and we
	 * prescan the input to determine dimensions.
	 *
	 * Dimension info takes the form of one or more [n] or [m:n] items.
	 * The outer loop iterates once per dimension item.
	 */
	p = string_save;
	ndim = 0;
	for (;;)
	{
		char	   *q;
		int			ub;

		/*
		 * Note: we currently allow whitespace between, but not within,
		 * dimension items.
		 */
		while (isspace((unsigned char) *p))
			p++;
		if (*p != '[')
			break;				/* no more dimension items */
		p++;
		if (ndim >= MAXDIM)
			elog(ERROR, "array_in: more than %d dimensions", MAXDIM);
		for (q = p; isdigit((unsigned char) *q); q++);
		if (q == p)				/* no digits? */
			elog(ERROR, "array_in: missing dimension value");
		if (*q == ':')
		{
			/* [m:n] format */
			*q = '\0';
			lBound[ndim] = atoi(p);
			p = q + 1;
			for (q = p; isdigit((unsigned char) *q); q++);
			if (q == p)			/* no digits? */
				elog(ERROR, "array_in: missing dimension value");
		}
		else
		{
			/* [n] format */
			lBound[ndim] = 1;
		}
		if (*q != ']')
			elog(ERROR, "array_in: missing ']' in array declaration");
		*q = '\0';
		ub = atoi(p);
		p = q + 1;
		if (ub < lBound[ndim])
			elog(ERROR, "array_in: upper_bound cannot be < lower_bound");
		dim[ndim] = ub - lBound[ndim] + 1;
		ndim++;
	}

	if (ndim == 0)
	{
		/* No array dimensions, so intuit dimensions from brace structure */
		if (*p != '{')
			elog(ERROR, "array_in: Need to specify dimension");
		ndim = ArrayCount(p, dim, typdelim);
		for (i = 0; i < ndim; i++)
			lBound[i] = 1;
	}
	else
	{
		/* If array dimensions are given, expect '=' operator */
		if (strncmp(p, ASSGN, strlen(ASSGN)) != 0)
			elog(ERROR, "array_in: missing assignment operator");
		p += strlen(ASSGN);
		while (isspace((unsigned char) *p))
			p++;
	}

#ifdef ARRAYDEBUG
	printf("array_in- ndim %d (", ndim);
	for (i = 0; i < ndim; i++)
	{
		printf(" %d", dim[i]);
	};
	printf(") for %s\n", string);
#endif

	nitems = ArrayGetNItems(ndim, dim);
	if (nitems == 0)
	{
		/* Return empty array */
		retval = (ArrayType *) palloc(sizeof(ArrayType));
		MemSet(retval, 0, sizeof(ArrayType));
		retval->size = sizeof(ArrayType);
		PG_RETURN_ARRAYTYPE_P(retval);
	}

	if (*p != '{')
		elog(ERROR, "array_in: missing left brace");

	dataPtr = ReadArrayStr(p, nitems, ndim, dim, &inputproc, typelem,
						   typmod, typdelim, typlen, typbyval, typalign,
						   &nbytes);
	nbytes += ARR_OVERHEAD(ndim);
	retval = (ArrayType *) palloc(nbytes);
	MemSet(retval, 0, nbytes);
	retval->size = nbytes;
	retval->ndim = ndim;
	memcpy((char *) ARR_DIMS(retval), (char *) dim,
		   ndim * sizeof(int));
	memcpy((char *) ARR_LBOUND(retval), (char *) lBound,
		   ndim * sizeof(int));

	CopyArrayEls(ARR_DATA_PTR(retval), dataPtr, nitems,
				 typbyval, typlen, typalign, true);
	pfree(dataPtr);
	pfree(string_save);
	PG_RETURN_ARRAYTYPE_P(retval);
}

/*-----------------------------------------------------------------------------
 * ArrayCount
 *	 Counts the number of dimensions and the *dim array for an array string.
 *		 The syntax for array input is C-like nested curly braces
 *-----------------------------------------------------------------------------
 */
static int
ArrayCount(char *str, int *dim, int typdelim)
{
	int			nest_level = 0,
				i;
	int			ndim = 0,
				temp[MAXDIM];
	bool		scanning_string = false;
	bool		eoArray = false;
	char	   *q;

	for (i = 0; i < MAXDIM; ++i)
		temp[i] = dim[i] = 0;

	if (strncmp(str, "{}", 2) == 0)
		return 0;

	q = str;
	while (eoArray != true)
	{
		bool		done = false;

		while (!done)
		{
			switch (*q)
			{
				case '\\':
					/* skip escaped characters (\ and ") inside strings */
					if (scanning_string && *(q + 1))
						q++;
					break;
				case '\0':

					/*
					 * Signal a premature end of the string.  DZ -
					 * 2-9-1996
					 */
					elog(ERROR, "malformed array constant: %s", str);
					break;
				case '\"':
					scanning_string = !scanning_string;
					break;
				case '{':
					if (!scanning_string)
					{
						temp[nest_level] = 0;
						nest_level++;
					}
					break;
				case '}':
					if (!scanning_string)
					{
						if (!ndim)
							ndim = nest_level;
						nest_level--;
						if (nest_level)
							temp[nest_level - 1]++;
						if (nest_level == 0)
							eoArray = done = true;
					}
					break;
				default:
					if (!ndim)
						ndim = nest_level;
					if (*q == typdelim && !scanning_string)
						done = true;
					break;
			}
			if (!done)
				q++;
		}
		temp[ndim - 1]++;
		q++;
		if (!eoArray)
			while (isspace((unsigned char) *q))
				q++;
	}
	for (i = 0; i < ndim; ++i)
		dim[i] = temp[i];

	return ndim;
}

/*---------------------------------------------------------------------------
 * ReadArrayStr :
 *	 parses the array string pointed by "arrayStr" and converts it to
 *	 internal format. The external format expected is like C array
 *	 declaration. Unspecified elements are initialized to zero for fixed length
 *	 base types and to empty varlena structures for variable length base
 *	 types.
 * result :
 *	 returns a palloc'd array of Datum representations of the array elements.
 *	 If element type is pass-by-ref, the Datums point to palloc'd values.
 *	 *nbytes is set to the amount of data space needed for the array,
 *	 including alignment padding but not including array header overhead.
 *---------------------------------------------------------------------------
 */
static Datum *
ReadArrayStr(char *arrayStr,
			 int nitems,
			 int ndim,
			 int *dim,
			 FmgrInfo *inputproc,
			 Oid typelem,
			 int32 typmod,
			 char typdelim,
			 int typlen,
			 bool typbyval,
			 char typalign,
			 int *nbytes)
{
	int			i,
				nest_level = 0;
	Datum	   *values;
	char	   *p,
			   *q,
			   *r;
	bool		scanning_string = false;
	int			indx[MAXDIM],
				prod[MAXDIM];
	bool		eoArray = false;

	mda_get_prod(ndim, dim, prod);
	values = (Datum *) palloc(nitems * sizeof(Datum));
	MemSet(values, 0, nitems * sizeof(Datum));
	MemSet(indx, 0, sizeof(indx));
	q = p = arrayStr;

	/* read array enclosed within {} */
	while (!eoArray)
	{
		bool		done = false;
		int			i = -1;

		while (!done)
		{
			switch (*q)
			{
				case '\\':
					/* Crunch the string on top of the backslash. */
					for (r = q; *r != '\0'; r++)
						*r = *(r + 1);
					break;
				case '\"':
					if (!scanning_string)
					{
						while (p != q)
							p++;
						p++;	/* get p past first doublequote */
					}
					else
						*q = '\0';
					scanning_string = !scanning_string;
					break;
				case '{':
					if (!scanning_string)
					{
						p++;
						nest_level++;
						if (nest_level > ndim)
							elog(ERROR, "array_in: illformed array constant");
						indx[nest_level - 1] = 0;
						indx[ndim - 1] = 0;
					}
					break;
				case '}':
					if (!scanning_string)
					{
						if (i == -1)
							i = ArrayGetOffset0(ndim, indx, prod);
						nest_level--;
						if (nest_level == 0)
							eoArray = done = true;
						else
						{
							*q = '\0';
							indx[nest_level - 1]++;
						}
					}
					break;
				default:
					if (*q == typdelim && !scanning_string)
					{
						if (i == -1)
							i = ArrayGetOffset0(ndim, indx, prod);
						done = true;
						indx[ndim - 1]++;
					}
					break;
			}
			if (!done)
				q++;
		}
		*q = '\0';
		if (i >= nitems)
			elog(ERROR, "array_in: illformed array constant");
		values[i] = FunctionCall3(inputproc,
								  CStringGetDatum(p),
								  ObjectIdGetDatum(typelem),
								  Int32GetDatum(typmod));
		p = ++q;
		/*
		 * if not at the end of the array skip white space
		 */
		if (!eoArray)
			while (isspace((unsigned char) *q))
			{
				p++;
				q++;
			}
	}
	/*
	 * Initialize any unset items and compute total data space needed
	 */
	if (typlen > 0)
	{
		*nbytes = nitems * typlen;
		if (!typbyval)
			for (i = 0; i < nitems; i++)
				if (values[i] == (Datum) 0)
				{
					values[i] = PointerGetDatum(palloc(typlen));
					MemSet(DatumGetPointer(values[i]), 0, typlen);
				}
	}
	else
	{
		*nbytes = 0;
		for (i = 0; i < nitems; i++)
		{
			if (values[i] != (Datum) 0)
			{
				/* let's just make sure data is not toasted */
				values[i] = PointerGetDatum(PG_DETOAST_DATUM(values[i]));
				if (typalign == 'd')
					*nbytes += MAXALIGN(VARSIZE(DatumGetPointer(values[i])));
				else
					*nbytes += INTALIGN(VARSIZE(DatumGetPointer(values[i])));
			}
			else
			{
				*nbytes += sizeof(int32);
				values[i] = PointerGetDatum(palloc(sizeof(int32)));
				VARATT_SIZEP(DatumGetPointer(values[i])) = sizeof(int32);
			}
		}
	}
	return values;
}


/*----------
 * Copy data into an array object from a temporary array of Datums.
 *
 * p: pointer to start of array data area
 * values: array of Datums to be copied
 * nitems: number of Datums to be copied
 * typbyval, typlen, typalign: info about element datatype
 * freedata: if TRUE and element type is pass-by-ref, pfree data values
 * referenced by Datums after copying them.
 *
 * If the input data is of varlena type, the caller must have ensured that
 * the values are not toasted.  (Doing it here doesn't work since the 
 * caller has already allocated space for the array...)
 *----------
 */
static void
CopyArrayEls(char *p,
			 Datum *values,
			 int nitems,
			 bool typbyval,
			 int typlen,
			 char typalign,
			 bool freedata)
{
	int			i;
	int			inc;

	if (typbyval)
		freedata = false;

	for (i = 0; i < nitems; i++)
	{
		inc = ArrayCastAndSet(values[i], typbyval, typlen, p);
		p += inc;
		if (freedata)
			pfree(DatumGetPointer(values[i]));
	}
}

/*-------------------------------------------------------------------------
 * array_out :
 *		   takes the internal representation of an array and returns a string
 *		  containing the array in its external format.
 *-------------------------------------------------------------------------
 */
Datum
array_out(PG_FUNCTION_ARGS)
{
	ArrayType  *v = PG_GETARG_ARRAYTYPE_P(0);
	Oid			element_type = PG_GETARG_OID(1);
	int			typlen;
	bool		typbyval;
	char		typdelim;
	Oid			typoutput,
				typelem;
	FmgrInfo	outputproc;
	char		typalign;
	char	   *p,
			   *tmp,
			   *retval,
			  **values,
				delim[2];
	int			nitems,
				overall_length,
				i,
				j,
				k,
#ifndef TCL_ARRAYS
				l,
#endif
				indx[MAXDIM];
	int			ndim,
			   *dim;

	system_cache_lookup(element_type, false, &typlen, &typbyval,
						&typdelim, &typelem, &typoutput, &typalign);
	fmgr_info(typoutput, &outputproc);
	sprintf(delim, "%c", typdelim);
	ndim = ARR_NDIM(v);
	dim = ARR_DIMS(v);
	nitems = ArrayGetNItems(ndim, dim);

	if (nitems == 0)
	{
		retval = (char *) palloc(3);
		retval[0] = '{';
		retval[1] = '}';
		retval[2] = '\0';
		PG_RETURN_CSTRING(retval);
	}

	p = ARR_DATA_PTR(v);
	overall_length = 1;			/* [TRH] don't forget to count \0 at end. */
	values = (char **) palloc(nitems * sizeof(char *));
	for (i = 0; i < nitems; i++)
	{
		Datum		itemvalue;

		itemvalue = fetch_att(p, typbyval, typlen);
		values[i] = DatumGetCString(FunctionCall3(&outputproc,
												  itemvalue,
												  ObjectIdGetDatum(typelem),
												  Int32GetDatum(-1)));
		if (typlen > 0)
			p += typlen;
		else
			p += INTALIGN(*(int32 *) p);

		/*
		 * For the pair of double quotes
		 */
		if (!typbyval)
			overall_length += 2;

		for (tmp = values[i]; *tmp; tmp++)
		{
			overall_length += 1;
#ifndef TCL_ARRAYS
			if (*tmp == '"')
				overall_length += 1;
#endif
		}
		overall_length += 1;
	}

	/*
	 * count total number of curly braces in output string
	 */
	for (i = j = 0, k = 1; i < ndim; k *= dim[i++], j += k);

	p = (char *) palloc(overall_length + 2 * j);
	retval = p;

	strcpy(p, "{");
	for (i = 0; i < ndim; indx[i++] = 0);
	j = 0;
	k = 0;
	do
	{
		for (i = j; i < ndim - 1; i++)
			strcat(p, "{");

		/*
		 * Surround anything that is not passed by value in double quotes.
		 * See above for more details.
		 */
		if (!typbyval)
		{
			strcat(p, "\"");
#ifndef TCL_ARRAYS
			l = strlen(p);
			for (tmp = values[k]; *tmp; tmp++)
			{
				if (*tmp == '"')
					p[l++] = '\\';
				p[l++] = *tmp;
			}
			p[l] = '\0';
#else
			strcat(p, values[k]);
#endif
			strcat(p, "\"");
		}
		else
			strcat(p, values[k]);
		pfree(values[k++]);

		for (i = ndim - 1; i >= 0; i--)
		{
			indx[i] = (indx[i] + 1) % dim[i];
			if (indx[i])
			{
				strcat(p, delim);
				break;
			}
			else
				strcat(p, "}");
		}
		j = i;
	} while (j != -1);

	pfree(values);
	PG_RETURN_CSTRING(retval);
}

/*-----------------------------------------------------------------------------
 * array_dims :
 *		  returns the dimensions of the array pointed to by "v", as a "text"
 *----------------------------------------------------------------------------
 */
Datum
array_dims(PG_FUNCTION_ARGS)
{
	ArrayType  *v = PG_GETARG_ARRAYTYPE_P(0);
	text	   *result;
	char	   *p;
	int			nbytes,
				i;
	int		   *dimv,
			   *lb;

	/* Sanity check: does it look like an array at all? */
	if (ARR_NDIM(v) <= 0 || ARR_NDIM(v) > MAXDIM)
		PG_RETURN_NULL();

	nbytes = ARR_NDIM(v) * 33 + 1;
	/*
	 * 33 since we assume 15 digits per number + ':' +'[]'
	 *
	 * +1 allows for temp trailing null
	 */

	result = (text *) palloc(nbytes + VARHDRSZ);
	p = VARDATA(result);

	dimv = ARR_DIMS(v);
	lb = ARR_LBOUND(v);

	for (i = 0; i < ARR_NDIM(v); i++)
	{
		sprintf(p, "[%d:%d]", lb[i], dimv[i] + lb[i] - 1);
		p += strlen(p);
	}
	VARATT_SIZEP(result) = strlen(VARDATA(result)) + VARHDRSZ;

	PG_RETURN_TEXT_P(result);
}

/*---------------------------------------------------------------------------
 * array_ref :
 *	  This routine takes an array pointer and an index array and returns
 *	  the referenced item as a Datum.  Note that for a pass-by-reference
 *	  datatype, the returned Datum is a pointer into the array object.
 *---------------------------------------------------------------------------
 */
Datum
array_ref(ArrayType *array,
		  int nSubscripts,
		  int *indx,
		  bool elmbyval,
		  int elmlen,
		  int arraylen,
		  bool *isNull)
{
	int			i,
				ndim,
			   *dim,
			   *lb,
				offset,
				fixedDim[1],
				fixedLb[1];
	char	   *arraydataptr,
			   *retptr;

	if (array == (ArrayType *) NULL)
		RETURN_NULL(Datum);

	if (arraylen > 0)
	{
		/*
		 * fixed-length arrays -- these are assumed to be 1-d, 0-based
		 */
		ndim = 1;
		fixedDim[0] = arraylen / elmlen;
		fixedLb[0] = 0;
		dim = fixedDim;
		lb = fixedLb;
		arraydataptr = (char *) array;
	}
	else
	{
		/* detoast input if necessary */
		array = DatumGetArrayTypeP(PointerGetDatum(array));

		ndim = ARR_NDIM(array);
		dim = ARR_DIMS(array);
		lb = ARR_LBOUND(array);
		arraydataptr = ARR_DATA_PTR(array);
	}

	/*
	 * Return NULL for invalid subscript
	 */
	if (ndim != nSubscripts || ndim <= 0 || ndim > MAXDIM)
		RETURN_NULL(Datum);
	for (i = 0; i < ndim; i++)
		if (indx[i] < lb[i] || indx[i] >= (dim[i] + lb[i]))
			RETURN_NULL(Datum);

	/*
	 * OK, get the element
	 */
	offset = ArrayGetOffset(nSubscripts, dim, lb, indx);

	retptr = array_seek(arraydataptr, elmlen, offset);

	return ArrayCast(retptr, elmbyval, elmlen);
}

/*-----------------------------------------------------------------------------
 * array_get_slice :
 *		   This routine takes an array and a range of indices (upperIndex and
 *		   lowerIndx), creates a new array structure for the referred elements
 *		   and returns a pointer to it.
 *
 * NOTE: we assume it is OK to scribble on the provided index arrays
 * lowerIndx[] and upperIndx[].  These are generally just temporaries.
 *-----------------------------------------------------------------------------
 */
ArrayType *
array_get_slice(ArrayType *array,
				int nSubscripts,
				int *upperIndx,
				int *lowerIndx,
				bool elmbyval,
				int elmlen,
				int arraylen,
				bool *isNull)
{
	int			i,
				ndim,
			   *dim,
			   *lb;
	int			fixedDim[1],
				fixedLb[1];
	char	   *arraydataptr;
	ArrayType  *newarray;
	int			bytes,
				span[MAXDIM];

	if (array == (ArrayType *) NULL)
		RETURN_NULL(ArrayType *);

	if (arraylen > 0)
	{
		/*
		 * fixed-length arrays -- currently, cannot slice these because
		 * parser labels output as being of the fixed-length array type!
		 * Code below shows how we could support it if the parser were
		 * changed to label output as a suitable varlena array type.
		 */
		elog(ERROR, "Slices of fixed-length arrays not implemented");

		/*
		 * fixed-length arrays -- these are assumed to be 1-d, 0-based
		 */
		ndim = 1;
		fixedDim[0] = arraylen / elmlen;
		fixedLb[0] = 0;
		dim = fixedDim;
		lb = fixedLb;
		arraydataptr = (char *) array;
	}
	else
	{
		/* detoast input if necessary */
		array = DatumGetArrayTypeP(PointerGetDatum(array));

		ndim = ARR_NDIM(array);
		dim = ARR_DIMS(array);
		lb = ARR_LBOUND(array);
		arraydataptr = ARR_DATA_PTR(array);
	}

	/*
	 * Check provided subscripts.  A slice exceeding the current array
	 * limits is silently truncated to the array limits.  If we end up with
	 * an empty slice, return NULL (should it be an empty array instead?)
	 */
	if (ndim != nSubscripts || ndim <= 0 || ndim > MAXDIM)
		RETURN_NULL(ArrayType *);

	for (i = 0; i < ndim; i++)
	{
		if (lowerIndx[i] < lb[i])
			lowerIndx[i] = lb[i];
		if (upperIndx[i] >= (dim[i] + lb[i]))
			upperIndx[i] = dim[i] + lb[i] - 1;
		if (lowerIndx[i] > upperIndx[i])
			RETURN_NULL(ArrayType *);
	}

	mda_get_range(nSubscripts, span, lowerIndx, upperIndx);

	bytes = array_slice_size(ndim, dim, lb, arraydataptr,
							 elmlen, lowerIndx, upperIndx);
	bytes += ARR_OVERHEAD(ndim);

	newarray = (ArrayType *) palloc(bytes);
	newarray->size = bytes;
	newarray->ndim = ndim;
	newarray->flags = 0;
	memcpy(ARR_DIMS(newarray), span, ndim * sizeof(int));
	memcpy(ARR_LBOUND(newarray), lowerIndx, ndim * sizeof(int));
	array_extract_slice(ndim, dim, lb, arraydataptr, elmlen,
						lowerIndx, upperIndx, ARR_DATA_PTR(newarray));

	return newarray;
}

/*-----------------------------------------------------------------------------
 * array_set :
 *		  This routine sets the value of an array location (specified by
 *		  an index array) to a new value specified by "dataValue".
 * result :
 *		  A new array is returned, just like the old except for the one
 *		  modified entry.
 *
 * For one-dimensional arrays only, we allow the array to be extended
 * by assigning to the position one above or one below the existing range.
 * (We could be more flexible if we had a way to represent NULL elements.)
 *
 * NOTE: For assignments, we throw an error for invalid subscripts etc,
 * rather than returning a NULL as the fetch operations do.  The reasoning
 * is that returning a NULL would cause the user's whole array to be replaced
 * with NULL, which will probably not make him happy.
 *-----------------------------------------------------------------------------
 */
ArrayType *
array_set(ArrayType *array,
		  int nSubscripts,
		  int *indx,
		  Datum dataValue,
		  bool elmbyval,
		  int elmlen,
		  int arraylen,
		  bool *isNull)
{
	int			i,
				ndim,
				dim[MAXDIM],
				lb[MAXDIM],
				offset;
	ArrayType  *newarray;
	char	   *elt_ptr;
	bool		extendbefore = false;
	bool		extendafter = false;
	int			olddatasize,
				newsize,
				olditemlen,
				newitemlen,
				overheadlen,
				lenbefore,
				lenafter;

	if (array == (ArrayType *) NULL)
		RETURN_NULL(ArrayType *);

	if (arraylen > 0)
	{
		/*
		 * fixed-length arrays -- these are assumed to be 1-d, 0-based.
		 * We cannot extend them, either.
		 */
		if (nSubscripts != 1)
			elog(ERROR, "Invalid array subscripts");
		if (indx[0] < 0 || indx[0] * elmlen >= arraylen)
			elog(ERROR, "Invalid array subscripts");
		newarray = (ArrayType *) palloc(arraylen);
		memcpy(newarray, array, arraylen);
		elt_ptr = (char *) newarray + indx[0] * elmlen;
		ArrayCastAndSet(dataValue, elmbyval, elmlen, elt_ptr);
		return newarray;
	}

	/* make sure item to be inserted is not toasted */
	if (elmlen < 0)
		dataValue = PointerGetDatum(PG_DETOAST_DATUM(dataValue));

	/* detoast input if necessary */
	array = DatumGetArrayTypeP(PointerGetDatum(array));

	ndim = ARR_NDIM(array);
	if (ndim != nSubscripts || ndim <= 0 || ndim > MAXDIM)
		elog(ERROR, "Invalid array subscripts");

	/* copy dim/lb since we may modify them */
	memcpy(dim, ARR_DIMS(array), ndim * sizeof(int));
	memcpy(lb, ARR_LBOUND(array), ndim * sizeof(int));

	/*
	 * Check subscripts
	 */
	for (i = 0; i < ndim; i++)
	{
		if (indx[i] < lb[i])
		{
			if (ndim == 1 && indx[i] == lb[i] - 1)
			{
				dim[i]++;
				lb[i]--;
				extendbefore = true;
			}
			else
			{
				elog(ERROR, "Invalid array subscripts");
			}
		}
		if (indx[i] >= (dim[i] + lb[i]))
		{
			if (ndim == 1 && indx[i] == (dim[i] + lb[i]))
			{
				dim[i]++;
				extendafter = true;
			}
			else
			{
				elog(ERROR, "Invalid array subscripts");
			}
		}
	}

	/*
	 * Compute sizes of items and areas to copy
	 */
	overheadlen = ARR_OVERHEAD(ndim);
	olddatasize = ARR_SIZE(array) - overheadlen;
	if (extendbefore)
	{
		lenbefore = 0;
		olditemlen = 0;
		lenafter = olddatasize;
	}
	else if (extendafter)
	{
		lenbefore = olddatasize;
		olditemlen = 0;
		lenafter = 0;
	}
	else
	{
		offset = ArrayGetOffset(nSubscripts, dim, lb, indx);
		elt_ptr = array_seek(ARR_DATA_PTR(array), elmlen, offset);
		lenbefore = (int) (elt_ptr - ARR_DATA_PTR(array));
		if (elmlen > 0)
			olditemlen = elmlen;
		else
			olditemlen = INTALIGN(*(int32 *) elt_ptr);
		lenafter = (int) (olddatasize - lenbefore - olditemlen);
	}

	if (elmlen > 0)
		newitemlen = elmlen;
	else
		newitemlen = INTALIGN(*(int32 *) DatumGetPointer(dataValue));

	newsize = overheadlen + lenbefore + newitemlen + lenafter;

	/*
	 * OK, do the assignment
	 */
	newarray = (ArrayType *) palloc(newsize);
	newarray->size = newsize;
	newarray->ndim = ndim;
	newarray->flags = 0;
	memcpy(ARR_DIMS(newarray), dim, ndim * sizeof(int));
	memcpy(ARR_LBOUND(newarray), lb, ndim * sizeof(int));
	memcpy((char *) newarray + overheadlen,
		   (char *) array + overheadlen,
		   lenbefore);
	memcpy((char *) newarray + overheadlen + lenbefore + newitemlen,
		   (char *) array + overheadlen + lenbefore + olditemlen,
		   lenafter);

	ArrayCastAndSet(dataValue, elmbyval, elmlen,
					(char *) newarray + overheadlen + lenbefore);

	return newarray;
}

/*----------------------------------------------------------------------------
 * array_set_slice :
 *		  This routine sets the value of a range of array locations (specified
 *		  by upper and lower index values ) to new values passed as
 *		  another array
 * result :
 *		  A new array is returned, just like the old except for the
 *		  modified range.
 *
 * NOTE: For assignments, we throw an error for silly subscripts etc,
 * rather than returning a NULL as the fetch operations do.  The reasoning
 * is that returning a NULL would cause the user's whole array to be replaced
 * with NULL, which will probably not make him happy.
 *----------------------------------------------------------------------------
 */
ArrayType *
array_set_slice(ArrayType *array,
				int nSubscripts,
				int *upperIndx,
				int *lowerIndx,
				ArrayType *srcArray,
				bool elmbyval,
				int elmlen,
				int arraylen,
				bool *isNull)
{
	int			i,
				ndim,
				dim[MAXDIM],
				lb[MAXDIM],
				span[MAXDIM];
	ArrayType  *newarray;
	int			nsrcitems,
				olddatasize,
				newsize,
				olditemsize,
				newitemsize,
				overheadlen,
				lenbefore,
				lenafter;

	if (array == (ArrayType *) NULL)
		RETURN_NULL(ArrayType *);
	if (srcArray == (ArrayType *) NULL)
		return array;

	if (arraylen > 0)
	{
		/*
		 * fixed-length arrays -- not got round to doing this...
		 */
		elog(ERROR, "Updates on slices of fixed-length arrays not implemented");
	}

	/* detoast arrays if necessary */
	array = DatumGetArrayTypeP(PointerGetDatum(array));
	srcArray = DatumGetArrayTypeP(PointerGetDatum(srcArray));

	/* note: we assume srcArray contains no toasted elements */

	ndim = ARR_NDIM(array);
	if (ndim != nSubscripts || ndim <= 0 || ndim > MAXDIM)
		elog(ERROR, "Invalid array subscripts");

	/* copy dim/lb since we may modify them */
	memcpy(dim, ARR_DIMS(array), ndim * sizeof(int));
	memcpy(lb, ARR_LBOUND(array), ndim * sizeof(int));

	/*
	 * Check provided subscripts.  A slice exceeding the current array
	 * limits throws an error, *except* in the 1-D case where we will
	 * extend the array as long as no hole is created.
	 * An empty slice is an error, too.
	 */
	for (i = 0; i < ndim; i++)
	{
		if (lowerIndx[i] > upperIndx[i])
			elog(ERROR, "Invalid array subscripts");
		if (lowerIndx[i] < lb[i])
		{
			if (ndim == 1 && upperIndx[i] >= lb[i] - 1)
			{
				dim[i] += lb[i] - lowerIndx[i];
				lb[i] = lowerIndx[i];
			}
			else
			{
				elog(ERROR, "Invalid array subscripts");
			}
		}
		if (upperIndx[i] >= (dim[i] + lb[i]))
		{
			if (ndim == 1 && lowerIndx[i] <= (dim[i] + lb[i]))
			{
				dim[i] = upperIndx[i] - lb[i] + 1;
			}
			else
			{
				elog(ERROR, "Invalid array subscripts");
			}
		}
	}

	/*
	 * Make sure source array has enough entries.  Note we ignore the shape
	 * of the source array and just read entries serially.
	 */
	mda_get_range(ndim, span, lowerIndx, upperIndx);
	nsrcitems = ArrayGetNItems(ndim, span);
	if (nsrcitems > ArrayGetNItems(ARR_NDIM(srcArray), ARR_DIMS(srcArray)))
		elog(ERROR, "Source array too small");

	/*
	 * Compute space occupied by new entries, space occupied by replaced
	 * entries, and required space for new array.
	 */
	newitemsize = array_nelems_size(ARR_DATA_PTR(srcArray), elmlen,
									nsrcitems);
	overheadlen = ARR_OVERHEAD(ndim);
	olddatasize = ARR_SIZE(array) - overheadlen;
	if (ndim > 1)
	{
		/*
		 * here we do not need to cope with extension of the array;
		 * it would be a lot more complicated if we had to do so...
		 */
		olditemsize = array_slice_size(ndim, dim, lb, ARR_DATA_PTR(array),
									   elmlen, lowerIndx, upperIndx);
		lenbefore = lenafter = 0; /* keep compiler quiet */
	}
	else
	{
		/*
		 * here we must allow for possibility of slice larger than orig array
		 */
		int		oldlb = ARR_LBOUND(array)[0];
		int		oldub = oldlb + ARR_DIMS(array)[0] - 1;
		int		slicelb = MAX(oldlb, lowerIndx[0]);
		int		sliceub = MIN(oldub, upperIndx[0]);
		char   *oldarraydata = ARR_DATA_PTR(array);

		lenbefore = array_nelems_size(oldarraydata,
									  elmlen,
									  slicelb - oldlb);
		if (slicelb > sliceub)
			olditemsize = 0;
		else
			olditemsize = array_nelems_size(oldarraydata + lenbefore,
											elmlen,
											sliceub - slicelb + 1);
		lenafter = olddatasize - lenbefore - olditemsize;
	}

	newsize = overheadlen + olddatasize - olditemsize + newitemsize;

	newarray = (ArrayType *) palloc(newsize);
	newarray->size = newsize;
	newarray->ndim = ndim;
	newarray->flags = 0;
	memcpy(ARR_DIMS(newarray), dim, ndim * sizeof(int));
	memcpy(ARR_LBOUND(newarray), lb, ndim * sizeof(int));

	if (ndim > 1)
	{
		/*
		 * here we do not need to cope with extension of the array;
		 * it would be a lot more complicated if we had to do so...
		 */
		array_insert_slice(ndim, dim, lb, ARR_DATA_PTR(array), olddatasize,
						   ARR_DATA_PTR(newarray), elmlen,
						   lowerIndx, upperIndx, ARR_DATA_PTR(srcArray));
	}
	else
	{
		memcpy((char *) newarray + overheadlen,
			   (char *) array + overheadlen,
			   lenbefore);
		memcpy((char *) newarray + overheadlen + lenbefore,
			   ARR_DATA_PTR(srcArray),
			   newitemsize);
		memcpy((char *) newarray + overheadlen + lenbefore + newitemsize,
			   (char *) array + overheadlen + lenbefore + olditemsize,
			   lenafter);
	}

	return newarray;
}

/*
 * array_map()
 *
 * Map an array through an arbitrary function.  Return a new array with
 * same dimensions and each source element transformed by fn().  Each
 * source element is passed as the first argument to fn(); additional
 * arguments to be passed to fn() can be specified by the caller.
 * The output array can have a different element type than the input.
 *
 * Parameters are:
 * * fcinfo: a function-call data structure pre-constructed by the caller
 *   to be ready to call the desired function, with everything except the
 *   first argument position filled in.  In particular, flinfo identifies
 *   the function fn(), and if nargs > 1 then argument positions after the
 *   first must be preset to the additional values to be passed.  The
 *   first argument position initially holds the input array value.
 * * inpType: OID of element type of input array.  This must be the same as,
 *   or binary-compatible with, the first argument type of fn().
 * * retType: OID of element type of output array.  This must be the same as,
 *   or binary-compatible with, the result type of fn().
 *
 * NB: caller must assure that input array is not NULL.  Currently,
 * any additional parameters passed to fn() may not be specified as NULL
 * either.
 */
Datum
array_map(FunctionCallInfo fcinfo, Oid inpType, Oid retType)
{
	ArrayType  *v;
	ArrayType  *result;
	Datum	   *values;
	Datum		elt;
	int		   *dim;
	int			ndim;
	int			nitems;
	int			i;
	int			nbytes = 0;
	int			inp_typlen;
	bool		inp_typbyval;
	int			typlen;
	bool		typbyval;
	char		typdelim;
	Oid			typelem;
	Oid			proc;
	char		typalign;
	char	   *s;

	/* Get input array */
	if (fcinfo->nargs < 1)
		elog(ERROR, "array_map: invalid nargs: %d", fcinfo->nargs);
	if (PG_ARGISNULL(0))
		elog(ERROR, "array_map: null input array");
	v = PG_GETARG_ARRAYTYPE_P(0);

	ndim = ARR_NDIM(v);
	dim = ARR_DIMS(v);
	nitems = ArrayGetNItems(ndim, dim);

	/* Check for empty array */
	if (nitems <= 0)
		PG_RETURN_ARRAYTYPE_P(v);

	/* Lookup source and result types. Unneeded variables are reused. */
	system_cache_lookup(inpType, false, &inp_typlen, &inp_typbyval,
						&typdelim, &typelem, &proc, &typalign);
	system_cache_lookup(retType, false, &typlen, &typbyval,
						&typdelim, &typelem, &proc, &typalign);

	/* Allocate temporary array for new values */
	values = (Datum *) palloc(nitems * sizeof(Datum));

	/* Loop over source data */
	s = (char *) ARR_DATA_PTR(v);
	for (i = 0; i < nitems; i++)
	{
		/* Get source element */
		elt = fetch_att(s, inp_typbyval, inp_typlen);

		if (inp_typlen > 0)
			s += inp_typlen;
		else
			s += INTALIGN(*(int32 *) s);

		/*
		 * Apply the given function to source elt and extra args.
		 *
		 * We assume the extra args are non-NULL, so need not check
		 * whether fn() is strict.  Would need to do more work here
		 * to support arrays containing nulls, too.
		 */
		fcinfo->arg[0] = elt;
		fcinfo->argnull[0] = false;
		fcinfo->isnull = false;
		values[i] = FunctionCallInvoke(fcinfo);
		if (fcinfo->isnull)
			elog(ERROR, "array_map: cannot handle NULL in array");

		/* Ensure data is not toasted, and update total result size */
		if (typbyval || typlen > 0)
			nbytes += typlen;
		else
		{
			values[i] = PointerGetDatum(PG_DETOAST_DATUM(values[i]));
			nbytes += INTALIGN(VARSIZE(DatumGetPointer(values[i])));
		}
	}

	/* Allocate and initialize the result array */
	nbytes += ARR_OVERHEAD(ndim);
	result = (ArrayType *) palloc(nbytes);
	MemSet(result, 0, nbytes);

	result->size = nbytes;
	result->ndim = ndim;
	memcpy(ARR_DIMS(result), ARR_DIMS(v), 2 * ndim * sizeof(int));

	/* Note: do not risk trying to pfree the results of the called function */
	CopyArrayEls(ARR_DATA_PTR(result), values, nitems,
				 typbyval, typlen, typalign, false);
	pfree(values);

	PG_RETURN_ARRAYTYPE_P(result);
}

/*----------
 * construct_array  --- simple method for constructing an array object
 *
 * elems: array of Datum items to become the array contents
 * nelems: number of items
 * elmbyval, elmlen, elmalign: info for the datatype of the items
 *
 * A palloc'd 1-D array object is constructed and returned.  Note that
 * elem values will be copied into the object even if pass-by-ref type.
 * NULL element values are not supported.
 *----------
 */
ArrayType *
construct_array(Datum *elems, int nelems,
				bool elmbyval, int elmlen, char elmalign)
{
	ArrayType  *result;
	int			nbytes;
	int			i;

	if (elmlen > 0)
	{
		/* XXX what about alignment? */
		nbytes = elmlen * nelems;
	}
	else
	{
		/* varlena type ... make sure it is untoasted */
		nbytes = 0;
		for (i = 0; i < nelems; i++)
		{
			elems[i] = PointerGetDatum(PG_DETOAST_DATUM(elems[i]));
			nbytes += INTALIGN(VARSIZE(DatumGetPointer(elems[i])));
		}
	}

	/* Allocate and initialize 1-D result array */
	nbytes += ARR_OVERHEAD(1);
	result = (ArrayType *) palloc(nbytes);

	result->size = nbytes;
	result->ndim = 1;
	result->flags = 0;
	ARR_DIMS(result)[0] = nelems;
	ARR_LBOUND(result)[0] = 1;

	CopyArrayEls(ARR_DATA_PTR(result), elems, nelems,
				 elmbyval, elmlen, elmalign, false);

	return result;
}

/*----------
 * deconstruct_array  --- simple method for extracting data from an array
 *
 * array: array object to examine (must not be NULL)
 * elmbyval, elmlen, elmalign: info for the datatype of the items
 * elemsp: return value, set to point to palloc'd array of Datum values
 * nelemsp: return value, set to number of extracted values
 *
 * If array elements are pass-by-ref data type, the returned Datums will
 * be pointers into the array object.
 *----------
 */
void
deconstruct_array(ArrayType *array,
				  bool elmbyval, int elmlen, char elmalign,
				  Datum **elemsp, int *nelemsp)
{
	Datum	   *elems;
	int			nelems;
	char	   *p;
	int			i;

	nelems = ArrayGetNItems(ARR_NDIM(array), ARR_DIMS(array));
	if (nelems <= 0)
	{
		*elemsp = NULL;
		*nelemsp = 0;
		return;
	}
	*elemsp = elems = (Datum *) palloc(nelems * sizeof(Datum));
	*nelemsp = nelems;

	p = ARR_DATA_PTR(array);
	for (i = 0; i < nelems; i++)
	{
		elems[i] = fetch_att(p, elmbyval, elmlen);
		if (elmlen > 0)
			p += elmlen;
		else
			p += INTALIGN(VARSIZE(p));
	}
}


/*-----------------------------------------------------------------------------
 * array_eq :
 *		  compares two arrays for equality
 * result :
 *		  returns true if the arrays are equal, false otherwise.
 *
 * XXX bitwise equality is pretty bogus ...
 *-----------------------------------------------------------------------------
 */
Datum
array_eq(PG_FUNCTION_ARGS)
{
	ArrayType  *array1 = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType  *array2 = PG_GETARG_ARRAYTYPE_P(1);
	bool		result = true;

	if (ARR_SIZE(array1) != ARR_SIZE(array2))
		result = false;
	else if (memcmp(array1, array2, ARR_SIZE(array1)) != 0)
		result = false;

	/* Avoid leaking memory when handed toasted input. */
	PG_FREE_IF_COPY(array1, 0);
	PG_FREE_IF_COPY(array2, 1);

	PG_RETURN_BOOL(result);
}


/***************************************************************************/
/******************|		  Support  Routines			  |*****************/
/***************************************************************************/

static void
system_cache_lookup(Oid element_type,
					bool input,
					int *typlen,
					bool *typbyval,
					char *typdelim,
					Oid *typelem,
					Oid *proc,
					char *typalign)
{
	HeapTuple	typeTuple;
	Form_pg_type typeStruct;

	typeTuple = SearchSysCache(TYPEOID,
							   ObjectIdGetDatum(element_type),
							   0, 0, 0);
	if (!HeapTupleIsValid(typeTuple))
		elog(ERROR, "array_out: Cache lookup failed for type %u",
			 element_type);
	typeStruct = (Form_pg_type) GETSTRUCT(typeTuple);

	*typlen = typeStruct->typlen;
	*typbyval = typeStruct->typbyval;
	*typdelim = typeStruct->typdelim;
	*typelem = typeStruct->typelem;
	*typalign = typeStruct->typalign;
	if (input)
		*proc = typeStruct->typinput;
	else
		*proc = typeStruct->typoutput;
	ReleaseSysCache(typeTuple);
}

/*
 * Fetch array element at pointer, converted correctly to a Datum
 */
static Datum
ArrayCast(char *value, bool byval, int len)
{
	return fetch_att(value, byval, len);
}

/*
 * Copy datum to *dest and return total space used (including align padding)
 *
 * XXX this routine needs to be told typalign too!
 */
static int
ArrayCastAndSet(Datum src,
				bool typbyval,
				int typlen,
				char *dest)
{
	int			inc;

	if (typlen > 0)
	{
		if (typbyval)
		{
			store_att_byval(dest, src, typlen);
			/* For by-val types, assume no alignment padding is needed */
			inc = typlen;
		}
		else
		{
			memmove(dest, DatumGetPointer(src), typlen);
			/* XXX WRONG: need to consider type's alignment requirement */
			inc = typlen;
		}
	}
	else
	{
		/* varlena type */
		memmove(dest, DatumGetPointer(src), VARSIZE(DatumGetPointer(src)));
		/* XXX WRONG: should use MAXALIGN or type's alignment requirement */
		inc = INTALIGN(VARSIZE(DatumGetPointer(src)));
	}

	return inc;
}

/*
 * Compute total size of the nitems array elements starting at *ptr
 *
 * XXX should consider alignment spec for fixed-length types
 */
static int
array_nelems_size(char *ptr, int eltsize, int nitems)
{
	char	   *origptr;
	int			i;

	/* fixed-size elements? */
	if (eltsize > 0)
		return eltsize * nitems;
	/* else assume they are varlena items */
	origptr = ptr;
	for (i = 0; i < nitems; i++)
		ptr += INTALIGN(*(int32 *) ptr);
	return ptr - origptr;
}

/*
 * Advance ptr over nitems array elements
 */
static char *
array_seek(char *ptr, int eltsize, int nitems)
{
	return ptr + array_nelems_size(ptr, eltsize, nitems);
}

/*
 * Copy nitems array elements from srcptr to destptr
 *
 * Returns number of bytes copied
 */
static int
array_copy(char *destptr, int eltsize, int nitems, char *srcptr)
{
	int			numbytes = array_nelems_size(srcptr, eltsize, nitems);

	memmove(destptr, srcptr, numbytes);
	return numbytes;
}

/*
 * Compute space needed for a slice of an array
 *
 * We assume the caller has verified that the slice coordinates are valid.
 */
static int
array_slice_size(int ndim, int *dim, int *lb, char *arraydataptr,
				 int eltsize, int *st, int *endp)
{
	int			st_pos,
				span[MAXDIM],
				prod[MAXDIM],
				dist[MAXDIM],
				indx[MAXDIM];
	char	   *ptr;
	int			i,
				j,
				inc;
	int			count = 0;

	mda_get_range(ndim, span, st, endp);

	/* Pretty easy for fixed element length ... */
	if (eltsize > 0)
		return ArrayGetNItems(ndim, span) * eltsize;

	/* Else gotta do it the hard way */
	st_pos = ArrayGetOffset(ndim, dim, lb, st);
	ptr = array_seek(arraydataptr, eltsize, st_pos);
	mda_get_prod(ndim, dim, prod);
	mda_get_offset_values(ndim, dist, prod, span);
	for (i = 0; i < ndim; i++)
		indx[i] = 0;
	j = ndim - 1;
	do
	{
		ptr = array_seek(ptr, eltsize, dist[j]);
		inc = INTALIGN(*(int32 *) ptr);
		ptr += inc;
		count += inc;
	} while ((j = mda_next_tuple(ndim, indx, span)) != -1);
	return count;
}

/*
 * Extract a slice of an array into consecutive elements at *destPtr.
 *
 * We assume the caller has verified that the slice coordinates are valid
 * and allocated enough storage at *destPtr.
 */
static void
array_extract_slice(int ndim,
					int *dim,
					int *lb,
					char *arraydataptr,
					int eltsize,
					int *st,
					int *endp,
					char *destPtr)
{
	int			st_pos,
				prod[MAXDIM],
				span[MAXDIM],
				dist[MAXDIM],
				indx[MAXDIM];
	char	   *srcPtr;
	int			i,
				j,
				inc;

	st_pos = ArrayGetOffset(ndim, dim, lb, st);
	srcPtr = array_seek(arraydataptr, eltsize, st_pos);
	mda_get_prod(ndim, dim, prod);
	mda_get_range(ndim, span, st, endp);
	mda_get_offset_values(ndim, dist, prod, span);
	for (i = 0; i < ndim; i++)
		indx[i] = 0;
	j = ndim - 1;
	do
	{
		srcPtr = array_seek(srcPtr, eltsize, dist[j]);
		inc = array_copy(destPtr, eltsize, 1, srcPtr);
		destPtr += inc;
		srcPtr += inc;
	} while ((j = mda_next_tuple(ndim, indx, span)) != -1);
}

/*
 * Insert a slice into an array.
 *
 * ndim/dim/lb are dimensions of the dest array, which has data area
 * starting at origPtr.  A new array with those same dimensions is to
 * be constructed; its data area starts at destPtr.
 *
 * Elements within the slice volume are taken from consecutive locations
 * at srcPtr; elements outside it are copied from origPtr.
 *
 * We assume the caller has verified that the slice coordinates are valid
 * and allocated enough storage at *destPtr.
 */
static void
array_insert_slice(int ndim,
				   int *dim,
				   int *lb,
				   char *origPtr,
				   int origdatasize,
				   char *destPtr,
				   int eltsize,
				   int *st,
				   int *endp,
				   char *srcPtr)
{
	int			st_pos,
				prod[MAXDIM],
				span[MAXDIM],
				dist[MAXDIM],
				indx[MAXDIM];
	char	   *origEndpoint = origPtr + origdatasize;
	int			i,
				j,
				inc;

	st_pos = ArrayGetOffset(ndim, dim, lb, st);
	inc = array_copy(destPtr, eltsize, st_pos, origPtr);
	destPtr += inc;
	origPtr += inc;
	mda_get_prod(ndim, dim, prod);
	mda_get_range(ndim, span, st, endp);
	mda_get_offset_values(ndim, dist, prod, span);
	for (i = 0; i < ndim; i++)
		indx[i] = 0;
	j = ndim - 1;
	do
	{
		/* Copy/advance over elements between here and next part of slice */
		inc = array_copy(destPtr, eltsize, dist[j], origPtr);
		destPtr += inc;
		origPtr += inc;
		/* Copy new element at this slice position */
		inc = array_copy(destPtr, eltsize, 1, srcPtr);
		destPtr += inc;
		srcPtr += inc;
		/* Advance over old element at this slice position */
		origPtr = array_seek(origPtr, eltsize, 1);
	} while ((j = mda_next_tuple(ndim, indx, span)) != -1);

	/* don't miss any data at the end */
	memcpy(destPtr, origPtr, origEndpoint - origPtr);
}
