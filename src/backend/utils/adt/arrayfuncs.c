/*-------------------------------------------------------------------------
 *
 * arrayfuncs.c
 *	  Support functions for arrays.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/arrayfuncs.c,v 1.62 2000/07/22 03:34:43 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <ctype.h>

#include "postgres.h"

#include "catalog/catalog.h"
#include "catalog/pg_type.h"
#include "utils/array.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

#define ASSGN	 "="

#define RETURN_NULL(type)  do { *isNull = true; return (type) 0; } while (0)


/*
 * An array has the following internal structure:
 *	  <nbytes>		- total number of bytes
 *	  <ndim>		- number of dimensions of the array
 *	  <flags>		- bit mask of flags
 *	  <dim>			- size of each array axis
 *	  <dim_lower>	- lower boundary of each dimension
 *	  <actual data> - whatever is the stored data
 * The actual data starts on a MAXALIGN boundary.
 */

static int	ArrayCount(char *str, int *dim, int typdelim);
static Datum *ReadArrayStr(char *arrayStr, int nitems, int ndim, int *dim,
			  FmgrInfo *inputproc, Oid typelem, int32 typmod,
			  char typdelim, int typlen, bool typbyval,
			  char typalign, int *nbytes);
static void CopyArrayEls(char *p, Datum *values, int nitems,
						 bool typbyval, int typlen, char typalign,
						 bool freedata);
static void system_cache_lookup(Oid element_type, bool input, int *typlen,
				 bool *typbyval, char *typdelim, Oid *typelem, Oid *proc,
					char *typalign);
static Datum ArrayCast(char *value, bool byval, int len);
static void ArrayClipCopy(int *st, int *endp, int bsize, char *destPtr,
						  ArrayType *array, bool from);
static int	ArrayClipCount(int *st, int *endp, ArrayType *array);
static int	ArrayCastAndSet(Datum src, bool typbyval, int typlen, char *dest);
static bool SanityCheckInput(int ndim, int n, int *dim, int *lb, int *indx);
static int	array_read(char *destptr, int eltsize, int nitems, char *srcptr);
static char *array_seek(char *ptr, int eltsize, int nitems);


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
	bool		typbyval,
				done;
	char		typdelim;
	Oid			typinput;
	Oid			typelem;
	char	   *string_save,
			   *p,
			   *q,
			   *r;
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

	system_cache_lookup(element_type, true, &typlen, &typbyval, &typdelim,
						&typelem, &typinput, &typalign);

	fmgr_info(typinput, &inputproc);

	string_save = (char *) palloc(strlen(string) + 3);
	strcpy(string_save, string);

	/* --- read array dimensions  ---------- */
	p = q = string_save;
	done = false;
	for (ndim = 0; !done;)
	{
		while (isspace((int) *p))
			p++;
		if (*p == '[')
		{
			p++;
			if ((r = (char *) strchr(p, ':')) == (char *) NULL)
				lBound[ndim] = 1;
			else
			{
				*r = '\0';
				lBound[ndim] = atoi(p);
				p = r + 1;
			}
			for (q = p; isdigit((int) *q); q++);
			if (*q != ']')
				elog(ERROR, "array_in: missing ']' in array declaration");
			*q = '\0';
			dim[ndim] = atoi(p);
			if ((dim[ndim] < 0) || (lBound[ndim] < 0))
				elog(ERROR, "array_in: array dimensions need to be positive");
			dim[ndim] = dim[ndim] - lBound[ndim] + 1;
			if (dim[ndim] < 0)
				elog(ERROR, "array_in: upper_bound cannot be < lower_bound");
			p = q + 1;
			ndim++;
		}
		else
			done = true;
	}

	if (ndim == 0)
	{
		if (*p == '{')
		{
			ndim = ArrayCount(p, dim, typdelim);
			for (i = 0; i < ndim; i++)
				lBound[i] = 1;
		}
		else
			elog(ERROR, "array_in: Need to specify dimension");
	}
	else
	{
		while (isspace((int) *p))
			p++;
		if (strncmp(p, ASSGN, strlen(ASSGN)))
			elog(ERROR, "array_in: missing assignment operator");
		p += strlen(ASSGN);
		while (isspace((int) *p))
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
			while (isspace((int) *q))
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
			while (isspace((int) *q))
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
		if (typbyval)
		{
			switch (typlen)
			{
				case 1:
					values[i] = DatumGetCString(FunctionCall3(&outputproc,
												CharGetDatum(*p),
												ObjectIdGetDatum(typelem),
												Int32GetDatum(-1)));
					break;
				case 2:
					values[i] = DatumGetCString(FunctionCall3(&outputproc,
												Int16GetDatum(*(int16 *) p),
												ObjectIdGetDatum(typelem),
												Int32GetDatum(-1)));
					break;
				case 3:
				case 4:
					values[i] = DatumGetCString(FunctionCall3(&outputproc,
												Int32GetDatum(*(int32 *) p),
												ObjectIdGetDatum(typelem),
												Int32GetDatum(-1)));
					break;
			}
			p += typlen;
		}
		else
		{
			values[i] = DatumGetCString(FunctionCall3(&outputproc,
												PointerGetDatum(p),
												ObjectIdGetDatum(typelem),
												Int32GetDatum(-1)));
			if (typlen > 0)
				p += typlen;
			else
				p += INTALIGN(*(int32 *) p);

			/*
			 * For the pair of double quotes
			 */
			overall_length += 2;
		}
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
	int			ndim,
			   *dim,
			   *lb,
				offset;
	char	   *retptr;

	if (array == (ArrayType *) NULL)
		RETURN_NULL(Datum);

	if (arraylen > 0)
	{
		/*
		 * fixed-length arrays -- these are assumed to be 1-d, 0-based
		 */
		if (nSubscripts != 1)
			RETURN_NULL(Datum);
		if (indx[0] < 0 || indx[0] * elmlen >= arraylen)
			RETURN_NULL(Datum);
		retptr = (char *) array + indx[0] * elmlen;
		return ArrayCast(retptr, elmbyval, elmlen);
	}

	/* detoast input if necessary */
	array = DatumGetArrayTypeP(PointerGetDatum(array));

	ndim = ARR_NDIM(array);
	dim = ARR_DIMS(array);
	lb = ARR_LBOUND(array);

	if (!SanityCheckInput(ndim, nSubscripts, dim, lb, indx))
		RETURN_NULL(Datum);

	offset = ArrayGetOffset(nSubscripts, dim, lb, indx);

	retptr = array_seek(ARR_DATA_PTR(array), elmlen, offset);

	return ArrayCast(retptr, elmbyval, elmlen);
}

/*-----------------------------------------------------------------------------
 * array_get_slice :
 *		   This routine takes an array and a range of indices (upperIndex and
 *		   lowerIndx), creates a new array structure for the referred elements
 *		   and returns a pointer to it.
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
	ArrayType  *newArr;
	int			bytes,
				span[MAXDIM];

	if (array == (ArrayType *) NULL)
		RETURN_NULL(ArrayType *);

	if (arraylen > 0)
	{
		/*
		 * fixed-length arrays -- no can do slice...
		 */
		elog(ERROR, "Slices of fixed-length arrays not implemented");
	}

	/* detoast input if necessary */
	array = DatumGetArrayTypeP(PointerGetDatum(array));

	ndim = ARR_NDIM(array);
	dim = ARR_DIMS(array);
	lb = ARR_LBOUND(array);

	if (!SanityCheckInput(ndim, nSubscripts, dim, lb, upperIndx) ||
		!SanityCheckInput(ndim, nSubscripts, dim, lb, lowerIndx))
		RETURN_NULL(ArrayType *);

	for (i = 0; i < nSubscripts; i++)
		if (lowerIndx[i] > upperIndx[i])
			RETURN_NULL(ArrayType *);

	mda_get_range(nSubscripts, span, lowerIndx, upperIndx);

	if (elmlen > 0)
		bytes = ArrayGetNItems(nSubscripts, span) * elmlen;
	else
		bytes = ArrayClipCount(lowerIndx, upperIndx, array);
	bytes += ARR_OVERHEAD(nSubscripts);

	newArr = (ArrayType *) palloc(bytes);
	newArr->size = bytes;
	newArr->ndim = array->ndim;
	newArr->flags = array->flags;
	memcpy(ARR_DIMS(newArr), span, nSubscripts * sizeof(int));
	memcpy(ARR_LBOUND(newArr), lowerIndx, nSubscripts * sizeof(int));
	ArrayClipCopy(lowerIndx, upperIndx, elmlen, ARR_DATA_PTR(newArr),
				  array, true);

	return newArr;
}

/*-----------------------------------------------------------------------------
 * array_set :
 *		  This routine sets the value of an array location (specified by
 *		  an index array) to a new value specified by "dataValue".
 * result :
 *		  A new array is returned, just like the old except for the one
 *		  modified entry.
 *
 * NOTE: For assignments, we throw an error for silly subscripts etc,
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
	int			ndim,
			   *dim,
			   *lb,
				offset;
	ArrayType  *newarray;
	char	   *elt_ptr;
	int			oldsize,
				newsize,
				oldlen,
				newlen,
				lth0,
				lth1,
				lth2;

	if (array == (ArrayType *) NULL)
		RETURN_NULL(ArrayType *);

	if (arraylen > 0)
	{
		/*
		 * fixed-length arrays -- these are assumed to be 1-d, 0-based
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

	/* detoast input if necessary */
	array = DatumGetArrayTypeP(PointerGetDatum(array));

	ndim = ARR_NDIM(array);
	dim = ARR_DIMS(array);
	lb = ARR_LBOUND(array);

	if (!SanityCheckInput(ndim, nSubscripts, dim, lb, indx))
		elog(ERROR, "Invalid array subscripts");

	offset = ArrayGetOffset(nSubscripts, dim, lb, indx);

	elt_ptr = array_seek(ARR_DATA_PTR(array), elmlen, offset);

	if (elmlen > 0)
	{
		oldlen = newlen = elmlen;
	}
	else
	{
		/* varlena type */
		oldlen = INTALIGN(*(int32 *) elt_ptr);
		newlen = INTALIGN(*(int32 *) DatumGetPointer(dataValue));
	}

	oldsize = ARR_SIZE(array);
	lth0 = ARR_OVERHEAD(ndim);
	lth1 = (int) (elt_ptr - ARR_DATA_PTR(array));
	lth2 = (int) (oldsize - lth0 - lth1 - oldlen);
	newsize = lth0 + lth1 + newlen + lth2;

	newarray = (ArrayType *) palloc(newsize);
	memcpy((char *) newarray, (char *) array, lth0 + lth1);
	memcpy((char *) newarray + lth0 + lth1 + newlen,
		   (char *) array + lth0 + lth1 + oldlen, lth2);
	newarray->size = newsize;
	newlen = ArrayCastAndSet(dataValue, elmbyval, elmlen,
							 (char *) newarray + lth0 + lth1);

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
			   *dim,
			   *lb;
	int			span[MAXDIM];

	if (array == (ArrayType *) NULL)
		RETURN_NULL(ArrayType *);
	if (srcArray == (ArrayType *) NULL)
		RETURN_NULL(ArrayType *);

	if (arraylen > 0)
	{
		/*
		 * fixed-length arrays -- no can do slice...
		 */
		elog(ERROR, "Updates on slices of fixed-length arrays not implemented");
	}

	/* detoast array, making sure we get an overwritable copy */
	array = DatumGetArrayTypePCopy(PointerGetDatum(array));

	/* detoast source array if necessary */
	srcArray = DatumGetArrayTypeP(PointerGetDatum(srcArray));

	if (elmlen < 0)
		elog(ERROR, "Updates on slices of arrays of variable length elements not implemented");

	ndim = ARR_NDIM(array);
	dim = ARR_DIMS(array);
	lb = ARR_LBOUND(array);

	if (!SanityCheckInput(ndim, nSubscripts, dim, lb, upperIndx) ||
		!SanityCheckInput(ndim, nSubscripts, dim, lb, lowerIndx))
		elog(ERROR, "Invalid array subscripts");

	for (i = 0; i < nSubscripts; i++)
		if (lowerIndx[i] > upperIndx[i])
		elog(ERROR, "Invalid array subscripts");

	/* make sure source array has enough entries */
	mda_get_range(ndim, span, lowerIndx, upperIndx);

	if (ArrayGetNItems(ndim, span) >
		ArrayGetNItems(ARR_NDIM(srcArray), ARR_DIMS(srcArray)))
		elog(ERROR, "Source array too small");

	ArrayClipCopy(lowerIndx, upperIndx, elmlen, ARR_DATA_PTR(srcArray),
				  array, false);

	return array;
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
		if (inp_typbyval)
		{
			switch (inp_typlen)
			{
				case 1:
					elt = CharGetDatum(*s);
					break;
				case 2:
					elt = Int16GetDatum(*(int16 *) s);
					break;
				case 4:
					elt = Int32GetDatum(*(int32 *) s);
					break;
				default:
					elog(ERROR, "array_map: unsupported byval length %d",
						 inp_typlen);
					elt = 0;	/* keep compiler quiet */
					break;
			}
			s += inp_typlen;
		}
		else
		{
			elt = PointerGetDatum(s);
			if (inp_typlen > 0)
				s += inp_typlen;
			else
				s += INTALIGN(*(int32 *) s);
		}

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

		/* Update total result size */
		if (typbyval)
			nbytes += typlen;
		else
			nbytes += ((typlen > 0) ? typlen :
					   INTALIGN(VARSIZE(DatumGetPointer(values[i]))));
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
		/* varlena type */
		nbytes = 0;
		for (i = 0; i < nelems; i++)
			nbytes += INTALIGN(VARSIZE(DatumGetPointer(elems[i])));
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
		if (elmbyval)
		{
			switch (elmlen)
			{
				case 1:
					elems[i] = CharGetDatum(*p);
					break;
				case 2:
					elems[i] = Int16GetDatum(*(int16 *) p);
					break;
				case 4:
					elems[i] = Int32GetDatum(*(int32 *) p);
					break;
			}
			p += elmlen;
		}
		else
		{
			elems[i] = PointerGetDatum(p);
			if (elmlen > 0)
				p += elmlen;
			else
				p += INTALIGN(VARSIZE(p));
		}
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

	typeTuple = SearchSysCacheTuple(TYPEOID,
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
}

/* Fetch array value at pointer, converted correctly to a Datum */
static Datum
ArrayCast(char *value, bool byval, int len)
{
	if (! byval)
		return PointerGetDatum(value);

	switch (len)
	{
		case 1:
			return CharGetDatum(*value);
		case 2:
			return Int16GetDatum(*(int16 *) value);
		case 4:
			return Int32GetDatum(*(int32 *) value);
		default:
			elog(ERROR, "ArrayCast: unsupported byval length %d", len);
			break;
	}
	return 0;					/* keep compiler quiet */
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
			switch (typlen)
			{
				case 1:
					*dest = DatumGetChar(src);
					break;
				case 2:
					*(int16 *) dest = DatumGetInt16(src);
					break;
				case 4:
					*(int32 *) dest = DatumGetInt32(src);
					break;
				default:
					elog(ERROR, "ArrayCastAndSet: unsupported byval length %d",
						 typlen);
					break;
			}
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

/* Do Sanity check on input subscripting info */
static bool
SanityCheckInput(int ndim, int n, int *dim, int *lb, int *indx)
{
	int			i;

	if (n != ndim || ndim <= 0 || ndim > MAXDIM)
		return false;
	for (i = 0; i < ndim; i++)
		if ((lb[i] > indx[i]) || (indx[i] >= (dim[i] + lb[i])))
			return false;
	return true;
}

/* Copy an array slice into or out of an array */
static void
ArrayClipCopy(int *st,
			  int *endp,
			  int bsize,
			  char *destPtr,
			  ArrayType *array,
			  bool from)
{
	int			n,
			   *dim,
			   *lb,
				st_pos,
				prod[MAXDIM];
	int			span[MAXDIM],
				dist[MAXDIM],
				indx[MAXDIM];
	int			i,
				j,
				inc;
	char	   *srcPtr;

	n = ARR_NDIM(array);
	dim = ARR_DIMS(array);
	lb = ARR_LBOUND(array);
	st_pos = ArrayGetOffset(n, dim, lb, st);
	srcPtr = array_seek(ARR_DATA_PTR(array), bsize, st_pos);
	mda_get_prod(n, dim, prod);
	mda_get_range(n, span, st, endp);
	mda_get_offset_values(n, dist, prod, span);
	for (i = 0; i < n; i++)
		indx[i] = 0;
	j = n - 1;
	do
	{
		srcPtr = array_seek(srcPtr, bsize, dist[j]);
		if (from)
			inc = array_read(destPtr, bsize, 1, srcPtr);
		else
			inc = array_read(srcPtr, bsize, 1, destPtr);
		destPtr += inc;
		srcPtr += inc;
	} while ((j = mda_next_tuple(n, indx, span)) != -1);
}

/* Compute space needed for an array slice of varlena items */
static int
ArrayClipCount(int *st, int *endp, ArrayType *array)
{
	int			n,
			   *dim,
			   *lb,
				st_pos,
				prod[MAXDIM];
	int			span[MAXDIM],
				dist[MAXDIM],
				indx[MAXDIM];
	int			i,
				j,
				inc;
	int			count = 0;
	char	   *ptr;

	n = ARR_NDIM(array);
	dim = ARR_DIMS(array);
	lb = ARR_LBOUND(array);
	st_pos = ArrayGetOffset(n, dim, lb, st);
	ptr = array_seek(ARR_DATA_PTR(array), -1, st_pos);
	mda_get_prod(n, dim, prod);
	mda_get_range(n, span, st, endp);
	mda_get_offset_values(n, dist, prod, span);
	for (i = 0; i < n; i++)
		indx[i] = 0;
	j = n - 1;
	do
	{
		ptr = array_seek(ptr, -1, dist[j]);
		inc = INTALIGN(*(int32 *) ptr);
		ptr += inc;
		count += inc;
	} while ((j = mda_next_tuple(n, indx, span)) != -1);
	return count;
}

/* Advance over nitems array elements */
static char *
array_seek(char *ptr, int eltsize, int nitems)
{
	int			i;

	if (eltsize > 0)
		return ptr + eltsize * nitems;
	for (i = 0; i < nitems; i++)
		ptr += INTALIGN(*(int32 *) ptr);
	return ptr;
}

/* Copy nitems array elements from srcptr to destptr */
static int
array_read(char *destptr, int eltsize, int nitems, char *srcptr)
{
	int			i,
				inc,
				tmp;

	if (eltsize > 0)
	{
		memmove(destptr, srcptr, eltsize * nitems);
		return eltsize * nitems;
	}
	inc = 0;
	for (i = 0; i < nitems; i++)
	{
		tmp = (INTALIGN(*(int32 *) srcptr));
		memmove(destptr, srcptr, tmp);
		srcptr += tmp;
		destptr += tmp;
		inc += tmp;
	}
	return inc;
}
