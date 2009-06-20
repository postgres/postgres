/*-------------------------------------------------------------------------
 *
 * arrayfuncs.c
 *	  Support functions for arrays.
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/arrayfuncs.c,v 1.158 2009/06/20 18:45:28 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "funcapi.h"
#include "libpq/pqformat.h"
#include "parser/parse_coerce.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/typcache.h"


/*
 * GUC parameter
 */
bool		Array_nulls = true;

/*
 * Local definitions
 */
#define ASSGN	 "="

typedef enum
{
	ARRAY_NO_LEVEL,
	ARRAY_LEVEL_STARTED,
	ARRAY_ELEM_STARTED,
	ARRAY_ELEM_COMPLETED,
	ARRAY_QUOTED_ELEM_STARTED,
	ARRAY_QUOTED_ELEM_COMPLETED,
	ARRAY_ELEM_DELIMITED,
	ARRAY_LEVEL_COMPLETED,
	ARRAY_LEVEL_DELIMITED
} ArrayParseState;

static int	ArrayCount(const char *str, int *dim, char typdelim);
static void ReadArrayStr(char *arrayStr, const char *origStr,
			 int nitems, int ndim, int *dim,
			 FmgrInfo *inputproc, Oid typioparam, int32 typmod,
			 char typdelim,
			 int typlen, bool typbyval, char typalign,
			 Datum *values, bool *nulls,
			 bool *hasnulls, int32 *nbytes);
static void ReadArrayBinary(StringInfo buf, int nitems,
				FmgrInfo *receiveproc, Oid typioparam, int32 typmod,
				int typlen, bool typbyval, char typalign,
				Datum *values, bool *nulls,
				bool *hasnulls, int32 *nbytes);
static void CopyArrayEls(ArrayType *array,
			 Datum *values, bool *nulls, int nitems,
			 int typlen, bool typbyval, char typalign,
			 bool freedata);
static bool array_get_isnull(const bits8 *nullbitmap, int offset);
static void array_set_isnull(bits8 *nullbitmap, int offset, bool isNull);
static Datum ArrayCast(char *value, bool byval, int len);
static int ArrayCastAndSet(Datum src,
				int typlen, bool typbyval, char typalign,
				char *dest);
static char *array_seek(char *ptr, int offset, bits8 *nullbitmap, int nitems,
		   int typlen, bool typbyval, char typalign);
static int array_nelems_size(char *ptr, int offset, bits8 *nullbitmap,
				  int nitems, int typlen, bool typbyval, char typalign);
static int array_copy(char *destptr, int nitems,
		   char *srcptr, int offset, bits8 *nullbitmap,
		   int typlen, bool typbyval, char typalign);
static int array_slice_size(char *arraydataptr, bits8 *arraynullsptr,
				 int ndim, int *dim, int *lb,
				 int *st, int *endp,
				 int typlen, bool typbyval, char typalign);
static void array_extract_slice(ArrayType *newarray,
					int ndim, int *dim, int *lb,
					char *arraydataptr, bits8 *arraynullsptr,
					int *st, int *endp,
					int typlen, bool typbyval, char typalign);
static void array_insert_slice(ArrayType *destArray, ArrayType *origArray,
				   ArrayType *srcArray,
				   int ndim, int *dim, int *lb,
				   int *st, int *endp,
				   int typlen, bool typbyval, char typalign);
static int	array_cmp(FunctionCallInfo fcinfo);
static ArrayType *create_array_envelope(int ndims, int *dimv, int *lbv, int nbytes,
					  Oid elmtype, int dataoffset);
static ArrayType *array_fill_internal(ArrayType *dims, ArrayType *lbs,
					Datum value, bool isnull, Oid elmtype,
					FunctionCallInfo fcinfo);


/*
 * array_in :
 *		  converts an array from the external format in "string" to
 *		  its internal format.
 *
 * return value :
 *		  the internal representation of the input array
 */
Datum
array_in(PG_FUNCTION_ARGS)
{
	char	   *string = PG_GETARG_CSTRING(0);	/* external form */
	Oid			element_type = PG_GETARG_OID(1);		/* type of an array
														 * element */
	int32		typmod = PG_GETARG_INT32(2);	/* typmod for array elements */
	int			typlen;
	bool		typbyval;
	char		typalign;
	char		typdelim;
	Oid			typioparam;
	char	   *string_save,
			   *p;
	int			i,
				nitems;
	Datum	   *dataPtr;
	bool	   *nullsPtr;
	bool		hasnulls;
	int32		nbytes;
	int32		dataoffset;
	ArrayType  *retval;
	int			ndim,
				dim[MAXDIM],
				lBound[MAXDIM];
	ArrayMetaState *my_extra;

	/*
	 * We arrange to look up info about element type, including its input
	 * conversion proc, only once per series of calls, assuming the element
	 * type doesn't change underneath us.
	 */
	my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL)
	{
		fcinfo->flinfo->fn_extra = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
													  sizeof(ArrayMetaState));
		my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
		my_extra->element_type = ~element_type;
	}

	if (my_extra->element_type != element_type)
	{
		/*
		 * Get info about element type, including its input conversion proc
		 */
		get_type_io_data(element_type, IOFunc_input,
						 &my_extra->typlen, &my_extra->typbyval,
						 &my_extra->typalign, &my_extra->typdelim,
						 &my_extra->typioparam, &my_extra->typiofunc);
		fmgr_info_cxt(my_extra->typiofunc, &my_extra->proc,
					  fcinfo->flinfo->fn_mcxt);
		my_extra->element_type = element_type;
	}
	typlen = my_extra->typlen;
	typbyval = my_extra->typbyval;
	typalign = my_extra->typalign;
	typdelim = my_extra->typdelim;
	typioparam = my_extra->typioparam;

	/* Make a modifiable copy of the input */
	string_save = pstrdup(string);

	/*
	 * If the input string starts with dimension info, read and use that.
	 * Otherwise, we require the input to be in curly-brace style, and we
	 * prescan the input to determine dimensions.
	 *
	 * Dimension info takes the form of one or more [n] or [m:n] items. The
	 * outer loop iterates once per dimension item.
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
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
							ndim, MAXDIM)));

		for (q = p; isdigit((unsigned char) *q) || (*q == '-') || (*q == '+'); q++);
		if (q == p)				/* no digits? */
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("missing dimension value")));

		if (*q == ':')
		{
			/* [m:n] format */
			*q = '\0';
			lBound[ndim] = atoi(p);
			p = q + 1;
			for (q = p; isdigit((unsigned char) *q) || (*q == '-') || (*q == '+'); q++);
			if (q == p)			/* no digits? */
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("missing dimension value")));
		}
		else
		{
			/* [n] format */
			lBound[ndim] = 1;
		}
		if (*q != ']')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("missing \"]\" in array dimensions")));

		*q = '\0';
		ub = atoi(p);
		p = q + 1;
		if (ub < lBound[ndim])
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("upper bound cannot be less than lower bound")));

		dim[ndim] = ub - lBound[ndim] + 1;
		ndim++;
	}

	if (ndim == 0)
	{
		/* No array dimensions, so intuit dimensions from brace structure */
		if (*p != '{')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("array value must start with \"{\" or dimension information")));
		ndim = ArrayCount(p, dim, typdelim);
		for (i = 0; i < ndim; i++)
			lBound[i] = 1;
	}
	else
	{
		int			ndim_braces,
					dim_braces[MAXDIM];

		/* If array dimensions are given, expect '=' operator */
		if (strncmp(p, ASSGN, strlen(ASSGN)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("missing assignment operator")));
		p += strlen(ASSGN);
		while (isspace((unsigned char) *p))
			p++;

		/*
		 * intuit dimensions from brace structure -- it better match what we
		 * were given
		 */
		if (*p != '{')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("array value must start with \"{\" or dimension information")));
		ndim_braces = ArrayCount(p, dim_braces, typdelim);
		if (ndim_braces != ndim)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				errmsg("array dimensions incompatible with array literal")));
		for (i = 0; i < ndim; ++i)
		{
			if (dim[i] != dim_braces[i])
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				errmsg("array dimensions incompatible with array literal")));
		}
	}

#ifdef ARRAYDEBUG
	printf("array_in- ndim %d (", ndim);
	for (i = 0; i < ndim; i++)
	{
		printf(" %d", dim[i]);
	};
	printf(") for %s\n", string);
#endif

	/* This checks for overflow of the array dimensions */
	nitems = ArrayGetNItems(ndim, dim);
	/* Empty array? */
	if (nitems == 0)
		PG_RETURN_ARRAYTYPE_P(construct_empty_array(element_type));

	dataPtr = (Datum *) palloc(nitems * sizeof(Datum));
	nullsPtr = (bool *) palloc(nitems * sizeof(bool));
	ReadArrayStr(p, string,
				 nitems, ndim, dim,
				 &my_extra->proc, typioparam, typmod,
				 typdelim,
				 typlen, typbyval, typalign,
				 dataPtr, nullsPtr,
				 &hasnulls, &nbytes);
	if (hasnulls)
	{
		dataoffset = ARR_OVERHEAD_WITHNULLS(ndim, nitems);
		nbytes += dataoffset;
	}
	else
	{
		dataoffset = 0;			/* marker for no null bitmap */
		nbytes += ARR_OVERHEAD_NONULLS(ndim);
	}
	retval = (ArrayType *) palloc0(nbytes);
	SET_VARSIZE(retval, nbytes);
	retval->ndim = ndim;
	retval->dataoffset = dataoffset;
	retval->elemtype = element_type;
	memcpy(ARR_DIMS(retval), dim, ndim * sizeof(int));
	memcpy(ARR_LBOUND(retval), lBound, ndim * sizeof(int));

	CopyArrayEls(retval,
				 dataPtr, nullsPtr, nitems,
				 typlen, typbyval, typalign,
				 true);

	pfree(dataPtr);
	pfree(nullsPtr);
	pfree(string_save);

	PG_RETURN_ARRAYTYPE_P(retval);
}

/*
 * ArrayCount
 *	 Determines the dimensions for an array string.
 *
 * Returns number of dimensions as function result.  The axis lengths are
 * returned in dim[], which must be of size MAXDIM.
 */
static int
ArrayCount(const char *str, int *dim, char typdelim)
{
	int			nest_level = 0,
				i;
	int			ndim = 1,
				temp[MAXDIM],
				nelems[MAXDIM],
				nelems_last[MAXDIM];
	bool		in_quotes = false;
	bool		eoArray = false;
	bool		empty_array = true;
	const char *ptr;
	ArrayParseState parse_state = ARRAY_NO_LEVEL;

	for (i = 0; i < MAXDIM; ++i)
	{
		temp[i] = dim[i] = 0;
		nelems_last[i] = nelems[i] = 1;
	}

	ptr = str;
	while (!eoArray)
	{
		bool		itemdone = false;

		while (!itemdone)
		{
			if (parse_state == ARRAY_ELEM_STARTED ||
				parse_state == ARRAY_QUOTED_ELEM_STARTED)
				empty_array = false;

			switch (*ptr)
			{
				case '\0':
					/* Signal a premature end of the string */
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							 errmsg("malformed array literal: \"%s\"", str)));
					break;
				case '\\':

					/*
					 * An escape must be after a level start, after an element
					 * start, or after an element delimiter. In any case we
					 * now must be past an element start.
					 */
					if (parse_state != ARRAY_LEVEL_STARTED &&
						parse_state != ARRAY_ELEM_STARTED &&
						parse_state != ARRAY_QUOTED_ELEM_STARTED &&
						parse_state != ARRAY_ELEM_DELIMITED)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							errmsg("malformed array literal: \"%s\"", str)));
					if (parse_state != ARRAY_QUOTED_ELEM_STARTED)
						parse_state = ARRAY_ELEM_STARTED;
					/* skip the escaped character */
					if (*(ptr + 1))
						ptr++;
					else
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							errmsg("malformed array literal: \"%s\"", str)));
					break;
				case '\"':

					/*
					 * A quote must be after a level start, after a quoted
					 * element start, or after an element delimiter. In any
					 * case we now must be past an element start.
					 */
					if (parse_state != ARRAY_LEVEL_STARTED &&
						parse_state != ARRAY_QUOTED_ELEM_STARTED &&
						parse_state != ARRAY_ELEM_DELIMITED)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							errmsg("malformed array literal: \"%s\"", str)));
					in_quotes = !in_quotes;
					if (in_quotes)
						parse_state = ARRAY_QUOTED_ELEM_STARTED;
					else
						parse_state = ARRAY_QUOTED_ELEM_COMPLETED;
					break;
				case '{':
					if (!in_quotes)
					{
						/*
						 * A left brace can occur if no nesting has occurred
						 * yet, after a level start, or after a level
						 * delimiter.
						 */
						if (parse_state != ARRAY_NO_LEVEL &&
							parse_state != ARRAY_LEVEL_STARTED &&
							parse_state != ARRAY_LEVEL_DELIMITED)
							ereport(ERROR,
							   (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							errmsg("malformed array literal: \"%s\"", str)));
						parse_state = ARRAY_LEVEL_STARTED;
						if (nest_level >= MAXDIM)
							ereport(ERROR,
									(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
									 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
											nest_level, MAXDIM)));
						temp[nest_level] = 0;
						nest_level++;
						if (ndim < nest_level)
							ndim = nest_level;
					}
					break;
				case '}':
					if (!in_quotes)
					{
						/*
						 * A right brace can occur after an element start, an
						 * element completion, a quoted element completion, or
						 * a level completion.
						 */
						if (parse_state != ARRAY_ELEM_STARTED &&
							parse_state != ARRAY_ELEM_COMPLETED &&
							parse_state != ARRAY_QUOTED_ELEM_COMPLETED &&
							parse_state != ARRAY_LEVEL_COMPLETED &&
							!(nest_level == 1 && parse_state == ARRAY_LEVEL_STARTED))
							ereport(ERROR,
							   (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							errmsg("malformed array literal: \"%s\"", str)));
						parse_state = ARRAY_LEVEL_COMPLETED;
						if (nest_level == 0)
							ereport(ERROR,
							   (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							errmsg("malformed array literal: \"%s\"", str)));
						nest_level--;

						if ((nelems_last[nest_level] != 1) &&
							(nelems[nest_level] != nelems_last[nest_level]))
							ereport(ERROR,
							   (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
								errmsg("multidimensional arrays must have "
									   "array expressions with matching "
									   "dimensions")));
						nelems_last[nest_level] = nelems[nest_level];
						nelems[nest_level] = 1;
						if (nest_level == 0)
							eoArray = itemdone = true;
						else
						{
							/*
							 * We don't set itemdone here; see comments in
							 * ReadArrayStr
							 */
							temp[nest_level - 1]++;
						}
					}
					break;
				default:
					if (!in_quotes)
					{
						if (*ptr == typdelim)
						{
							/*
							 * Delimiters can occur after an element start, an
							 * element completion, a quoted element
							 * completion, or a level completion.
							 */
							if (parse_state != ARRAY_ELEM_STARTED &&
								parse_state != ARRAY_ELEM_COMPLETED &&
								parse_state != ARRAY_QUOTED_ELEM_COMPLETED &&
								parse_state != ARRAY_LEVEL_COMPLETED)
								ereport(ERROR,
								(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
								 errmsg("malformed array literal: \"%s\"", str)));
							if (parse_state == ARRAY_LEVEL_COMPLETED)
								parse_state = ARRAY_LEVEL_DELIMITED;
							else
								parse_state = ARRAY_ELEM_DELIMITED;
							itemdone = true;
							nelems[nest_level - 1]++;
						}
						else if (!isspace((unsigned char) *ptr))
						{
							/*
							 * Other non-space characters must be after a
							 * level start, after an element start, or after
							 * an element delimiter. In any case we now must
							 * be past an element start.
							 */
							if (parse_state != ARRAY_LEVEL_STARTED &&
								parse_state != ARRAY_ELEM_STARTED &&
								parse_state != ARRAY_ELEM_DELIMITED)
								ereport(ERROR,
								(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
								 errmsg("malformed array literal: \"%s\"", str)));
							parse_state = ARRAY_ELEM_STARTED;
						}
					}
					break;
			}
			if (!itemdone)
				ptr++;
		}
		temp[ndim - 1]++;
		ptr++;
	}

	/* only whitespace is allowed after the closing brace */
	while (*ptr)
	{
		if (!isspace((unsigned char) *ptr++))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed array literal: \"%s\"", str)));
	}

	/* special case for an empty array */
	if (empty_array)
		return 0;

	for (i = 0; i < ndim; ++i)
		dim[i] = temp[i];

	return ndim;
}

/*
 * ReadArrayStr :
 *	 parses the array string pointed to by "arrayStr" and converts the values
 *	 to internal format.  Unspecified elements are initialized to nulls.
 *	 The array dimensions must already have been determined.
 *
 * Inputs:
 *	arrayStr: the string to parse.
 *			  CAUTION: the contents of "arrayStr" will be modified!
 *	origStr: the unmodified input string, used only in error messages.
 *	nitems: total number of array elements, as already determined.
 *	ndim: number of array dimensions
 *	dim[]: array axis lengths
 *	inputproc: type-specific input procedure for element datatype.
 *	typioparam, typmod: auxiliary values to pass to inputproc.
 *	typdelim: the value delimiter (type-specific).
 *	typlen, typbyval, typalign: storage parameters of element datatype.
 *
 * Outputs:
 *	values[]: filled with converted data values.
 *	nulls[]: filled with is-null markers.
 *	*hasnulls: set TRUE iff there are any null elements.
 *	*nbytes: set to total size of data area needed (including alignment
 *		padding but not including array header overhead).
 *
 * Note that values[] and nulls[] are allocated by the caller, and must have
 * nitems elements.
 */
static void
ReadArrayStr(char *arrayStr,
			 const char *origStr,
			 int nitems,
			 int ndim,
			 int *dim,
			 FmgrInfo *inputproc,
			 Oid typioparam,
			 int32 typmod,
			 char typdelim,
			 int typlen,
			 bool typbyval,
			 char typalign,
			 Datum *values,
			 bool *nulls,
			 bool *hasnulls,
			 int32 *nbytes)
{
	int			i,
				nest_level = 0;
	char	   *srcptr;
	bool		in_quotes = false;
	bool		eoArray = false;
	bool		hasnull;
	int32		totbytes;
	int			indx[MAXDIM],
				prod[MAXDIM];

	mda_get_prod(ndim, dim, prod);
	MemSet(indx, 0, sizeof(indx));

	/* Initialize is-null markers to true */
	memset(nulls, true, nitems * sizeof(bool));

	/*
	 * We have to remove " and \ characters to create a clean item value to
	 * pass to the datatype input routine.	We overwrite each item value
	 * in-place within arrayStr to do this.  srcptr is the current scan point,
	 * and dstptr is where we are copying to.
	 *
	 * We also want to suppress leading and trailing unquoted whitespace. We
	 * use the leadingspace flag to suppress leading space.  Trailing space is
	 * tracked by using dstendptr to point to the last significant output
	 * character.
	 *
	 * The error checking in this routine is mostly pro-forma, since we expect
	 * that ArrayCount() already validated the string.
	 */
	srcptr = arrayStr;
	while (!eoArray)
	{
		bool		itemdone = false;
		bool		leadingspace = true;
		bool		hasquoting = false;
		char	   *itemstart;
		char	   *dstptr;
		char	   *dstendptr;

		i = -1;
		itemstart = dstptr = dstendptr = srcptr;

		while (!itemdone)
		{
			switch (*srcptr)
			{
				case '\0':
					/* Signal a premature end of the string */
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							 errmsg("malformed array literal: \"%s\"",
									origStr)));
					break;
				case '\\':
					/* Skip backslash, copy next character as-is. */
					srcptr++;
					if (*srcptr == '\0')
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
								 errmsg("malformed array literal: \"%s\"",
										origStr)));
					*dstptr++ = *srcptr++;
					/* Treat the escaped character as non-whitespace */
					leadingspace = false;
					dstendptr = dstptr;
					hasquoting = true;	/* can't be a NULL marker */
					break;
				case '\"':
					in_quotes = !in_quotes;
					if (in_quotes)
						leadingspace = false;
					else
					{
						/*
						 * Advance dstendptr when we exit in_quotes; this
						 * saves having to do it in all the other in_quotes
						 * cases.
						 */
						dstendptr = dstptr;
					}
					hasquoting = true;	/* can't be a NULL marker */
					srcptr++;
					break;
				case '{':
					if (!in_quotes)
					{
						if (nest_level >= ndim)
							ereport(ERROR,
							   (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
								errmsg("malformed array literal: \"%s\"",
									   origStr)));
						nest_level++;
						indx[nest_level - 1] = 0;
						srcptr++;
					}
					else
						*dstptr++ = *srcptr++;
					break;
				case '}':
					if (!in_quotes)
					{
						if (nest_level == 0)
							ereport(ERROR,
							   (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
								errmsg("malformed array literal: \"%s\"",
									   origStr)));
						if (i == -1)
							i = ArrayGetOffset0(ndim, indx, prod);
						indx[nest_level - 1] = 0;
						nest_level--;
						if (nest_level == 0)
							eoArray = itemdone = true;
						else
							indx[nest_level - 1]++;
						srcptr++;
					}
					else
						*dstptr++ = *srcptr++;
					break;
				default:
					if (in_quotes)
						*dstptr++ = *srcptr++;
					else if (*srcptr == typdelim)
					{
						if (i == -1)
							i = ArrayGetOffset0(ndim, indx, prod);
						itemdone = true;
						indx[ndim - 1]++;
						srcptr++;
					}
					else if (isspace((unsigned char) *srcptr))
					{
						/*
						 * If leading space, drop it immediately.  Else, copy
						 * but don't advance dstendptr.
						 */
						if (leadingspace)
							srcptr++;
						else
							*dstptr++ = *srcptr++;
					}
					else
					{
						*dstptr++ = *srcptr++;
						leadingspace = false;
						dstendptr = dstptr;
					}
					break;
			}
		}

		Assert(dstptr < srcptr);
		*dstendptr = '\0';

		if (i < 0 || i >= nitems)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed array literal: \"%s\"",
							origStr)));

		if (Array_nulls && !hasquoting &&
			pg_strcasecmp(itemstart, "NULL") == 0)
		{
			/* it's a NULL item */
			values[i] = InputFunctionCall(inputproc, NULL,
										  typioparam, typmod);
			nulls[i] = true;
		}
		else
		{
			values[i] = InputFunctionCall(inputproc, itemstart,
										  typioparam, typmod);
			nulls[i] = false;
		}
	}

	/*
	 * Check for nulls, compute total data space needed
	 */
	hasnull = false;
	totbytes = 0;
	for (i = 0; i < nitems; i++)
	{
		if (nulls[i])
			hasnull = true;
		else
		{
			/* let's just make sure data is not toasted */
			if (typlen == -1)
				values[i] = PointerGetDatum(PG_DETOAST_DATUM(values[i]));
			totbytes = att_addlength_datum(totbytes, typlen, values[i]);
			totbytes = att_align_nominal(totbytes, typalign);
			/* check for overflow of total request */
			if (!AllocSizeIsValid(totbytes))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("array size exceeds the maximum allowed (%d)",
								(int) MaxAllocSize)));
		}
	}
	*hasnulls = hasnull;
	*nbytes = totbytes;
}


/*
 * Copy data into an array object from a temporary array of Datums.
 *
 * array: array object (with header fields already filled in)
 * values: array of Datums to be copied
 * nulls: array of is-null flags (can be NULL if no nulls)
 * nitems: number of Datums to be copied
 * typbyval, typlen, typalign: info about element datatype
 * freedata: if TRUE and element type is pass-by-ref, pfree data values
 * referenced by Datums after copying them.
 *
 * If the input data is of varlena type, the caller must have ensured that
 * the values are not toasted.	(Doing it here doesn't work since the
 * caller has already allocated space for the array...)
 */
static void
CopyArrayEls(ArrayType *array,
			 Datum *values,
			 bool *nulls,
			 int nitems,
			 int typlen,
			 bool typbyval,
			 char typalign,
			 bool freedata)
{
	char	   *p = ARR_DATA_PTR(array);
	bits8	   *bitmap = ARR_NULLBITMAP(array);
	int			bitval = 0;
	int			bitmask = 1;
	int			i;

	if (typbyval)
		freedata = false;

	for (i = 0; i < nitems; i++)
	{
		if (nulls && nulls[i])
		{
			if (!bitmap)		/* shouldn't happen */
				elog(ERROR, "null array element where not supported");
			/* bitmap bit stays 0 */
		}
		else
		{
			bitval |= bitmask;
			p += ArrayCastAndSet(values[i], typlen, typbyval, typalign, p);
			if (freedata)
				pfree(DatumGetPointer(values[i]));
		}
		if (bitmap)
		{
			bitmask <<= 1;
			if (bitmask == 0x100)
			{
				*bitmap++ = bitval;
				bitval = 0;
				bitmask = 1;
			}
		}
	}

	if (bitmap && bitmask != 1)
		*bitmap = bitval;
}

/*
 * array_out :
 *		   takes the internal representation of an array and returns a string
 *		  containing the array in its external format.
 */
Datum
array_out(PG_FUNCTION_ARGS)
{
	ArrayType  *v = PG_GETARG_ARRAYTYPE_P(0);
	Oid			element_type = ARR_ELEMTYPE(v);
	int			typlen;
	bool		typbyval;
	char		typalign;
	char		typdelim;
	char	   *p,
			   *tmp,
			   *retval,
			  **values,
				dims_str[(MAXDIM * 33) + 2];

	/*
	 * 33 per dim since we assume 15 digits per number + ':' +'[]'
	 *
	 * +2 allows for assignment operator + trailing null
	 */
	bits8	   *bitmap;
	int			bitmask;
	bool	   *needquotes,
				needdims = false;
	int			nitems,
				overall_length,
				i,
				j,
				k,
				indx[MAXDIM];
	int			ndim,
			   *dims,
			   *lb;
	ArrayMetaState *my_extra;

	/*
	 * We arrange to look up info about element type, including its output
	 * conversion proc, only once per series of calls, assuming the element
	 * type doesn't change underneath us.
	 */
	my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL)
	{
		fcinfo->flinfo->fn_extra = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
													  sizeof(ArrayMetaState));
		my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
		my_extra->element_type = ~element_type;
	}

	if (my_extra->element_type != element_type)
	{
		/*
		 * Get info about element type, including its output conversion proc
		 */
		get_type_io_data(element_type, IOFunc_output,
						 &my_extra->typlen, &my_extra->typbyval,
						 &my_extra->typalign, &my_extra->typdelim,
						 &my_extra->typioparam, &my_extra->typiofunc);
		fmgr_info_cxt(my_extra->typiofunc, &my_extra->proc,
					  fcinfo->flinfo->fn_mcxt);
		my_extra->element_type = element_type;
	}
	typlen = my_extra->typlen;
	typbyval = my_extra->typbyval;
	typalign = my_extra->typalign;
	typdelim = my_extra->typdelim;

	ndim = ARR_NDIM(v);
	dims = ARR_DIMS(v);
	lb = ARR_LBOUND(v);
	nitems = ArrayGetNItems(ndim, dims);

	if (nitems == 0)
	{
		retval = pstrdup("{}");
		PG_RETURN_CSTRING(retval);
	}

	/*
	 * we will need to add explicit dimensions if any dimension has a lower
	 * bound other than one
	 */
	for (i = 0; i < ndim; i++)
	{
		if (lb[i] != 1)
		{
			needdims = true;
			break;
		}
	}

	/*
	 * Convert all values to string form, count total space needed (including
	 * any overhead such as escaping backslashes), and detect whether each
	 * item needs double quotes.
	 */
	values = (char **) palloc(nitems * sizeof(char *));
	needquotes = (bool *) palloc(nitems * sizeof(bool));
	overall_length = 1;			/* don't forget to count \0 at end. */

	p = ARR_DATA_PTR(v);
	bitmap = ARR_NULLBITMAP(v);
	bitmask = 1;

	for (i = 0; i < nitems; i++)
	{
		bool		needquote;

		/* Get source element, checking for NULL */
		if (bitmap && (*bitmap & bitmask) == 0)
		{
			values[i] = pstrdup("NULL");
			overall_length += 4;
			needquote = false;
		}
		else
		{
			Datum		itemvalue;

			itemvalue = fetch_att(p, typbyval, typlen);
			values[i] = OutputFunctionCall(&my_extra->proc, itemvalue);
			p = att_addlength_pointer(p, typlen, p);
			p = (char *) att_align_nominal(p, typalign);

			/* count data plus backslashes; detect chars needing quotes */
			if (values[i][0] == '\0')
				needquote = true;		/* force quotes for empty string */
			else if (pg_strcasecmp(values[i], "NULL") == 0)
				needquote = true;		/* force quotes for literal NULL */
			else
				needquote = false;

			for (tmp = values[i]; *tmp != '\0'; tmp++)
			{
				char		ch = *tmp;

				overall_length += 1;
				if (ch == '"' || ch == '\\')
				{
					needquote = true;
					overall_length += 1;
				}
				else if (ch == '{' || ch == '}' || ch == typdelim ||
						 isspace((unsigned char) ch))
					needquote = true;
			}
		}

		needquotes[i] = needquote;

		/* Count the pair of double quotes, if needed */
		if (needquote)
			overall_length += 2;
		/* and the comma */
		overall_length += 1;

		/* advance bitmap pointer if any */
		if (bitmap)
		{
			bitmask <<= 1;
			if (bitmask == 0x100)
			{
				bitmap++;
				bitmask = 1;
			}
		}
	}

	/*
	 * count total number of curly braces in output string
	 */
	for (i = j = 0, k = 1; i < ndim; i++)
		k *= dims[i], j += k;

	dims_str[0] = '\0';

	/* add explicit dimensions if required */
	if (needdims)
	{
		char	   *ptr = dims_str;

		for (i = 0; i < ndim; i++)
		{
			sprintf(ptr, "[%d:%d]", lb[i], lb[i] + dims[i] - 1);
			ptr += strlen(ptr);
		}
		*ptr++ = *ASSGN;
		*ptr = '\0';
	}

	retval = (char *) palloc(strlen(dims_str) + overall_length + 2 * j);
	p = retval;

#define APPENDSTR(str)	(strcpy(p, (str)), p += strlen(p))
#define APPENDCHAR(ch)	(*p++ = (ch), *p = '\0')

	if (needdims)
		APPENDSTR(dims_str);
	APPENDCHAR('{');
	for (i = 0; i < ndim; i++)
		indx[i] = 0;
	j = 0;
	k = 0;
	do
	{
		for (i = j; i < ndim - 1; i++)
			APPENDCHAR('{');

		if (needquotes[k])
		{
			APPENDCHAR('"');
			for (tmp = values[k]; *tmp; tmp++)
			{
				char		ch = *tmp;

				if (ch == '"' || ch == '\\')
					*p++ = '\\';
				*p++ = ch;
			}
			*p = '\0';
			APPENDCHAR('"');
		}
		else
			APPENDSTR(values[k]);
		pfree(values[k++]);

		for (i = ndim - 1; i >= 0; i--)
		{
			indx[i] = (indx[i] + 1) % dims[i];
			if (indx[i])
			{
				APPENDCHAR(typdelim);
				break;
			}
			else
				APPENDCHAR('}');
		}
		j = i;
	} while (j != -1);

#undef APPENDSTR
#undef APPENDCHAR

	pfree(values);
	pfree(needquotes);

	PG_RETURN_CSTRING(retval);
}

/*
 * array_recv :
 *		  converts an array from the external binary format to
 *		  its internal format.
 *
 * return value :
 *		  the internal representation of the input array
 */
Datum
array_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	Oid			spec_element_type = PG_GETARG_OID(1);	/* type of an array
														 * element */
	int32		typmod = PG_GETARG_INT32(2);	/* typmod for array elements */
	Oid			element_type;
	int			typlen;
	bool		typbyval;
	char		typalign;
	Oid			typioparam;
	int			i,
				nitems;
	Datum	   *dataPtr;
	bool	   *nullsPtr;
	bool		hasnulls;
	int32		nbytes;
	int32		dataoffset;
	ArrayType  *retval;
	int			ndim,
				flags,
				dim[MAXDIM],
				lBound[MAXDIM];
	ArrayMetaState *my_extra;

	/* Get the array header information */
	ndim = pq_getmsgint(buf, 4);
	if (ndim < 0)				/* we do allow zero-dimension arrays */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid number of dimensions: %d", ndim)));
	if (ndim > MAXDIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
						ndim, MAXDIM)));

	flags = pq_getmsgint(buf, 4);
	if (flags != 0 && flags != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid array flags")));

	element_type = pq_getmsgint(buf, sizeof(Oid));
	if (element_type != spec_element_type)
	{
		/* XXX Can we allow taking the input element type in any cases? */
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("wrong element type")));
	}

	for (i = 0; i < ndim; i++)
	{
		dim[i] = pq_getmsgint(buf, 4);
		lBound[i] = pq_getmsgint(buf, 4);
	}

	/* This checks for overflow of array dimensions */
	nitems = ArrayGetNItems(ndim, dim);

	/*
	 * We arrange to look up info about element type, including its receive
	 * conversion proc, only once per series of calls, assuming the element
	 * type doesn't change underneath us.
	 */
	my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL)
	{
		fcinfo->flinfo->fn_extra = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
													  sizeof(ArrayMetaState));
		my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
		my_extra->element_type = ~element_type;
	}

	if (my_extra->element_type != element_type)
	{
		/* Get info about element type, including its receive proc */
		get_type_io_data(element_type, IOFunc_receive,
						 &my_extra->typlen, &my_extra->typbyval,
						 &my_extra->typalign, &my_extra->typdelim,
						 &my_extra->typioparam, &my_extra->typiofunc);
		if (!OidIsValid(my_extra->typiofunc))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("no binary input function available for type %s",
							format_type_be(element_type))));
		fmgr_info_cxt(my_extra->typiofunc, &my_extra->proc,
					  fcinfo->flinfo->fn_mcxt);
		my_extra->element_type = element_type;
	}

	if (nitems == 0)
	{
		/* Return empty array ... but not till we've validated element_type */
		PG_RETURN_ARRAYTYPE_P(construct_empty_array(element_type));
	}

	typlen = my_extra->typlen;
	typbyval = my_extra->typbyval;
	typalign = my_extra->typalign;
	typioparam = my_extra->typioparam;

	dataPtr = (Datum *) palloc(nitems * sizeof(Datum));
	nullsPtr = (bool *) palloc(nitems * sizeof(bool));
	ReadArrayBinary(buf, nitems,
					&my_extra->proc, typioparam, typmod,
					typlen, typbyval, typalign,
					dataPtr, nullsPtr,
					&hasnulls, &nbytes);
	if (hasnulls)
	{
		dataoffset = ARR_OVERHEAD_WITHNULLS(ndim, nitems);
		nbytes += dataoffset;
	}
	else
	{
		dataoffset = 0;			/* marker for no null bitmap */
		nbytes += ARR_OVERHEAD_NONULLS(ndim);
	}
	retval = (ArrayType *) palloc(nbytes);
	SET_VARSIZE(retval, nbytes);
	retval->ndim = ndim;
	retval->dataoffset = dataoffset;
	retval->elemtype = element_type;
	memcpy(ARR_DIMS(retval), dim, ndim * sizeof(int));
	memcpy(ARR_LBOUND(retval), lBound, ndim * sizeof(int));

	CopyArrayEls(retval,
				 dataPtr, nullsPtr, nitems,
				 typlen, typbyval, typalign,
				 true);

	pfree(dataPtr);
	pfree(nullsPtr);

	PG_RETURN_ARRAYTYPE_P(retval);
}

/*
 * ReadArrayBinary:
 *	 collect the data elements of an array being read in binary style.
 *
 * Inputs:
 *	buf: the data buffer to read from.
 *	nitems: total number of array elements (already read).
 *	receiveproc: type-specific receive procedure for element datatype.
 *	typioparam, typmod: auxiliary values to pass to receiveproc.
 *	typlen, typbyval, typalign: storage parameters of element datatype.
 *
 * Outputs:
 *	values[]: filled with converted data values.
 *	nulls[]: filled with is-null markers.
 *	*hasnulls: set TRUE iff there are any null elements.
 *	*nbytes: set to total size of data area needed (including alignment
 *		padding but not including array header overhead).
 *
 * Note that values[] and nulls[] are allocated by the caller, and must have
 * nitems elements.
 */
static void
ReadArrayBinary(StringInfo buf,
				int nitems,
				FmgrInfo *receiveproc,
				Oid typioparam,
				int32 typmod,
				int typlen,
				bool typbyval,
				char typalign,
				Datum *values,
				bool *nulls,
				bool *hasnulls,
				int32 *nbytes)
{
	int			i;
	bool		hasnull;
	int32		totbytes;

	for (i = 0; i < nitems; i++)
	{
		int			itemlen;
		StringInfoData elem_buf;
		char		csave;

		/* Get and check the item length */
		itemlen = pq_getmsgint(buf, 4);
		if (itemlen < -1 || itemlen > (buf->len - buf->cursor))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("insufficient data left in message")));

		if (itemlen == -1)
		{
			/* -1 length means NULL */
			values[i] = ReceiveFunctionCall(receiveproc, NULL,
											typioparam, typmod);
			nulls[i] = true;
			continue;
		}

		/*
		 * Rather than copying data around, we just set up a phony StringInfo
		 * pointing to the correct portion of the input buffer. We assume we
		 * can scribble on the input buffer so as to maintain the convention
		 * that StringInfos have a trailing null.
		 */
		elem_buf.data = &buf->data[buf->cursor];
		elem_buf.maxlen = itemlen + 1;
		elem_buf.len = itemlen;
		elem_buf.cursor = 0;

		buf->cursor += itemlen;

		csave = buf->data[buf->cursor];
		buf->data[buf->cursor] = '\0';

		/* Now call the element's receiveproc */
		values[i] = ReceiveFunctionCall(receiveproc, &elem_buf,
										typioparam, typmod);
		nulls[i] = false;

		/* Trouble if it didn't eat the whole buffer */
		if (elem_buf.cursor != itemlen)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("improper binary format in array element %d",
							i + 1)));

		buf->data[buf->cursor] = csave;
	}

	/*
	 * Check for nulls, compute total data space needed
	 */
	hasnull = false;
	totbytes = 0;
	for (i = 0; i < nitems; i++)
	{
		if (nulls[i])
			hasnull = true;
		else
		{
			/* let's just make sure data is not toasted */
			if (typlen == -1)
				values[i] = PointerGetDatum(PG_DETOAST_DATUM(values[i]));
			totbytes = att_addlength_datum(totbytes, typlen, values[i]);
			totbytes = att_align_nominal(totbytes, typalign);
			/* check for overflow of total request */
			if (!AllocSizeIsValid(totbytes))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("array size exceeds the maximum allowed (%d)",
								(int) MaxAllocSize)));
		}
	}
	*hasnulls = hasnull;
	*nbytes = totbytes;
}


/*
 * array_send :
 *		  takes the internal representation of an array and returns a bytea
 *		  containing the array in its external binary format.
 */
Datum
array_send(PG_FUNCTION_ARGS)
{
	ArrayType  *v = PG_GETARG_ARRAYTYPE_P(0);
	Oid			element_type = ARR_ELEMTYPE(v);
	int			typlen;
	bool		typbyval;
	char		typalign;
	char	   *p;
	bits8	   *bitmap;
	int			bitmask;
	int			nitems,
				i;
	int			ndim,
			   *dim;
	StringInfoData buf;
	ArrayMetaState *my_extra;

	/*
	 * We arrange to look up info about element type, including its send
	 * conversion proc, only once per series of calls, assuming the element
	 * type doesn't change underneath us.
	 */
	my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL)
	{
		fcinfo->flinfo->fn_extra = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
													  sizeof(ArrayMetaState));
		my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
		my_extra->element_type = ~element_type;
	}

	if (my_extra->element_type != element_type)
	{
		/* Get info about element type, including its send proc */
		get_type_io_data(element_type, IOFunc_send,
						 &my_extra->typlen, &my_extra->typbyval,
						 &my_extra->typalign, &my_extra->typdelim,
						 &my_extra->typioparam, &my_extra->typiofunc);
		if (!OidIsValid(my_extra->typiofunc))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("no binary output function available for type %s",
							format_type_be(element_type))));
		fmgr_info_cxt(my_extra->typiofunc, &my_extra->proc,
					  fcinfo->flinfo->fn_mcxt);
		my_extra->element_type = element_type;
	}
	typlen = my_extra->typlen;
	typbyval = my_extra->typbyval;
	typalign = my_extra->typalign;

	ndim = ARR_NDIM(v);
	dim = ARR_DIMS(v);
	nitems = ArrayGetNItems(ndim, dim);

	pq_begintypsend(&buf);

	/* Send the array header information */
	pq_sendint(&buf, ndim, 4);
	pq_sendint(&buf, ARR_HASNULL(v) ? 1 : 0, 4);
	pq_sendint(&buf, element_type, sizeof(Oid));
	for (i = 0; i < ndim; i++)
	{
		pq_sendint(&buf, ARR_DIMS(v)[i], 4);
		pq_sendint(&buf, ARR_LBOUND(v)[i], 4);
	}

	/* Send the array elements using the element's own sendproc */
	p = ARR_DATA_PTR(v);
	bitmap = ARR_NULLBITMAP(v);
	bitmask = 1;

	for (i = 0; i < nitems; i++)
	{
		/* Get source element, checking for NULL */
		if (bitmap && (*bitmap & bitmask) == 0)
		{
			/* -1 length means a NULL */
			pq_sendint(&buf, -1, 4);
		}
		else
		{
			Datum		itemvalue;
			bytea	   *outputbytes;

			itemvalue = fetch_att(p, typbyval, typlen);
			outputbytes = SendFunctionCall(&my_extra->proc, itemvalue);
			pq_sendint(&buf, VARSIZE(outputbytes) - VARHDRSZ, 4);
			pq_sendbytes(&buf, VARDATA(outputbytes),
						 VARSIZE(outputbytes) - VARHDRSZ);
			pfree(outputbytes);

			p = att_addlength_pointer(p, typlen, p);
			p = (char *) att_align_nominal(p, typalign);
		}

		/* advance bitmap pointer if any */
		if (bitmap)
		{
			bitmask <<= 1;
			if (bitmask == 0x100)
			{
				bitmap++;
				bitmask = 1;
			}
		}
	}

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * array_ndims :
 *		  returns the number of dimensions of the array pointed to by "v"
 */
Datum
array_ndims(PG_FUNCTION_ARGS)
{
	ArrayType  *v = PG_GETARG_ARRAYTYPE_P(0);

	/* Sanity check: does it look like an array at all? */
	if (ARR_NDIM(v) <= 0 || ARR_NDIM(v) > MAXDIM)
		PG_RETURN_NULL();

	PG_RETURN_INT32(ARR_NDIM(v));
}

/*
 * array_dims :
 *		  returns the dimensions of the array pointed to by "v", as a "text"
 */
Datum
array_dims(PG_FUNCTION_ARGS)
{
	ArrayType  *v = PG_GETARG_ARRAYTYPE_P(0);
	char	   *p;
	int			i;
	int		   *dimv,
			   *lb;

	/*
	 * 33 since we assume 15 digits per number + ':' +'[]'
	 *
	 * +1 for trailing null
	 */
	char		buf[MAXDIM * 33 + 1];

	/* Sanity check: does it look like an array at all? */
	if (ARR_NDIM(v) <= 0 || ARR_NDIM(v) > MAXDIM)
		PG_RETURN_NULL();

	dimv = ARR_DIMS(v);
	lb = ARR_LBOUND(v);

	p = buf;
	for (i = 0; i < ARR_NDIM(v); i++)
	{
		sprintf(p, "[%d:%d]", lb[i], dimv[i] + lb[i] - 1);
		p += strlen(p);
	}

	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

/*
 * array_lower :
 *		returns the lower dimension, of the DIM requested, for
 *		the array pointed to by "v", as an int4
 */
Datum
array_lower(PG_FUNCTION_ARGS)
{
	ArrayType  *v = PG_GETARG_ARRAYTYPE_P(0);
	int			reqdim = PG_GETARG_INT32(1);
	int		   *lb;
	int			result;

	/* Sanity check: does it look like an array at all? */
	if (ARR_NDIM(v) <= 0 || ARR_NDIM(v) > MAXDIM)
		PG_RETURN_NULL();

	/* Sanity check: was the requested dim valid */
	if (reqdim <= 0 || reqdim > ARR_NDIM(v))
		PG_RETURN_NULL();

	lb = ARR_LBOUND(v);
	result = lb[reqdim - 1];

	PG_RETURN_INT32(result);
}

/*
 * array_upper :
 *		returns the upper dimension, of the DIM requested, for
 *		the array pointed to by "v", as an int4
 */
Datum
array_upper(PG_FUNCTION_ARGS)
{
	ArrayType  *v = PG_GETARG_ARRAYTYPE_P(0);
	int			reqdim = PG_GETARG_INT32(1);
	int		   *dimv,
			   *lb;
	int			result;

	/* Sanity check: does it look like an array at all? */
	if (ARR_NDIM(v) <= 0 || ARR_NDIM(v) > MAXDIM)
		PG_RETURN_NULL();

	/* Sanity check: was the requested dim valid */
	if (reqdim <= 0 || reqdim > ARR_NDIM(v))
		PG_RETURN_NULL();

	lb = ARR_LBOUND(v);
	dimv = ARR_DIMS(v);

	result = dimv[reqdim - 1] + lb[reqdim - 1] - 1;

	PG_RETURN_INT32(result);
}

/*
 * array_length :
 *		returns the length, of the dimension requested, for
 *		the array pointed to by "v", as an int4
 */
Datum
array_length(PG_FUNCTION_ARGS)
{
	ArrayType  *v = PG_GETARG_ARRAYTYPE_P(0);
	int			reqdim = PG_GETARG_INT32(1);
	int		   *dimv;
	int			result;

	/* Sanity check: does it look like an array at all? */
	if (ARR_NDIM(v) <= 0 || ARR_NDIM(v) > MAXDIM)
		PG_RETURN_NULL();

	/* Sanity check: was the requested dim valid */
	if (reqdim <= 0 || reqdim > ARR_NDIM(v))
		PG_RETURN_NULL();

	dimv = ARR_DIMS(v);

	result = dimv[reqdim - 1];

	PG_RETURN_INT32(result);
}

/*
 * array_ref :
 *	  This routine takes an array pointer and a subscript array and returns
 *	  the referenced item as a Datum.  Note that for a pass-by-reference
 *	  datatype, the returned Datum is a pointer into the array object.
 *
 * This handles both ordinary varlena arrays and fixed-length arrays.
 *
 * Inputs:
 *	array: the array object (mustn't be NULL)
 *	nSubscripts: number of subscripts supplied
 *	indx[]: the subscript values
 *	arraytyplen: pg_type.typlen for the array type
 *	elmlen: pg_type.typlen for the array's element type
 *	elmbyval: pg_type.typbyval for the array's element type
 *	elmalign: pg_type.typalign for the array's element type
 *
 * Outputs:
 *	The return value is the element Datum.
 *	*isNull is set to indicate whether the element is NULL.
 */
Datum
array_ref(ArrayType *array,
		  int nSubscripts,
		  int *indx,
		  int arraytyplen,
		  int elmlen,
		  bool elmbyval,
		  char elmalign,
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
	bits8	   *arraynullsptr;

	if (arraytyplen > 0)
	{
		/*
		 * fixed-length arrays -- these are assumed to be 1-d, 0-based
		 */
		ndim = 1;
		fixedDim[0] = arraytyplen / elmlen;
		fixedLb[0] = 0;
		dim = fixedDim;
		lb = fixedLb;
		arraydataptr = (char *) array;
		arraynullsptr = NULL;
	}
	else
	{
		/* detoast input array if necessary */
		array = DatumGetArrayTypeP(PointerGetDatum(array));

		ndim = ARR_NDIM(array);
		dim = ARR_DIMS(array);
		lb = ARR_LBOUND(array);
		arraydataptr = ARR_DATA_PTR(array);
		arraynullsptr = ARR_NULLBITMAP(array);
	}

	/*
	 * Return NULL for invalid subscript
	 */
	if (ndim != nSubscripts || ndim <= 0 || ndim > MAXDIM)
	{
		*isNull = true;
		return (Datum) 0;
	}
	for (i = 0; i < ndim; i++)
	{
		if (indx[i] < lb[i] || indx[i] >= (dim[i] + lb[i]))
		{
			*isNull = true;
			return (Datum) 0;
		}
	}

	/*
	 * Calculate the element number
	 */
	offset = ArrayGetOffset(nSubscripts, dim, lb, indx);

	/*
	 * Check for NULL array element
	 */
	if (array_get_isnull(arraynullsptr, offset))
	{
		*isNull = true;
		return (Datum) 0;
	}

	/*
	 * OK, get the element
	 */
	*isNull = false;
	retptr = array_seek(arraydataptr, 0, arraynullsptr, offset,
						elmlen, elmbyval, elmalign);
	return ArrayCast(retptr, elmbyval, elmlen);
}

/*
 * array_get_slice :
 *		   This routine takes an array and a range of indices (upperIndex and
 *		   lowerIndx), creates a new array structure for the referred elements
 *		   and returns a pointer to it.
 *
 * This handles both ordinary varlena arrays and fixed-length arrays.
 *
 * Inputs:
 *	array: the array object (mustn't be NULL)
 *	nSubscripts: number of subscripts supplied (must be same for upper/lower)
 *	upperIndx[]: the upper subscript values
 *	lowerIndx[]: the lower subscript values
 *	arraytyplen: pg_type.typlen for the array type
 *	elmlen: pg_type.typlen for the array's element type
 *	elmbyval: pg_type.typbyval for the array's element type
 *	elmalign: pg_type.typalign for the array's element type
 *
 * Outputs:
 *	The return value is the new array Datum (it's never NULL)
 *
 * NOTE: we assume it is OK to scribble on the provided subscript arrays
 * lowerIndx[] and upperIndx[].  These are generally just temporaries.
 */
ArrayType *
array_get_slice(ArrayType *array,
				int nSubscripts,
				int *upperIndx,
				int *lowerIndx,
				int arraytyplen,
				int elmlen,
				bool elmbyval,
				char elmalign)
{
	ArrayType  *newarray;
	int			i,
				ndim,
			   *dim,
			   *lb,
			   *newlb;
	int			fixedDim[1],
				fixedLb[1];
	Oid			elemtype;
	char	   *arraydataptr;
	bits8	   *arraynullsptr;
	int32		dataoffset;
	int			bytes,
				span[MAXDIM];

	if (arraytyplen > 0)
	{
		/*
		 * fixed-length arrays -- currently, cannot slice these because parser
		 * labels output as being of the fixed-length array type! Code below
		 * shows how we could support it if the parser were changed to label
		 * output as a suitable varlena array type.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("slices of fixed-length arrays not implemented")));

		/*
		 * fixed-length arrays -- these are assumed to be 1-d, 0-based
		 *
		 * XXX where would we get the correct ELEMTYPE from?
		 */
		ndim = 1;
		fixedDim[0] = arraytyplen / elmlen;
		fixedLb[0] = 0;
		dim = fixedDim;
		lb = fixedLb;
		elemtype = InvalidOid;	/* XXX */
		arraydataptr = (char *) array;
		arraynullsptr = NULL;
	}
	else
	{
		/* detoast input array if necessary */
		array = DatumGetArrayTypeP(PointerGetDatum(array));

		ndim = ARR_NDIM(array);
		dim = ARR_DIMS(array);
		lb = ARR_LBOUND(array);
		elemtype = ARR_ELEMTYPE(array);
		arraydataptr = ARR_DATA_PTR(array);
		arraynullsptr = ARR_NULLBITMAP(array);
	}

	/*
	 * Check provided subscripts.  A slice exceeding the current array limits
	 * is silently truncated to the array limits.  If we end up with an empty
	 * slice, return an empty array.
	 */
	if (ndim < nSubscripts || ndim <= 0 || ndim > MAXDIM)
		return construct_empty_array(elemtype);

	for (i = 0; i < nSubscripts; i++)
	{
		if (lowerIndx[i] < lb[i])
			lowerIndx[i] = lb[i];
		if (upperIndx[i] >= (dim[i] + lb[i]))
			upperIndx[i] = dim[i] + lb[i] - 1;
		if (lowerIndx[i] > upperIndx[i])
			return construct_empty_array(elemtype);
	}
	/* fill any missing subscript positions with full array range */
	for (; i < ndim; i++)
	{
		lowerIndx[i] = lb[i];
		upperIndx[i] = dim[i] + lb[i] - 1;
		if (lowerIndx[i] > upperIndx[i])
			return construct_empty_array(elemtype);
	}

	mda_get_range(ndim, span, lowerIndx, upperIndx);

	bytes = array_slice_size(arraydataptr, arraynullsptr,
							 ndim, dim, lb,
							 lowerIndx, upperIndx,
							 elmlen, elmbyval, elmalign);

	/*
	 * Currently, we put a null bitmap in the result if the source has one;
	 * could be smarter ...
	 */
	if (arraynullsptr)
	{
		dataoffset = ARR_OVERHEAD_WITHNULLS(ndim, ArrayGetNItems(ndim, span));
		bytes += dataoffset;
	}
	else
	{
		dataoffset = 0;			/* marker for no null bitmap */
		bytes += ARR_OVERHEAD_NONULLS(ndim);
	}

	newarray = (ArrayType *) palloc(bytes);
	SET_VARSIZE(newarray, bytes);
	newarray->ndim = ndim;
	newarray->dataoffset = dataoffset;
	newarray->elemtype = elemtype;
	memcpy(ARR_DIMS(newarray), span, ndim * sizeof(int));

	/*
	 * Lower bounds of the new array are set to 1.	Formerly (before 7.3) we
	 * copied the given lowerIndx values ... but that seems confusing.
	 */
	newlb = ARR_LBOUND(newarray);
	for (i = 0; i < ndim; i++)
		newlb[i] = 1;

	array_extract_slice(newarray,
						ndim, dim, lb,
						arraydataptr, arraynullsptr,
						lowerIndx, upperIndx,
						elmlen, elmbyval, elmalign);

	return newarray;
}

/*
 * array_set :
 *		  This routine sets the value of an array element (specified by
 *		  a subscript array) to a new value specified by "dataValue".
 *
 * This handles both ordinary varlena arrays and fixed-length arrays.
 *
 * Inputs:
 *	array: the initial array object (mustn't be NULL)
 *	nSubscripts: number of subscripts supplied
 *	indx[]: the subscript values
 *	dataValue: the datum to be inserted at the given position
 *	isNull: whether dataValue is NULL
 *	arraytyplen: pg_type.typlen for the array type
 *	elmlen: pg_type.typlen for the array's element type
 *	elmbyval: pg_type.typbyval for the array's element type
 *	elmalign: pg_type.typalign for the array's element type
 *
 * Result:
 *		  A new array is returned, just like the old except for the one
 *		  modified entry.  The original array object is not changed.
 *
 * For one-dimensional arrays only, we allow the array to be extended
 * by assigning to a position outside the existing subscript range; any
 * positions between the existing elements and the new one are set to NULLs.
 * (XXX TODO: allow a corresponding behavior for multidimensional arrays)
 *
 * NOTE: For assignments, we throw an error for invalid subscripts etc,
 * rather than returning a NULL as the fetch operations do.
 */
ArrayType *
array_set(ArrayType *array,
		  int nSubscripts,
		  int *indx,
		  Datum dataValue,
		  bool isNull,
		  int arraytyplen,
		  int elmlen,
		  bool elmbyval,
		  char elmalign)
{
	ArrayType  *newarray;
	int			i,
				ndim,
				dim[MAXDIM],
				lb[MAXDIM],
				offset;
	char	   *elt_ptr;
	bool		newhasnulls;
	bits8	   *oldnullbitmap;
	int			oldnitems,
				newnitems,
				olddatasize,
				newsize,
				olditemlen,
				newitemlen,
				overheadlen,
				oldoverheadlen,
				addedbefore,
				addedafter,
				lenbefore,
				lenafter;

	if (arraytyplen > 0)
	{
		/*
		 * fixed-length arrays -- these are assumed to be 1-d, 0-based. We
		 * cannot extend them, either.
		 */
		if (nSubscripts != 1)
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("wrong number of array subscripts")));

		if (indx[0] < 0 || indx[0] * elmlen >= arraytyplen)
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("array subscript out of range")));

		if (isNull)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("cannot assign null value to an element of a fixed-length array")));

		newarray = (ArrayType *) palloc(arraytyplen);
		memcpy(newarray, array, arraytyplen);
		elt_ptr = (char *) newarray + indx[0] * elmlen;
		ArrayCastAndSet(dataValue, elmlen, elmbyval, elmalign, elt_ptr);
		return newarray;
	}

	if (nSubscripts <= 0 || nSubscripts > MAXDIM)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("wrong number of array subscripts")));

	/* make sure item to be inserted is not toasted */
	if (elmlen == -1 && !isNull)
		dataValue = PointerGetDatum(PG_DETOAST_DATUM(dataValue));

	/* detoast input array if necessary */
	array = DatumGetArrayTypeP(PointerGetDatum(array));

	ndim = ARR_NDIM(array);

	/*
	 * if number of dims is zero, i.e. an empty array, create an array with
	 * nSubscripts dimensions, and set the lower bounds to the supplied
	 * subscripts
	 */
	if (ndim == 0)
	{
		Oid			elmtype = ARR_ELEMTYPE(array);

		for (i = 0; i < nSubscripts; i++)
		{
			dim[i] = 1;
			lb[i] = indx[i];
		}

		return construct_md_array(&dataValue, &isNull, nSubscripts,
								  dim, lb, elmtype,
								  elmlen, elmbyval, elmalign);
	}

	if (ndim != nSubscripts)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("wrong number of array subscripts")));

	/* copy dim/lb since we may modify them */
	memcpy(dim, ARR_DIMS(array), ndim * sizeof(int));
	memcpy(lb, ARR_LBOUND(array), ndim * sizeof(int));

	newhasnulls = (ARR_HASNULL(array) || isNull);
	addedbefore = addedafter = 0;

	/*
	 * Check subscripts
	 */
	if (ndim == 1)
	{
		if (indx[0] < lb[0])
		{
			addedbefore = lb[0] - indx[0];
			dim[0] += addedbefore;
			lb[0] = indx[0];
			if (addedbefore > 1)
				newhasnulls = true;		/* will insert nulls */
		}
		if (indx[0] >= (dim[0] + lb[0]))
		{
			addedafter = indx[0] - (dim[0] + lb[0]) + 1;
			dim[0] += addedafter;
			if (addedafter > 1)
				newhasnulls = true;		/* will insert nulls */
		}
	}
	else
	{
		/*
		 * XXX currently we do not support extending multi-dimensional arrays
		 * during assignment
		 */
		for (i = 0; i < ndim; i++)
		{
			if (indx[i] < lb[i] ||
				indx[i] >= (dim[i] + lb[i]))
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("array subscript out of range")));
		}
	}

	/*
	 * Compute sizes of items and areas to copy
	 */
	newnitems = ArrayGetNItems(ndim, dim);
	if (newhasnulls)
		overheadlen = ARR_OVERHEAD_WITHNULLS(ndim, newnitems);
	else
		overheadlen = ARR_OVERHEAD_NONULLS(ndim);
	oldnitems = ArrayGetNItems(ndim, ARR_DIMS(array));
	oldnullbitmap = ARR_NULLBITMAP(array);
	oldoverheadlen = ARR_DATA_OFFSET(array);
	olddatasize = ARR_SIZE(array) - oldoverheadlen;
	if (addedbefore)
	{
		offset = 0;
		lenbefore = 0;
		olditemlen = 0;
		lenafter = olddatasize;
	}
	else if (addedafter)
	{
		offset = oldnitems;
		lenbefore = olddatasize;
		olditemlen = 0;
		lenafter = 0;
	}
	else
	{
		offset = ArrayGetOffset(nSubscripts, dim, lb, indx);
		elt_ptr = array_seek(ARR_DATA_PTR(array), 0, oldnullbitmap, offset,
							 elmlen, elmbyval, elmalign);
		lenbefore = (int) (elt_ptr - ARR_DATA_PTR(array));
		if (array_get_isnull(oldnullbitmap, offset))
			olditemlen = 0;
		else
		{
			olditemlen = att_addlength_pointer(0, elmlen, elt_ptr);
			olditemlen = att_align_nominal(olditemlen, elmalign);
		}
		lenafter = (int) (olddatasize - lenbefore - olditemlen);
	}

	if (isNull)
		newitemlen = 0;
	else
	{
		newitemlen = att_addlength_datum(0, elmlen, dataValue);
		newitemlen = att_align_nominal(newitemlen, elmalign);
	}

	newsize = overheadlen + lenbefore + newitemlen + lenafter;

	/*
	 * OK, create the new array and fill in header/dimensions
	 */
	newarray = (ArrayType *) palloc(newsize);
	SET_VARSIZE(newarray, newsize);
	newarray->ndim = ndim;
	newarray->dataoffset = newhasnulls ? overheadlen : 0;
	newarray->elemtype = ARR_ELEMTYPE(array);
	memcpy(ARR_DIMS(newarray), dim, ndim * sizeof(int));
	memcpy(ARR_LBOUND(newarray), lb, ndim * sizeof(int));

	/*
	 * Fill in data
	 */
	memcpy((char *) newarray + overheadlen,
		   (char *) array + oldoverheadlen,
		   lenbefore);
	if (!isNull)
		ArrayCastAndSet(dataValue, elmlen, elmbyval, elmalign,
						(char *) newarray + overheadlen + lenbefore);
	memcpy((char *) newarray + overheadlen + lenbefore + newitemlen,
		   (char *) array + oldoverheadlen + lenbefore + olditemlen,
		   lenafter);

	/*
	 * Fill in nulls bitmap if needed
	 *
	 * Note: it's possible we just replaced the last NULL with a non-NULL, and
	 * could get rid of the bitmap.  Seems not worth testing for though.
	 */
	if (newhasnulls)
	{
		bits8	   *newnullbitmap = ARR_NULLBITMAP(newarray);

		/* Zero the bitmap to take care of marking inserted positions null */
		MemSet(newnullbitmap, 0, (newnitems + 7) / 8);
		/* Fix the inserted value */
		if (addedafter)
			array_set_isnull(newnullbitmap, newnitems - 1, isNull);
		else
			array_set_isnull(newnullbitmap, offset, isNull);
		/* Fix the copied range(s) */
		if (addedbefore)
			array_bitmap_copy(newnullbitmap, addedbefore,
							  oldnullbitmap, 0,
							  oldnitems);
		else
		{
			array_bitmap_copy(newnullbitmap, 0,
							  oldnullbitmap, 0,
							  offset);
			if (addedafter == 0)
				array_bitmap_copy(newnullbitmap, offset + 1,
								  oldnullbitmap, offset + 1,
								  oldnitems - offset - 1);
		}
	}

	return newarray;
}

/*
 * array_set_slice :
 *		  This routine sets the value of a range of array locations (specified
 *		  by upper and lower subscript values) to new values passed as
 *		  another array.
 *
 * This handles both ordinary varlena arrays and fixed-length arrays.
 *
 * Inputs:
 *	array: the initial array object (mustn't be NULL)
 *	nSubscripts: number of subscripts supplied (must be same for upper/lower)
 *	upperIndx[]: the upper subscript values
 *	lowerIndx[]: the lower subscript values
 *	srcArray: the source for the inserted values
 *	isNull: indicates whether srcArray is NULL
 *	arraytyplen: pg_type.typlen for the array type
 *	elmlen: pg_type.typlen for the array's element type
 *	elmbyval: pg_type.typbyval for the array's element type
 *	elmalign: pg_type.typalign for the array's element type
 *
 * Result:
 *		  A new array is returned, just like the old except for the
 *		  modified range.  The original array object is not changed.
 *
 * For one-dimensional arrays only, we allow the array to be extended
 * by assigning to positions outside the existing subscript range; any
 * positions between the existing elements and the new ones are set to NULLs.
 * (XXX TODO: allow a corresponding behavior for multidimensional arrays)
 *
 * NOTE: we assume it is OK to scribble on the provided index arrays
 * lowerIndx[] and upperIndx[].  These are generally just temporaries.
 *
 * NOTE: For assignments, we throw an error for silly subscripts etc,
 * rather than returning a NULL or empty array as the fetch operations do.
 */
ArrayType *
array_set_slice(ArrayType *array,
				int nSubscripts,
				int *upperIndx,
				int *lowerIndx,
				ArrayType *srcArray,
				bool isNull,
				int arraytyplen,
				int elmlen,
				bool elmbyval,
				char elmalign)
{
	ArrayType  *newarray;
	int			i,
				ndim,
				dim[MAXDIM],
				lb[MAXDIM],
				span[MAXDIM];
	bool		newhasnulls;
	int			nitems,
				nsrcitems,
				olddatasize,
				newsize,
				olditemsize,
				newitemsize,
				overheadlen,
				oldoverheadlen,
				addedbefore,
				addedafter,
				lenbefore,
				lenafter,
				itemsbefore,
				itemsafter,
				nolditems;

	/* Currently, assignment from a NULL source array is a no-op */
	if (isNull)
		return array;

	if (arraytyplen > 0)
	{
		/*
		 * fixed-length arrays -- not got round to doing this...
		 */
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		errmsg("updates on slices of fixed-length arrays not implemented")));
	}

	/* detoast arrays if necessary */
	array = DatumGetArrayTypeP(PointerGetDatum(array));
	srcArray = DatumGetArrayTypeP(PointerGetDatum(srcArray));

	/* note: we assume srcArray contains no toasted elements */

	ndim = ARR_NDIM(array);

	/*
	 * if number of dims is zero, i.e. an empty array, create an array with
	 * nSubscripts dimensions, and set the upper and lower bounds to the
	 * supplied subscripts
	 */
	if (ndim == 0)
	{
		Datum	   *dvalues;
		bool	   *dnulls;
		int			nelems;
		Oid			elmtype = ARR_ELEMTYPE(array);

		deconstruct_array(srcArray, elmtype, elmlen, elmbyval, elmalign,
						  &dvalues, &dnulls, &nelems);

		for (i = 0; i < nSubscripts; i++)
		{
			dim[i] = 1 + upperIndx[i] - lowerIndx[i];
			lb[i] = lowerIndx[i];
		}

		/* complain if too few source items; we ignore extras, however */
		if (nelems < ArrayGetNItems(nSubscripts, dim))
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("source array too small")));

		return construct_md_array(dvalues, dnulls, nSubscripts,
								  dim, lb, elmtype,
								  elmlen, elmbyval, elmalign);
	}

	if (ndim < nSubscripts || ndim <= 0 || ndim > MAXDIM)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("wrong number of array subscripts")));

	/* copy dim/lb since we may modify them */
	memcpy(dim, ARR_DIMS(array), ndim * sizeof(int));
	memcpy(lb, ARR_LBOUND(array), ndim * sizeof(int));

	newhasnulls = (ARR_HASNULL(array) || ARR_HASNULL(srcArray));
	addedbefore = addedafter = 0;

	/*
	 * Check subscripts
	 */
	if (ndim == 1)
	{
		Assert(nSubscripts == 1);
		if (lowerIndx[0] > upperIndx[0])
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("upper bound cannot be less than lower bound")));
		if (lowerIndx[0] < lb[0])
		{
			if (upperIndx[0] < lb[0] - 1)
				newhasnulls = true;		/* will insert nulls */
			addedbefore = lb[0] - lowerIndx[0];
			dim[0] += addedbefore;
			lb[0] = lowerIndx[0];
		}
		if (upperIndx[0] >= (dim[0] + lb[0]))
		{
			if (lowerIndx[0] > (dim[0] + lb[0]))
				newhasnulls = true;		/* will insert nulls */
			addedafter = upperIndx[0] - (dim[0] + lb[0]) + 1;
			dim[0] += addedafter;
		}
	}
	else
	{
		/*
		 * XXX currently we do not support extending multi-dimensional arrays
		 * during assignment
		 */
		for (i = 0; i < nSubscripts; i++)
		{
			if (lowerIndx[i] > upperIndx[i])
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("upper bound cannot be less than lower bound")));
			if (lowerIndx[i] < lb[i] ||
				upperIndx[i] >= (dim[i] + lb[i]))
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("array subscript out of range")));
		}
		/* fill any missing subscript positions with full array range */
		for (; i < ndim; i++)
		{
			lowerIndx[i] = lb[i];
			upperIndx[i] = dim[i] + lb[i] - 1;
			if (lowerIndx[i] > upperIndx[i])
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("upper bound cannot be less than lower bound")));
		}
	}

	/* Do this mainly to check for overflow */
	nitems = ArrayGetNItems(ndim, dim);

	/*
	 * Make sure source array has enough entries.  Note we ignore the shape of
	 * the source array and just read entries serially.
	 */
	mda_get_range(ndim, span, lowerIndx, upperIndx);
	nsrcitems = ArrayGetNItems(ndim, span);
	if (nsrcitems > ArrayGetNItems(ARR_NDIM(srcArray), ARR_DIMS(srcArray)))
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("source array too small")));

	/*
	 * Compute space occupied by new entries, space occupied by replaced
	 * entries, and required space for new array.
	 */
	if (newhasnulls)
		overheadlen = ARR_OVERHEAD_WITHNULLS(ndim, nitems);
	else
		overheadlen = ARR_OVERHEAD_NONULLS(ndim);
	newitemsize = array_nelems_size(ARR_DATA_PTR(srcArray), 0,
									ARR_NULLBITMAP(srcArray), nsrcitems,
									elmlen, elmbyval, elmalign);
	oldoverheadlen = ARR_DATA_OFFSET(array);
	olddatasize = ARR_SIZE(array) - oldoverheadlen;
	if (ndim > 1)
	{
		/*
		 * here we do not need to cope with extension of the array; it would
		 * be a lot more complicated if we had to do so...
		 */
		olditemsize = array_slice_size(ARR_DATA_PTR(array),
									   ARR_NULLBITMAP(array),
									   ndim, dim, lb,
									   lowerIndx, upperIndx,
									   elmlen, elmbyval, elmalign);
		lenbefore = lenafter = 0;		/* keep compiler quiet */
		itemsbefore = itemsafter = nolditems = 0;
	}
	else
	{
		/*
		 * here we must allow for possibility of slice larger than orig array
		 */
		int			oldlb = ARR_LBOUND(array)[0];
		int			oldub = oldlb + ARR_DIMS(array)[0] - 1;
		int			slicelb = Max(oldlb, lowerIndx[0]);
		int			sliceub = Min(oldub, upperIndx[0]);
		char	   *oldarraydata = ARR_DATA_PTR(array);
		bits8	   *oldarraybitmap = ARR_NULLBITMAP(array);

		itemsbefore = Min(slicelb, oldub + 1) - oldlb;
		lenbefore = array_nelems_size(oldarraydata, 0, oldarraybitmap,
									  itemsbefore,
									  elmlen, elmbyval, elmalign);
		if (slicelb > sliceub)
		{
			nolditems = 0;
			olditemsize = 0;
		}
		else
		{
			nolditems = sliceub - slicelb + 1;
			olditemsize = array_nelems_size(oldarraydata + lenbefore,
											itemsbefore, oldarraybitmap,
											nolditems,
											elmlen, elmbyval, elmalign);
		}
		itemsafter = oldub - sliceub;
		lenafter = olddatasize - lenbefore - olditemsize;
	}

	newsize = overheadlen + olddatasize - olditemsize + newitemsize;

	newarray = (ArrayType *) palloc(newsize);
	SET_VARSIZE(newarray, newsize);
	newarray->ndim = ndim;
	newarray->dataoffset = newhasnulls ? overheadlen : 0;
	newarray->elemtype = ARR_ELEMTYPE(array);
	memcpy(ARR_DIMS(newarray), dim, ndim * sizeof(int));
	memcpy(ARR_LBOUND(newarray), lb, ndim * sizeof(int));

	if (ndim > 1)
	{
		/*
		 * here we do not need to cope with extension of the array; it would
		 * be a lot more complicated if we had to do so...
		 */
		array_insert_slice(newarray, array, srcArray,
						   ndim, dim, lb,
						   lowerIndx, upperIndx,
						   elmlen, elmbyval, elmalign);
	}
	else
	{
		/* fill in data */
		memcpy((char *) newarray + overheadlen,
			   (char *) array + oldoverheadlen,
			   lenbefore);
		memcpy((char *) newarray + overheadlen + lenbefore,
			   ARR_DATA_PTR(srcArray),
			   newitemsize);
		memcpy((char *) newarray + overheadlen + lenbefore + newitemsize,
			   (char *) array + oldoverheadlen + lenbefore + olditemsize,
			   lenafter);
		/* fill in nulls bitmap if needed */
		if (newhasnulls)
		{
			bits8	   *newnullbitmap = ARR_NULLBITMAP(newarray);
			bits8	   *oldnullbitmap = ARR_NULLBITMAP(array);

			/* Zero the bitmap to handle marking inserted positions null */
			MemSet(newnullbitmap, 0, (nitems + 7) / 8);
			array_bitmap_copy(newnullbitmap, addedbefore,
							  oldnullbitmap, 0,
							  itemsbefore);
			array_bitmap_copy(newnullbitmap, lowerIndx[0] - lb[0],
							  ARR_NULLBITMAP(srcArray), 0,
							  nsrcitems);
			array_bitmap_copy(newnullbitmap, addedbefore + itemsbefore + nolditems,
							  oldnullbitmap, itemsbefore + nolditems,
							  itemsafter);
		}
	}

	return newarray;
}

/*
 * array_map()
 *
 * Map an array through an arbitrary function.	Return a new array with
 * same dimensions and each source element transformed by fn().  Each
 * source element is passed as the first argument to fn(); additional
 * arguments to be passed to fn() can be specified by the caller.
 * The output array can have a different element type than the input.
 *
 * Parameters are:
 * * fcinfo: a function-call data structure pre-constructed by the caller
 *	 to be ready to call the desired function, with everything except the
 *	 first argument position filled in.  In particular, flinfo identifies
 *	 the function fn(), and if nargs > 1 then argument positions after the
 *	 first must be preset to the additional values to be passed.  The
 *	 first argument position initially holds the input array value.
 * * inpType: OID of element type of input array.  This must be the same as,
 *	 or binary-compatible with, the first argument type of fn().
 * * retType: OID of element type of output array.	This must be the same as,
 *	 or binary-compatible with, the result type of fn().
 * * amstate: workspace for array_map.	Must be zeroed by caller before
 *	 first call, and not touched after that.
 *
 * It is legitimate to pass a freshly-zeroed ArrayMapState on each call,
 * but better performance can be had if the state can be preserved across
 * a series of calls.
 *
 * NB: caller must assure that input array is not NULL.  NULL elements in
 * the array are OK however.
 */
Datum
array_map(FunctionCallInfo fcinfo, Oid inpType, Oid retType,
		  ArrayMapState *amstate)
{
	ArrayType  *v;
	ArrayType  *result;
	Datum	   *values;
	bool	   *nulls;
	Datum		elt;
	int		   *dim;
	int			ndim;
	int			nitems;
	int			i;
	int32		nbytes = 0;
	int32		dataoffset;
	bool		hasnulls;
	int			inp_typlen;
	bool		inp_typbyval;
	char		inp_typalign;
	int			typlen;
	bool		typbyval;
	char		typalign;
	char	   *s;
	bits8	   *bitmap;
	int			bitmask;
	ArrayMetaState *inp_extra;
	ArrayMetaState *ret_extra;

	/* Get input array */
	if (fcinfo->nargs < 1)
		elog(ERROR, "invalid nargs: %d", fcinfo->nargs);
	if (PG_ARGISNULL(0))
		elog(ERROR, "null input array");
	v = PG_GETARG_ARRAYTYPE_P(0);

	Assert(ARR_ELEMTYPE(v) == inpType);

	ndim = ARR_NDIM(v);
	dim = ARR_DIMS(v);
	nitems = ArrayGetNItems(ndim, dim);

	/* Check for empty array */
	if (nitems <= 0)
	{
		/* Return empty array */
		PG_RETURN_ARRAYTYPE_P(construct_empty_array(retType));
	}

	/*
	 * We arrange to look up info about input and return element types only
	 * once per series of calls, assuming the element type doesn't change
	 * underneath us.
	 */
	inp_extra = &amstate->inp_extra;
	ret_extra = &amstate->ret_extra;

	if (inp_extra->element_type != inpType)
	{
		get_typlenbyvalalign(inpType,
							 &inp_extra->typlen,
							 &inp_extra->typbyval,
							 &inp_extra->typalign);
		inp_extra->element_type = inpType;
	}
	inp_typlen = inp_extra->typlen;
	inp_typbyval = inp_extra->typbyval;
	inp_typalign = inp_extra->typalign;

	if (ret_extra->element_type != retType)
	{
		get_typlenbyvalalign(retType,
							 &ret_extra->typlen,
							 &ret_extra->typbyval,
							 &ret_extra->typalign);
		ret_extra->element_type = retType;
	}
	typlen = ret_extra->typlen;
	typbyval = ret_extra->typbyval;
	typalign = ret_extra->typalign;

	/* Allocate temporary arrays for new values */
	values = (Datum *) palloc(nitems * sizeof(Datum));
	nulls = (bool *) palloc(nitems * sizeof(bool));

	/* Loop over source data */
	s = ARR_DATA_PTR(v);
	bitmap = ARR_NULLBITMAP(v);
	bitmask = 1;
	hasnulls = false;

	for (i = 0; i < nitems; i++)
	{
		bool		callit = true;

		/* Get source element, checking for NULL */
		if (bitmap && (*bitmap & bitmask) == 0)
		{
			fcinfo->argnull[0] = true;
		}
		else
		{
			elt = fetch_att(s, inp_typbyval, inp_typlen);
			s = att_addlength_datum(s, inp_typlen, elt);
			s = (char *) att_align_nominal(s, inp_typalign);
			fcinfo->arg[0] = elt;
			fcinfo->argnull[0] = false;
		}

		/*
		 * Apply the given function to source elt and extra args.
		 */
		if (fcinfo->flinfo->fn_strict)
		{
			int			j;

			for (j = 0; j < fcinfo->nargs; j++)
			{
				if (fcinfo->argnull[j])
				{
					callit = false;
					break;
				}
			}
		}

		if (callit)
		{
			fcinfo->isnull = false;
			values[i] = FunctionCallInvoke(fcinfo);
		}
		else
			fcinfo->isnull = true;

		nulls[i] = fcinfo->isnull;
		if (fcinfo->isnull)
			hasnulls = true;
		else
		{
			/* Ensure data is not toasted */
			if (typlen == -1)
				values[i] = PointerGetDatum(PG_DETOAST_DATUM(values[i]));
			/* Update total result size */
			nbytes = att_addlength_datum(nbytes, typlen, values[i]);
			nbytes = att_align_nominal(nbytes, typalign);
			/* check for overflow of total request */
			if (!AllocSizeIsValid(nbytes))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("array size exceeds the maximum allowed (%d)",
								(int) MaxAllocSize)));
		}

		/* advance bitmap pointer if any */
		if (bitmap)
		{
			bitmask <<= 1;
			if (bitmask == 0x100)
			{
				bitmap++;
				bitmask = 1;
			}
		}
	}

	/* Allocate and initialize the result array */
	if (hasnulls)
	{
		dataoffset = ARR_OVERHEAD_WITHNULLS(ndim, nitems);
		nbytes += dataoffset;
	}
	else
	{
		dataoffset = 0;			/* marker for no null bitmap */
		nbytes += ARR_OVERHEAD_NONULLS(ndim);
	}
	result = (ArrayType *) palloc(nbytes);
	SET_VARSIZE(result, nbytes);
	result->ndim = ndim;
	result->dataoffset = dataoffset;
	result->elemtype = retType;
	memcpy(ARR_DIMS(result), ARR_DIMS(v), 2 * ndim * sizeof(int));

	/*
	 * Note: do not risk trying to pfree the results of the called function
	 */
	CopyArrayEls(result,
				 values, nulls, nitems,
				 typlen, typbyval, typalign,
				 false);

	pfree(values);
	pfree(nulls);

	PG_RETURN_ARRAYTYPE_P(result);
}

/*
 * construct_array	--- simple method for constructing an array object
 *
 * elems: array of Datum items to become the array contents
 *		  (NULL element values are not supported).
 * nelems: number of items
 * elmtype, elmlen, elmbyval, elmalign: info for the datatype of the items
 *
 * A palloc'd 1-D array object is constructed and returned.  Note that
 * elem values will be copied into the object even if pass-by-ref type.
 *
 * NOTE: it would be cleaner to look up the elmlen/elmbval/elmalign info
 * from the system catalogs, given the elmtype.  However, the caller is
 * in a better position to cache this info across multiple uses, or even
 * to hard-wire values if the element type is hard-wired.
 */
ArrayType *
construct_array(Datum *elems, int nelems,
				Oid elmtype,
				int elmlen, bool elmbyval, char elmalign)
{
	int			dims[1];
	int			lbs[1];

	dims[0] = nelems;
	lbs[0] = 1;

	return construct_md_array(elems, NULL, 1, dims, lbs,
							  elmtype, elmlen, elmbyval, elmalign);
}

/*
 * construct_md_array	--- simple method for constructing an array object
 *							with arbitrary dimensions and possible NULLs
 *
 * elems: array of Datum items to become the array contents
 * nulls: array of is-null flags (can be NULL if no nulls)
 * ndims: number of dimensions
 * dims: integer array with size of each dimension
 * lbs: integer array with lower bound of each dimension
 * elmtype, elmlen, elmbyval, elmalign: info for the datatype of the items
 *
 * A palloc'd ndims-D array object is constructed and returned.  Note that
 * elem values will be copied into the object even if pass-by-ref type.
 *
 * NOTE: it would be cleaner to look up the elmlen/elmbval/elmalign info
 * from the system catalogs, given the elmtype.  However, the caller is
 * in a better position to cache this info across multiple uses, or even
 * to hard-wire values if the element type is hard-wired.
 */
ArrayType *
construct_md_array(Datum *elems,
				   bool *nulls,
				   int ndims,
				   int *dims,
				   int *lbs,
				   Oid elmtype, int elmlen, bool elmbyval, char elmalign)
{
	ArrayType  *result;
	bool		hasnulls;
	int32		nbytes;
	int32		dataoffset;
	int			i;
	int			nelems;

	if (ndims < 0)				/* we do allow zero-dimension arrays */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid number of dimensions: %d", ndims)));
	if (ndims > MAXDIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
						ndims, MAXDIM)));

	/* fast track for empty array */
	if (ndims == 0)
		return construct_empty_array(elmtype);

	nelems = ArrayGetNItems(ndims, dims);

	/* compute required space */
	nbytes = 0;
	hasnulls = false;
	for (i = 0; i < nelems; i++)
	{
		if (nulls && nulls[i])
		{
			hasnulls = true;
			continue;
		}
		/* make sure data is not toasted */
		if (elmlen == -1)
			elems[i] = PointerGetDatum(PG_DETOAST_DATUM(elems[i]));
		nbytes = att_addlength_datum(nbytes, elmlen, elems[i]);
		nbytes = att_align_nominal(nbytes, elmalign);
		/* check for overflow of total request */
		if (!AllocSizeIsValid(nbytes))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("array size exceeds the maximum allowed (%d)",
							(int) MaxAllocSize)));
	}

	/* Allocate and initialize result array */
	if (hasnulls)
	{
		dataoffset = ARR_OVERHEAD_WITHNULLS(ndims, nelems);
		nbytes += dataoffset;
	}
	else
	{
		dataoffset = 0;			/* marker for no null bitmap */
		nbytes += ARR_OVERHEAD_NONULLS(ndims);
	}
	result = (ArrayType *) palloc(nbytes);
	SET_VARSIZE(result, nbytes);
	result->ndim = ndims;
	result->dataoffset = dataoffset;
	result->elemtype = elmtype;
	memcpy(ARR_DIMS(result), dims, ndims * sizeof(int));
	memcpy(ARR_LBOUND(result), lbs, ndims * sizeof(int));

	CopyArrayEls(result,
				 elems, nulls, nelems,
				 elmlen, elmbyval, elmalign,
				 false);

	return result;
}

/*
 * construct_empty_array	--- make a zero-dimensional array of given type
 */
ArrayType *
construct_empty_array(Oid elmtype)
{
	ArrayType  *result;

	result = (ArrayType *) palloc(sizeof(ArrayType));
	SET_VARSIZE(result, sizeof(ArrayType));
	result->ndim = 0;
	result->dataoffset = 0;
	result->elemtype = elmtype;
	return result;
}

/*
 * deconstruct_array  --- simple method for extracting data from an array
 *
 * array: array object to examine (must not be NULL)
 * elmtype, elmlen, elmbyval, elmalign: info for the datatype of the items
 * elemsp: return value, set to point to palloc'd array of Datum values
 * nullsp: return value, set to point to palloc'd array of isnull markers
 * nelemsp: return value, set to number of extracted values
 *
 * The caller may pass nullsp == NULL if it does not support NULLs in the
 * array.  Note that this produces a very uninformative error message,
 * so do it only in cases where a NULL is really not expected.
 *
 * If array elements are pass-by-ref data type, the returned Datums will
 * be pointers into the array object.
 *
 * NOTE: it would be cleaner to look up the elmlen/elmbval/elmalign info
 * from the system catalogs, given the elmtype.  However, in most current
 * uses the type is hard-wired into the caller and so we can save a lookup
 * cycle by hard-wiring the type info as well.
 */
void
deconstruct_array(ArrayType *array,
				  Oid elmtype,
				  int elmlen, bool elmbyval, char elmalign,
				  Datum **elemsp, bool **nullsp, int *nelemsp)
{
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;
	char	   *p;
	bits8	   *bitmap;
	int			bitmask;
	int			i;

	Assert(ARR_ELEMTYPE(array) == elmtype);

	nelems = ArrayGetNItems(ARR_NDIM(array), ARR_DIMS(array));
	*elemsp = elems = (Datum *) palloc(nelems * sizeof(Datum));
	if (nullsp)
		*nullsp = nulls = (bool *) palloc(nelems * sizeof(bool));
	else
		nulls = NULL;
	*nelemsp = nelems;

	p = ARR_DATA_PTR(array);
	bitmap = ARR_NULLBITMAP(array);
	bitmask = 1;

	for (i = 0; i < nelems; i++)
	{
		/* Get source element, checking for NULL */
		if (bitmap && (*bitmap & bitmask) == 0)
		{
			elems[i] = (Datum) 0;
			if (nulls)
				nulls[i] = true;
			else
				ereport(ERROR,
						(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				  errmsg("null array element not allowed in this context")));
		}
		else
		{
			elems[i] = fetch_att(p, elmbyval, elmlen);
			if (nulls)
				nulls[i] = false;
			p = att_addlength_pointer(p, elmlen, p);
			p = (char *) att_align_nominal(p, elmalign);
		}

		/* advance bitmap pointer if any */
		if (bitmap)
		{
			bitmask <<= 1;
			if (bitmask == 0x100)
			{
				bitmap++;
				bitmask = 1;
			}
		}
	}
}


/*
 * array_eq :
 *		  compares two arrays for equality
 * result :
 *		  returns true if the arrays are equal, false otherwise.
 *
 * Note: we do not use array_cmp here, since equality may be meaningful in
 * datatypes that don't have a total ordering (and hence no btree support).
 */
Datum
array_eq(PG_FUNCTION_ARGS)
{
	ArrayType  *array1 = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType  *array2 = PG_GETARG_ARRAYTYPE_P(1);
	int			ndims1 = ARR_NDIM(array1);
	int			ndims2 = ARR_NDIM(array2);
	int		   *dims1 = ARR_DIMS(array1);
	int		   *dims2 = ARR_DIMS(array2);
	Oid			element_type = ARR_ELEMTYPE(array1);
	bool		result = true;
	int			nitems;
	TypeCacheEntry *typentry;
	int			typlen;
	bool		typbyval;
	char		typalign;
	char	   *ptr1;
	char	   *ptr2;
	bits8	   *bitmap1;
	bits8	   *bitmap2;
	int			bitmask;
	int			i;
	FunctionCallInfoData locfcinfo;

	if (element_type != ARR_ELEMTYPE(array2))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot compare arrays of different element types")));

	/* fast path if the arrays do not have the same dimensionality */
	if (ndims1 != ndims2 ||
		memcmp(dims1, dims2, 2 * ndims1 * sizeof(int)) != 0)
		result = false;
	else
	{
		/*
		 * We arrange to look up the equality function only once per series of
		 * calls, assuming the element type doesn't change underneath us.  The
		 * typcache is used so that we have no memory leakage when being used
		 * as an index support function.
		 */
		typentry = (TypeCacheEntry *) fcinfo->flinfo->fn_extra;
		if (typentry == NULL ||
			typentry->type_id != element_type)
		{
			typentry = lookup_type_cache(element_type,
										 TYPECACHE_EQ_OPR_FINFO);
			if (!OidIsValid(typentry->eq_opr_finfo.fn_oid))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
				errmsg("could not identify an equality operator for type %s",
					   format_type_be(element_type))));
			fcinfo->flinfo->fn_extra = (void *) typentry;
		}
		typlen = typentry->typlen;
		typbyval = typentry->typbyval;
		typalign = typentry->typalign;

		/*
		 * apply the operator to each pair of array elements.
		 */
		InitFunctionCallInfoData(locfcinfo, &typentry->eq_opr_finfo, 2,
								 NULL, NULL);

		/* Loop over source data */
		nitems = ArrayGetNItems(ndims1, dims1);
		ptr1 = ARR_DATA_PTR(array1);
		ptr2 = ARR_DATA_PTR(array2);
		bitmap1 = ARR_NULLBITMAP(array1);
		bitmap2 = ARR_NULLBITMAP(array2);
		bitmask = 1;			/* use same bitmask for both arrays */

		for (i = 0; i < nitems; i++)
		{
			Datum		elt1;
			Datum		elt2;
			bool		isnull1;
			bool		isnull2;
			bool		oprresult;

			/* Get elements, checking for NULL */
			if (bitmap1 && (*bitmap1 & bitmask) == 0)
			{
				isnull1 = true;
				elt1 = (Datum) 0;
			}
			else
			{
				isnull1 = false;
				elt1 = fetch_att(ptr1, typbyval, typlen);
				ptr1 = att_addlength_pointer(ptr1, typlen, ptr1);
				ptr1 = (char *) att_align_nominal(ptr1, typalign);
			}

			if (bitmap2 && (*bitmap2 & bitmask) == 0)
			{
				isnull2 = true;
				elt2 = (Datum) 0;
			}
			else
			{
				isnull2 = false;
				elt2 = fetch_att(ptr2, typbyval, typlen);
				ptr2 = att_addlength_pointer(ptr2, typlen, ptr2);
				ptr2 = (char *) att_align_nominal(ptr2, typalign);
			}

			/* advance bitmap pointers if any */
			bitmask <<= 1;
			if (bitmask == 0x100)
			{
				if (bitmap1)
					bitmap1++;
				if (bitmap2)
					bitmap2++;
				bitmask = 1;
			}

			/*
			 * We consider two NULLs equal; NULL and not-NULL are unequal.
			 */
			if (isnull1 && isnull2)
				continue;
			if (isnull1 || isnull2)
			{
				result = false;
				break;
			}

			/*
			 * Apply the operator to the element pair
			 */
			locfcinfo.arg[0] = elt1;
			locfcinfo.arg[1] = elt2;
			locfcinfo.argnull[0] = false;
			locfcinfo.argnull[1] = false;
			locfcinfo.isnull = false;
			oprresult = DatumGetBool(FunctionCallInvoke(&locfcinfo));
			if (!oprresult)
			{
				result = false;
				break;
			}
		}
	}

	/* Avoid leaking memory when handed toasted input. */
	PG_FREE_IF_COPY(array1, 0);
	PG_FREE_IF_COPY(array2, 1);

	PG_RETURN_BOOL(result);
}


/*-----------------------------------------------------------------------------
 * array-array bool operators:
 *		Given two arrays, iterate comparison operators
 *		over the array. Uses logic similar to text comparison
 *		functions, except element-by-element instead of
 *		character-by-character.
 *----------------------------------------------------------------------------
 */

Datum
array_ne(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(!DatumGetBool(array_eq(fcinfo)));
}

Datum
array_lt(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(array_cmp(fcinfo) < 0);
}

Datum
array_gt(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(array_cmp(fcinfo) > 0);
}

Datum
array_le(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(array_cmp(fcinfo) <= 0);
}

Datum
array_ge(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(array_cmp(fcinfo) >= 0);
}

Datum
btarraycmp(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(array_cmp(fcinfo));
}

/*
 * array_cmp()
 * Internal comparison function for arrays.
 *
 * Returns -1, 0 or 1
 */
static int
array_cmp(FunctionCallInfo fcinfo)
{
	ArrayType  *array1 = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType  *array2 = PG_GETARG_ARRAYTYPE_P(1);
	int			ndims1 = ARR_NDIM(array1);
	int			ndims2 = ARR_NDIM(array2);
	int		   *dims1 = ARR_DIMS(array1);
	int		   *dims2 = ARR_DIMS(array2);
	int			nitems1 = ArrayGetNItems(ndims1, dims1);
	int			nitems2 = ArrayGetNItems(ndims2, dims2);
	Oid			element_type = ARR_ELEMTYPE(array1);
	int			result = 0;
	TypeCacheEntry *typentry;
	int			typlen;
	bool		typbyval;
	char		typalign;
	int			min_nitems;
	char	   *ptr1;
	char	   *ptr2;
	bits8	   *bitmap1;
	bits8	   *bitmap2;
	int			bitmask;
	int			i;
	FunctionCallInfoData locfcinfo;

	if (element_type != ARR_ELEMTYPE(array2))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot compare arrays of different element types")));

	/*
	 * We arrange to look up the comparison function only once per series of
	 * calls, assuming the element type doesn't change underneath us. The
	 * typcache is used so that we have no memory leakage when being used as
	 * an index support function.
	 */
	typentry = (TypeCacheEntry *) fcinfo->flinfo->fn_extra;
	if (typentry == NULL ||
		typentry->type_id != element_type)
	{
		typentry = lookup_type_cache(element_type,
									 TYPECACHE_CMP_PROC_FINFO);
		if (!OidIsValid(typentry->cmp_proc_finfo.fn_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
			   errmsg("could not identify a comparison function for type %s",
					  format_type_be(element_type))));
		fcinfo->flinfo->fn_extra = (void *) typentry;
	}
	typlen = typentry->typlen;
	typbyval = typentry->typbyval;
	typalign = typentry->typalign;

	/*
	 * apply the operator to each pair of array elements.
	 */
	InitFunctionCallInfoData(locfcinfo, &typentry->cmp_proc_finfo, 2,
							 NULL, NULL);

	/* Loop over source data */
	min_nitems = Min(nitems1, nitems2);
	ptr1 = ARR_DATA_PTR(array1);
	ptr2 = ARR_DATA_PTR(array2);
	bitmap1 = ARR_NULLBITMAP(array1);
	bitmap2 = ARR_NULLBITMAP(array2);
	bitmask = 1;				/* use same bitmask for both arrays */

	for (i = 0; i < min_nitems; i++)
	{
		Datum		elt1;
		Datum		elt2;
		bool		isnull1;
		bool		isnull2;
		int32		cmpresult;

		/* Get elements, checking for NULL */
		if (bitmap1 && (*bitmap1 & bitmask) == 0)
		{
			isnull1 = true;
			elt1 = (Datum) 0;
		}
		else
		{
			isnull1 = false;
			elt1 = fetch_att(ptr1, typbyval, typlen);
			ptr1 = att_addlength_pointer(ptr1, typlen, ptr1);
			ptr1 = (char *) att_align_nominal(ptr1, typalign);
		}

		if (bitmap2 && (*bitmap2 & bitmask) == 0)
		{
			isnull2 = true;
			elt2 = (Datum) 0;
		}
		else
		{
			isnull2 = false;
			elt2 = fetch_att(ptr2, typbyval, typlen);
			ptr2 = att_addlength_pointer(ptr2, typlen, ptr2);
			ptr2 = (char *) att_align_nominal(ptr2, typalign);
		}

		/* advance bitmap pointers if any */
		bitmask <<= 1;
		if (bitmask == 0x100)
		{
			if (bitmap1)
				bitmap1++;
			if (bitmap2)
				bitmap2++;
			bitmask = 1;
		}

		/*
		 * We consider two NULLs equal; NULL > not-NULL.
		 */
		if (isnull1 && isnull2)
			continue;
		if (isnull1)
		{
			/* arg1 is greater than arg2 */
			result = 1;
			break;
		}
		if (isnull2)
		{
			/* arg1 is less than arg2 */
			result = -1;
			break;
		}

		/* Compare the pair of elements */
		locfcinfo.arg[0] = elt1;
		locfcinfo.arg[1] = elt2;
		locfcinfo.argnull[0] = false;
		locfcinfo.argnull[1] = false;
		locfcinfo.isnull = false;
		cmpresult = DatumGetInt32(FunctionCallInvoke(&locfcinfo));

		if (cmpresult == 0)
			continue;			/* equal */

		if (cmpresult < 0)
		{
			/* arg1 is less than arg2 */
			result = -1;
			break;
		}
		else
		{
			/* arg1 is greater than arg2 */
			result = 1;
			break;
		}
	}

	/*
	 * If arrays contain same data (up to end of shorter one), apply
	 * additional rules to sort by dimensionality.	The relative significance
	 * of the different bits of information is historical; mainly we just care
	 * that we don't say "equal" for arrays of different dimensionality.
	 */
	if (result == 0)
	{
		if (nitems1 != nitems2)
			result = (nitems1 < nitems2) ? -1 : 1;
		else if (ndims1 != ndims2)
			result = (ndims1 < ndims2) ? -1 : 1;
		else
		{
			/* this relies on LB array immediately following DIMS array */
			for (i = 0; i < ndims1 * 2; i++)
			{
				if (dims1[i] != dims2[i])
				{
					result = (dims1[i] < dims2[i]) ? -1 : 1;
					break;
				}
			}
		}
	}

	/* Avoid leaking memory when handed toasted input. */
	PG_FREE_IF_COPY(array1, 0);
	PG_FREE_IF_COPY(array2, 1);

	return result;
}


/*-----------------------------------------------------------------------------
 * array overlap/containment comparisons
 *		These use the same methods of comparing array elements as array_eq.
 *		We consider only the elements of the arrays, ignoring dimensionality.
 *----------------------------------------------------------------------------
 */

/*
 * array_contain_compare :
 *		  compares two arrays for overlap/containment
 *
 * When matchall is true, return true if all members of array1 are in array2.
 * When matchall is false, return true if any members of array1 are in array2.
 */
static bool
array_contain_compare(ArrayType *array1, ArrayType *array2, bool matchall,
					  void **fn_extra)
{
	bool		result = matchall;
	Oid			element_type = ARR_ELEMTYPE(array1);
	TypeCacheEntry *typentry;
	int			nelems1;
	Datum	   *values2;
	bool	   *nulls2;
	int			nelems2;
	int			typlen;
	bool		typbyval;
	char		typalign;
	char	   *ptr1;
	bits8	   *bitmap1;
	int			bitmask;
	int			i;
	int			j;
	FunctionCallInfoData locfcinfo;

	if (element_type != ARR_ELEMTYPE(array2))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot compare arrays of different element types")));

	/*
	 * We arrange to look up the equality function only once per series of
	 * calls, assuming the element type doesn't change underneath us.  The
	 * typcache is used so that we have no memory leakage when being used as
	 * an index support function.
	 */
	typentry = (TypeCacheEntry *) *fn_extra;
	if (typentry == NULL ||
		typentry->type_id != element_type)
	{
		typentry = lookup_type_cache(element_type,
									 TYPECACHE_EQ_OPR_FINFO);
		if (!OidIsValid(typentry->eq_opr_finfo.fn_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
				errmsg("could not identify an equality operator for type %s",
					   format_type_be(element_type))));
		*fn_extra = (void *) typentry;
	}
	typlen = typentry->typlen;
	typbyval = typentry->typbyval;
	typalign = typentry->typalign;

	/*
	 * Since we probably will need to scan array2 multiple times, it's
	 * worthwhile to use deconstruct_array on it.  We scan array1 the hard way
	 * however, since we very likely won't need to look at all of it.
	 */
	deconstruct_array(array2, element_type, typlen, typbyval, typalign,
					  &values2, &nulls2, &nelems2);

	/*
	 * Apply the comparison operator to each pair of array elements.
	 */
	InitFunctionCallInfoData(locfcinfo, &typentry->eq_opr_finfo, 2,
							 NULL, NULL);

	/* Loop over source data */
	nelems1 = ArrayGetNItems(ARR_NDIM(array1), ARR_DIMS(array1));
	ptr1 = ARR_DATA_PTR(array1);
	bitmap1 = ARR_NULLBITMAP(array1);
	bitmask = 1;

	for (i = 0; i < nelems1; i++)
	{
		Datum		elt1;
		bool		isnull1;

		/* Get element, checking for NULL */
		if (bitmap1 && (*bitmap1 & bitmask) == 0)
		{
			isnull1 = true;
			elt1 = (Datum) 0;
		}
		else
		{
			isnull1 = false;
			elt1 = fetch_att(ptr1, typbyval, typlen);
			ptr1 = att_addlength_pointer(ptr1, typlen, ptr1);
			ptr1 = (char *) att_align_nominal(ptr1, typalign);
		}

		/* advance bitmap pointer if any */
		bitmask <<= 1;
		if (bitmask == 0x100)
		{
			if (bitmap1)
				bitmap1++;
			bitmask = 1;
		}

		/*
		 * We assume that the comparison operator is strict, so a NULL can't
		 * match anything.	XXX this diverges from the "NULL=NULL" behavior of
		 * array_eq, should we act like that?
		 */
		if (isnull1)
		{
			if (matchall)
			{
				result = false;
				break;
			}
			continue;
		}

		for (j = 0; j < nelems2; j++)
		{
			Datum		elt2 = values2[j];
			bool		isnull2 = nulls2[j];
			bool		oprresult;

			if (isnull2)
				continue;		/* can't match */

			/*
			 * Apply the operator to the element pair
			 */
			locfcinfo.arg[0] = elt1;
			locfcinfo.arg[1] = elt2;
			locfcinfo.argnull[0] = false;
			locfcinfo.argnull[1] = false;
			locfcinfo.isnull = false;
			oprresult = DatumGetBool(FunctionCallInvoke(&locfcinfo));
			if (oprresult)
				break;
		}

		if (j < nelems2)
		{
			/* found a match for elt1 */
			if (!matchall)
			{
				result = true;
				break;
			}
		}
		else
		{
			/* no match for elt1 */
			if (matchall)
			{
				result = false;
				break;
			}
		}
	}

	pfree(values2);
	pfree(nulls2);

	return result;
}

Datum
arrayoverlap(PG_FUNCTION_ARGS)
{
	ArrayType  *array1 = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType  *array2 = PG_GETARG_ARRAYTYPE_P(1);
	bool		result;

	result = array_contain_compare(array1, array2, false,
								   &fcinfo->flinfo->fn_extra);

	/* Avoid leaking memory when handed toasted input. */
	PG_FREE_IF_COPY(array1, 0);
	PG_FREE_IF_COPY(array2, 1);

	PG_RETURN_BOOL(result);
}

Datum
arraycontains(PG_FUNCTION_ARGS)
{
	ArrayType  *array1 = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType  *array2 = PG_GETARG_ARRAYTYPE_P(1);
	bool		result;

	result = array_contain_compare(array2, array1, true,
								   &fcinfo->flinfo->fn_extra);

	/* Avoid leaking memory when handed toasted input. */
	PG_FREE_IF_COPY(array1, 0);
	PG_FREE_IF_COPY(array2, 1);

	PG_RETURN_BOOL(result);
}

Datum
arraycontained(PG_FUNCTION_ARGS)
{
	ArrayType  *array1 = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType  *array2 = PG_GETARG_ARRAYTYPE_P(1);
	bool		result;

	result = array_contain_compare(array1, array2, true,
								   &fcinfo->flinfo->fn_extra);

	/* Avoid leaking memory when handed toasted input. */
	PG_FREE_IF_COPY(array1, 0);
	PG_FREE_IF_COPY(array2, 1);

	PG_RETURN_BOOL(result);
}


/***************************************************************************/
/******************|		  Support  Routines			  |*****************/
/***************************************************************************/

/*
 * Check whether a specific array element is NULL
 *
 * nullbitmap: pointer to array's null bitmap (NULL if none)
 * offset: 0-based linear element number of array element
 */
static bool
array_get_isnull(const bits8 *nullbitmap, int offset)
{
	if (nullbitmap == NULL)
		return false;			/* assume not null */
	if (nullbitmap[offset / 8] & (1 << (offset % 8)))
		return false;			/* not null */
	return true;
}

/*
 * Set a specific array element's null-bitmap entry
 *
 * nullbitmap: pointer to array's null bitmap (mustn't be NULL)
 * offset: 0-based linear element number of array element
 * isNull: null status to set
 */
static void
array_set_isnull(bits8 *nullbitmap, int offset, bool isNull)
{
	int			bitmask;

	nullbitmap += offset / 8;
	bitmask = 1 << (offset % 8);
	if (isNull)
		*nullbitmap &= ~bitmask;
	else
		*nullbitmap |= bitmask;
}

/*
 * Fetch array element at pointer, converted correctly to a Datum
 *
 * Caller must have handled case of NULL element
 */
static Datum
ArrayCast(char *value, bool byval, int len)
{
	return fetch_att(value, byval, len);
}

/*
 * Copy datum to *dest and return total space used (including align padding)
 *
 * Caller must have handled case of NULL element
 */
static int
ArrayCastAndSet(Datum src,
				int typlen,
				bool typbyval,
				char typalign,
				char *dest)
{
	int			inc;

	if (typlen > 0)
	{
		if (typbyval)
			store_att_byval(dest, src, typlen);
		else
			memmove(dest, DatumGetPointer(src), typlen);
		inc = att_align_nominal(typlen, typalign);
	}
	else
	{
		Assert(!typbyval);
		inc = att_addlength_datum(0, typlen, src);
		memmove(dest, DatumGetPointer(src), inc);
		inc = att_align_nominal(inc, typalign);
	}

	return inc;
}

/*
 * Advance ptr over nitems array elements
 *
 * ptr: starting location in array
 * offset: 0-based linear element number of first element (the one at *ptr)
 * nullbitmap: start of array's null bitmap, or NULL if none
 * nitems: number of array elements to advance over (>= 0)
 * typlen, typbyval, typalign: storage parameters of array element datatype
 *
 * It is caller's responsibility to ensure that nitems is within range
 */
static char *
array_seek(char *ptr, int offset, bits8 *nullbitmap, int nitems,
		   int typlen, bool typbyval, char typalign)
{
	int			bitmask;
	int			i;

	/* easy if fixed-size elements and no NULLs */
	if (typlen > 0 && !nullbitmap)
		return ptr + nitems * ((Size) att_align_nominal(typlen, typalign));

	/* seems worth having separate loops for NULL and no-NULLs cases */
	if (nullbitmap)
	{
		nullbitmap += offset / 8;
		bitmask = 1 << (offset % 8);

		for (i = 0; i < nitems; i++)
		{
			if (*nullbitmap & bitmask)
			{
				ptr = att_addlength_pointer(ptr, typlen, ptr);
				ptr = (char *) att_align_nominal(ptr, typalign);
			}
			bitmask <<= 1;
			if (bitmask == 0x100)
			{
				nullbitmap++;
				bitmask = 1;
			}
		}
	}
	else
	{
		for (i = 0; i < nitems; i++)
		{
			ptr = att_addlength_pointer(ptr, typlen, ptr);
			ptr = (char *) att_align_nominal(ptr, typalign);
		}
	}
	return ptr;
}

/*
 * Compute total size of the nitems array elements starting at *ptr
 *
 * Parameters same as for array_seek
 */
static int
array_nelems_size(char *ptr, int offset, bits8 *nullbitmap, int nitems,
				  int typlen, bool typbyval, char typalign)
{
	return array_seek(ptr, offset, nullbitmap, nitems,
					  typlen, typbyval, typalign) - ptr;
}

/*
 * Copy nitems array elements from srcptr to destptr
 *
 * destptr: starting destination location (must be enough room!)
 * nitems: number of array elements to copy (>= 0)
 * srcptr: starting location in source array
 * offset: 0-based linear element number of first element (the one at *srcptr)
 * nullbitmap: start of source array's null bitmap, or NULL if none
 * typlen, typbyval, typalign: storage parameters of array element datatype
 *
 * Returns number of bytes copied
 *
 * NB: this does not take care of setting up the destination's null bitmap!
 */
static int
array_copy(char *destptr, int nitems,
		   char *srcptr, int offset, bits8 *nullbitmap,
		   int typlen, bool typbyval, char typalign)
{
	int			numbytes;

	numbytes = array_nelems_size(srcptr, offset, nullbitmap, nitems,
								 typlen, typbyval, typalign);
	memcpy(destptr, srcptr, numbytes);
	return numbytes;
}

/*
 * Copy nitems null-bitmap bits from source to destination
 *
 * destbitmap: start of destination array's null bitmap (mustn't be NULL)
 * destoffset: 0-based linear element number of first dest element
 * srcbitmap: start of source array's null bitmap, or NULL if none
 * srcoffset: 0-based linear element number of first source element
 * nitems: number of bits to copy (>= 0)
 *
 * If srcbitmap is NULL then we assume the source is all-non-NULL and
 * fill 1's into the destination bitmap.  Note that only the specified
 * bits in the destination map are changed, not any before or after.
 *
 * Note: this could certainly be optimized using standard bitblt methods.
 * However, it's not clear that the typical Postgres array has enough elements
 * to make it worth worrying too much.	For the moment, KISS.
 */
void
array_bitmap_copy(bits8 *destbitmap, int destoffset,
				  const bits8 *srcbitmap, int srcoffset,
				  int nitems)
{
	int			destbitmask,
				destbitval,
				srcbitmask,
				srcbitval;

	Assert(destbitmap);
	if (nitems <= 0)
		return;					/* don't risk fetch off end of memory */
	destbitmap += destoffset / 8;
	destbitmask = 1 << (destoffset % 8);
	destbitval = *destbitmap;
	if (srcbitmap)
	{
		srcbitmap += srcoffset / 8;
		srcbitmask = 1 << (srcoffset % 8);
		srcbitval = *srcbitmap;
		while (nitems-- > 0)
		{
			if (srcbitval & srcbitmask)
				destbitval |= destbitmask;
			else
				destbitval &= ~destbitmask;
			destbitmask <<= 1;
			if (destbitmask == 0x100)
			{
				*destbitmap++ = destbitval;
				destbitmask = 1;
				if (nitems > 0)
					destbitval = *destbitmap;
			}
			srcbitmask <<= 1;
			if (srcbitmask == 0x100)
			{
				srcbitmap++;
				srcbitmask = 1;
				if (nitems > 0)
					srcbitval = *srcbitmap;
			}
		}
		if (destbitmask != 1)
			*destbitmap = destbitval;
	}
	else
	{
		while (nitems-- > 0)
		{
			destbitval |= destbitmask;
			destbitmask <<= 1;
			if (destbitmask == 0x100)
			{
				*destbitmap++ = destbitval;
				destbitmask = 1;
				if (nitems > 0)
					destbitval = *destbitmap;
			}
		}
		if (destbitmask != 1)
			*destbitmap = destbitval;
	}
}

/*
 * Compute space needed for a slice of an array
 *
 * We assume the caller has verified that the slice coordinates are valid.
 */
static int
array_slice_size(char *arraydataptr, bits8 *arraynullsptr,
				 int ndim, int *dim, int *lb,
				 int *st, int *endp,
				 int typlen, bool typbyval, char typalign)
{
	int			src_offset,
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

	/* Pretty easy for fixed element length without nulls ... */
	if (typlen > 0 && !arraynullsptr)
		return ArrayGetNItems(ndim, span) * att_align_nominal(typlen, typalign);

	/* Else gotta do it the hard way */
	src_offset = ArrayGetOffset(ndim, dim, lb, st);
	ptr = array_seek(arraydataptr, 0, arraynullsptr, src_offset,
					 typlen, typbyval, typalign);
	mda_get_prod(ndim, dim, prod);
	mda_get_offset_values(ndim, dist, prod, span);
	for (i = 0; i < ndim; i++)
		indx[i] = 0;
	j = ndim - 1;
	do
	{
		if (dist[j])
		{
			ptr = array_seek(ptr, src_offset, arraynullsptr, dist[j],
							 typlen, typbyval, typalign);
			src_offset += dist[j];
		}
		if (!array_get_isnull(arraynullsptr, src_offset))
		{
			inc = att_addlength_pointer(0, typlen, ptr);
			inc = att_align_nominal(inc, typalign);
			ptr += inc;
			count += inc;
		}
		src_offset++;
	} while ((j = mda_next_tuple(ndim, indx, span)) != -1);
	return count;
}

/*
 * Extract a slice of an array into consecutive elements in the destination
 * array.
 *
 * We assume the caller has verified that the slice coordinates are valid,
 * allocated enough storage for the result, and initialized the header
 * of the new array.
 */
static void
array_extract_slice(ArrayType *newarray,
					int ndim,
					int *dim,
					int *lb,
					char *arraydataptr,
					bits8 *arraynullsptr,
					int *st,
					int *endp,
					int typlen,
					bool typbyval,
					char typalign)
{
	char	   *destdataptr = ARR_DATA_PTR(newarray);
	bits8	   *destnullsptr = ARR_NULLBITMAP(newarray);
	char	   *srcdataptr;
	int			src_offset,
				dest_offset,
				prod[MAXDIM],
				span[MAXDIM],
				dist[MAXDIM],
				indx[MAXDIM];
	int			i,
				j,
				inc;

	src_offset = ArrayGetOffset(ndim, dim, lb, st);
	srcdataptr = array_seek(arraydataptr, 0, arraynullsptr, src_offset,
							typlen, typbyval, typalign);
	mda_get_prod(ndim, dim, prod);
	mda_get_range(ndim, span, st, endp);
	mda_get_offset_values(ndim, dist, prod, span);
	for (i = 0; i < ndim; i++)
		indx[i] = 0;
	dest_offset = 0;
	j = ndim - 1;
	do
	{
		if (dist[j])
		{
			/* skip unwanted elements */
			srcdataptr = array_seek(srcdataptr, src_offset, arraynullsptr,
									dist[j],
									typlen, typbyval, typalign);
			src_offset += dist[j];
		}
		inc = array_copy(destdataptr, 1,
						 srcdataptr, src_offset, arraynullsptr,
						 typlen, typbyval, typalign);
		if (destnullsptr)
			array_bitmap_copy(destnullsptr, dest_offset,
							  arraynullsptr, src_offset,
							  1);
		destdataptr += inc;
		srcdataptr += inc;
		src_offset++;
		dest_offset++;
	} while ((j = mda_next_tuple(ndim, indx, span)) != -1);
}

/*
 * Insert a slice into an array.
 *
 * ndim/dim[]/lb[] are dimensions of the original array.  A new array with
 * those same dimensions is to be constructed.	destArray must already
 * have been allocated and its header initialized.
 *
 * st[]/endp[] identify the slice to be replaced.  Elements within the slice
 * volume are taken from consecutive elements of the srcArray; elements
 * outside it are copied from origArray.
 *
 * We assume the caller has verified that the slice coordinates are valid.
 */
static void
array_insert_slice(ArrayType *destArray,
				   ArrayType *origArray,
				   ArrayType *srcArray,
				   int ndim,
				   int *dim,
				   int *lb,
				   int *st,
				   int *endp,
				   int typlen,
				   bool typbyval,
				   char typalign)
{
	char	   *destPtr = ARR_DATA_PTR(destArray);
	char	   *origPtr = ARR_DATA_PTR(origArray);
	char	   *srcPtr = ARR_DATA_PTR(srcArray);
	bits8	   *destBitmap = ARR_NULLBITMAP(destArray);
	bits8	   *origBitmap = ARR_NULLBITMAP(origArray);
	bits8	   *srcBitmap = ARR_NULLBITMAP(srcArray);
	int			orignitems = ArrayGetNItems(ARR_NDIM(origArray),
											ARR_DIMS(origArray));
	int			dest_offset,
				orig_offset,
				src_offset,
				prod[MAXDIM],
				span[MAXDIM],
				dist[MAXDIM],
				indx[MAXDIM];
	int			i,
				j,
				inc;

	dest_offset = ArrayGetOffset(ndim, dim, lb, st);
	/* copy items before the slice start */
	inc = array_copy(destPtr, dest_offset,
					 origPtr, 0, origBitmap,
					 typlen, typbyval, typalign);
	destPtr += inc;
	origPtr += inc;
	if (destBitmap)
		array_bitmap_copy(destBitmap, 0, origBitmap, 0, dest_offset);
	orig_offset = dest_offset;
	mda_get_prod(ndim, dim, prod);
	mda_get_range(ndim, span, st, endp);
	mda_get_offset_values(ndim, dist, prod, span);
	for (i = 0; i < ndim; i++)
		indx[i] = 0;
	src_offset = 0;
	j = ndim - 1;
	do
	{
		/* Copy/advance over elements between here and next part of slice */
		if (dist[j])
		{
			inc = array_copy(destPtr, dist[j],
							 origPtr, orig_offset, origBitmap,
							 typlen, typbyval, typalign);
			destPtr += inc;
			origPtr += inc;
			if (destBitmap)
				array_bitmap_copy(destBitmap, dest_offset,
								  origBitmap, orig_offset,
								  dist[j]);
			dest_offset += dist[j];
			orig_offset += dist[j];
		}
		/* Copy new element at this slice position */
		inc = array_copy(destPtr, 1,
						 srcPtr, src_offset, srcBitmap,
						 typlen, typbyval, typalign);
		if (destBitmap)
			array_bitmap_copy(destBitmap, dest_offset,
							  srcBitmap, src_offset,
							  1);
		destPtr += inc;
		srcPtr += inc;
		dest_offset++;
		src_offset++;
		/* Advance over old element at this slice position */
		origPtr = array_seek(origPtr, orig_offset, origBitmap, 1,
							 typlen, typbyval, typalign);
		orig_offset++;
	} while ((j = mda_next_tuple(ndim, indx, span)) != -1);

	/* don't miss any data at the end */
	array_copy(destPtr, orignitems - orig_offset,
			   origPtr, orig_offset, origBitmap,
			   typlen, typbyval, typalign);
	if (destBitmap)
		array_bitmap_copy(destBitmap, dest_offset,
						  origBitmap, orig_offset,
						  orignitems - orig_offset);
}

/*
 * accumArrayResult - accumulate one (more) Datum for an array result
 *
 *	astate is working state (NULL on first call)
 *	rcontext is where to keep working state
 */
ArrayBuildState *
accumArrayResult(ArrayBuildState *astate,
				 Datum dvalue, bool disnull,
				 Oid element_type,
				 MemoryContext rcontext)
{
	MemoryContext arr_context,
				oldcontext;

	if (astate == NULL)
	{
		/* First time through --- initialize */

		/* Make a temporary context to hold all the junk */
		arr_context = AllocSetContextCreate(rcontext,
											"accumArrayResult",
											ALLOCSET_DEFAULT_MINSIZE,
											ALLOCSET_DEFAULT_INITSIZE,
											ALLOCSET_DEFAULT_MAXSIZE);
		oldcontext = MemoryContextSwitchTo(arr_context);
		astate = (ArrayBuildState *) palloc(sizeof(ArrayBuildState));
		astate->mcontext = arr_context;
		astate->alen = 64;		/* arbitrary starting array size */
		astate->dvalues = (Datum *) palloc(astate->alen * sizeof(Datum));
		astate->dnulls = (bool *) palloc(astate->alen * sizeof(bool));
		astate->nelems = 0;
		astate->element_type = element_type;
		get_typlenbyvalalign(element_type,
							 &astate->typlen,
							 &astate->typbyval,
							 &astate->typalign);
	}
	else
	{
		oldcontext = MemoryContextSwitchTo(astate->mcontext);
		Assert(astate->element_type == element_type);
		/* enlarge dvalues[]/dnulls[] if needed */
		if (astate->nelems >= astate->alen)
		{
			astate->alen *= 2;
			astate->dvalues = (Datum *)
				repalloc(astate->dvalues, astate->alen * sizeof(Datum));
			astate->dnulls = (bool *)
				repalloc(astate->dnulls, astate->alen * sizeof(bool));
		}
	}

	/*
	 * Ensure pass-by-ref stuff is copied into mcontext; and detoast it too
	 * if it's varlena.  (You might think that detoasting is not needed here
	 * because construct_md_array can detoast the array elements later.
	 * However, we must not let construct_md_array modify the ArrayBuildState
	 * because that would mean array_agg_finalfn damages its input, which
	 * is verboten.  Also, this way frequently saves one copying step.)
	 */
	if (!disnull && !astate->typbyval)
	{
		if (astate->typlen == -1)
			dvalue = PointerGetDatum(PG_DETOAST_DATUM_COPY(dvalue));
		else
			dvalue = datumCopy(dvalue, astate->typbyval, astate->typlen);
	}

	astate->dvalues[astate->nelems] = dvalue;
	astate->dnulls[astate->nelems] = disnull;
	astate->nelems++;

	MemoryContextSwitchTo(oldcontext);

	return astate;
}

/*
 * makeArrayResult - produce 1-D final result of accumArrayResult
 *
 *	astate is working state (not NULL)
 *	rcontext is where to construct result
 */
Datum
makeArrayResult(ArrayBuildState *astate,
				MemoryContext rcontext)
{
	int			dims[1];
	int			lbs[1];

	dims[0] = astate->nelems;
	lbs[0] = 1;

	return makeMdArrayResult(astate, 1, dims, lbs, rcontext, true);
}

/*
 * makeMdArrayResult - produce multi-D final result of accumArrayResult
 *
 * beware: no check that specified dimensions match the number of values
 * accumulated.
 *
 *	astate is working state (not NULL)
 *	rcontext is where to construct result
 *	release is true if okay to release working state
 */
Datum
makeMdArrayResult(ArrayBuildState *astate,
				  int ndims,
				  int *dims,
				  int *lbs,
				  MemoryContext rcontext,
				  bool release)
{
	ArrayType  *result;
	MemoryContext oldcontext;

	/* Build the final array result in rcontext */
	oldcontext = MemoryContextSwitchTo(rcontext);

	result = construct_md_array(astate->dvalues,
								astate->dnulls,
								ndims,
								dims,
								lbs,
								astate->element_type,
								astate->typlen,
								astate->typbyval,
								astate->typalign);

	MemoryContextSwitchTo(oldcontext);

	/* Clean up all the junk */
	if (release)
		MemoryContextDelete(astate->mcontext);

	return PointerGetDatum(result);
}

Datum
array_larger(PG_FUNCTION_ARGS)
{
	ArrayType  *v1,
			   *v2,
			   *result;

	v1 = PG_GETARG_ARRAYTYPE_P(0);
	v2 = PG_GETARG_ARRAYTYPE_P(1);

	result = ((array_cmp(fcinfo) > 0) ? v1 : v2);

	PG_RETURN_ARRAYTYPE_P(result);
}

Datum
array_smaller(PG_FUNCTION_ARGS)
{
	ArrayType  *v1,
			   *v2,
			   *result;

	v1 = PG_GETARG_ARRAYTYPE_P(0);
	v2 = PG_GETARG_ARRAYTYPE_P(1);

	result = ((array_cmp(fcinfo) < 0) ? v1 : v2);

	PG_RETURN_ARRAYTYPE_P(result);
}


typedef struct generate_subscripts_fctx
{
	int4		lower;
	int4		upper;
	bool		reverse;
} generate_subscripts_fctx;

/*
 * generate_subscripts(array anyarray, dim int [, reverse bool])
 *		Returns all subscripts of the array for any dimension
 */
Datum
generate_subscripts(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	MemoryContext oldcontext;
	generate_subscripts_fctx *fctx;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		ArrayType  *v = PG_GETARG_ARRAYTYPE_P(0);
		int			reqdim = PG_GETARG_INT32(1);
		int		   *lb,
				   *dimv;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/* Sanity check: does it look like an array at all? */
		if (ARR_NDIM(v) <= 0 || ARR_NDIM(v) > MAXDIM)
			SRF_RETURN_DONE(funcctx);

		/* Sanity check: was the requested dim valid */
		if (reqdim <= 0 || reqdim > ARR_NDIM(v))
			SRF_RETURN_DONE(funcctx);

		/*
		 * switch to memory context appropriate for multiple function calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		fctx = (generate_subscripts_fctx *) palloc(sizeof(generate_subscripts_fctx));

		lb = ARR_LBOUND(v);
		dimv = ARR_DIMS(v);

		fctx->lower = lb[reqdim - 1];
		fctx->upper = dimv[reqdim - 1] + lb[reqdim - 1] - 1;
		fctx->reverse = (PG_NARGS() < 3) ? false : PG_GETARG_BOOL(2);

		funcctx->user_fctx = fctx;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();

	fctx = funcctx->user_fctx;

	if (fctx->lower <= fctx->upper)
	{
		if (!fctx->reverse)
			SRF_RETURN_NEXT(funcctx, Int32GetDatum(fctx->lower++));
		else
			SRF_RETURN_NEXT(funcctx, Int32GetDatum(fctx->upper--));
	}
	else
		/* done when there are no more elements left */
		SRF_RETURN_DONE(funcctx);
}

/*
 * generate_subscripts_nodir
 *		Implements the 2-argument version of generate_subscripts
 */
Datum
generate_subscripts_nodir(PG_FUNCTION_ARGS)
{
	/* just call the other one -- it can handle both cases */
	return generate_subscripts(fcinfo);
}

/*
 * array_fill_with_lower_bounds
 *		Create and fill array with defined lower bounds.
 */
Datum
array_fill_with_lower_bounds(PG_FUNCTION_ARGS)
{
	ArrayType  *dims;
	ArrayType  *lbs;
	ArrayType  *result;
	Oid			elmtype;
	Datum		value;
	bool		isnull;

	if (PG_ARGISNULL(1) || PG_ARGISNULL(2))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
			   errmsg("dimension array or low bound array cannot be NULL")));

	dims = PG_GETARG_ARRAYTYPE_P(1);
	lbs = PG_GETARG_ARRAYTYPE_P(2);

	if (!PG_ARGISNULL(0))
	{
		value = PG_GETARG_DATUM(0);
		isnull = false;
	}
	else
	{
		value = 0;
		isnull = true;
	}

	elmtype = get_fn_expr_argtype(fcinfo->flinfo, 0);
	if (!OidIsValid(elmtype))
		elog(ERROR, "could not determine data type of input");

	result = array_fill_internal(dims, lbs, value, isnull, elmtype, fcinfo);
	PG_RETURN_ARRAYTYPE_P(result);
}

/*
 * array_fill
 *		Create and fill array with default lower bounds.
 */
Datum
array_fill(PG_FUNCTION_ARGS)
{
	ArrayType  *dims;
	ArrayType  *result;
	Oid			elmtype;
	Datum		value;
	bool		isnull;

	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
			   errmsg("dimension array or low bound array cannot be NULL")));

	dims = PG_GETARG_ARRAYTYPE_P(1);

	if (!PG_ARGISNULL(0))
	{
		value = PG_GETARG_DATUM(0);
		isnull = false;
	}
	else
	{
		value = 0;
		isnull = true;
	}

	elmtype = get_fn_expr_argtype(fcinfo->flinfo, 0);
	if (!OidIsValid(elmtype))
		elog(ERROR, "could not determine data type of input");

	result = array_fill_internal(dims, NULL, value, isnull, elmtype, fcinfo);
	PG_RETURN_ARRAYTYPE_P(result);
}

static ArrayType *
create_array_envelope(int ndims, int *dimv, int *lbsv, int nbytes,
					  Oid elmtype, int dataoffset)
{
	ArrayType  *result;

	result = (ArrayType *) palloc0(nbytes);
	SET_VARSIZE(result, nbytes);
	result->ndim = ndims;
	result->dataoffset = dataoffset;
	result->elemtype = elmtype;
	memcpy(ARR_DIMS(result), dimv, ndims * sizeof(int));
	memcpy(ARR_LBOUND(result), lbsv, ndims * sizeof(int));

	return result;
}

static ArrayType *
array_fill_internal(ArrayType *dims, ArrayType *lbs,
					Datum value, bool isnull, Oid elmtype,
					FunctionCallInfo fcinfo)
{
	ArrayType  *result;
	int		   *dimv;
	int		   *lbsv;
	int			ndims;
	int			nitems;
	int			deflbs[MAXDIM];
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	ArrayMetaState *my_extra;

	/*
	 * Params checks
	 */
	if (ARR_NDIM(dims) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("wrong number of array subscripts"),
				 errdetail("Dimension array must be one dimensional.")));

	if (ARR_LBOUND(dims)[0] != 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("wrong range of array subscripts"),
				 errdetail("Lower bound of dimension array must be one.")));

	if (ARR_HASNULL(dims))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("dimension values cannot be null")));

	dimv = (int *) ARR_DATA_PTR(dims);
	ndims = ARR_DIMS(dims)[0];

	if (ndims < 0)				/* we do allow zero-dimension arrays */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid number of dimensions: %d", ndims)));
	if (ndims > MAXDIM)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
						ndims, MAXDIM)));

	if (lbs != NULL)
	{
		if (ARR_NDIM(lbs) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("wrong number of array subscripts"),
					 errdetail("Dimension array must be one dimensional.")));

		if (ARR_LBOUND(lbs)[0] != 1)
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("wrong range of array subscripts"),
				  errdetail("Lower bound of dimension array must be one.")));

		if (ARR_HASNULL(lbs))
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("dimension values cannot be null")));

		if (ARR_DIMS(lbs)[0] != ndims)
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("wrong number of array subscripts"),
					 errdetail("Low bound array has different size than dimensions array.")));

		lbsv = (int *) ARR_DATA_PTR(lbs);
	}
	else
	{
		int			i;

		for (i = 0; i < MAXDIM; i++)
			deflbs[i] = 1;

		lbsv = deflbs;
	}

	/* fast track for empty array */
	if (ndims == 0)
		return construct_empty_array(elmtype);

	nitems = ArrayGetNItems(ndims, dimv);

	/*
	 * We arrange to look up info about element type only once per series of
	 * calls, assuming the element type doesn't change underneath us.
	 */
	my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL)
	{
		fcinfo->flinfo->fn_extra = MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
													  sizeof(ArrayMetaState));
		my_extra = (ArrayMetaState *) fcinfo->flinfo->fn_extra;
		my_extra->element_type = InvalidOid;
	}

	if (my_extra->element_type != elmtype)
	{
		/* Get info about element type */
		get_typlenbyvalalign(elmtype,
							 &my_extra->typlen,
							 &my_extra->typbyval,
							 &my_extra->typalign);
		my_extra->element_type = elmtype;
	}

	elmlen = my_extra->typlen;
	elmbyval = my_extra->typbyval;
	elmalign = my_extra->typalign;

	/* compute required space */
	if (!isnull)
	{
		int			i;
		char	   *p;
		int			nbytes;
		int			totbytes;

		/* make sure data is not toasted */
		if (elmlen == -1)
			value = PointerGetDatum(PG_DETOAST_DATUM(value));

		nbytes = att_addlength_datum(0, elmlen, value);
		nbytes = att_align_nominal(nbytes, elmalign);
		Assert(nbytes > 0);

		totbytes = nbytes * nitems;

		/* check for overflow of multiplication or total request */
		if (totbytes / nbytes != nitems ||
			!AllocSizeIsValid(totbytes))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("array size exceeds the maximum allowed (%d)",
							(int) MaxAllocSize)));

		/*
		 * This addition can't overflow, but it might cause us to go past
		 * MaxAllocSize.  We leave it to palloc to complain in that case.
		 */
		totbytes += ARR_OVERHEAD_NONULLS(ndims);

		result = create_array_envelope(ndims, dimv, lbsv, totbytes,
									   elmtype, 0);

		p = ARR_DATA_PTR(result);
		for (i = 0; i < nitems; i++)
			p += ArrayCastAndSet(value, elmlen, elmbyval, elmalign, p);
	}
	else
	{
		int			nbytes;
		int			dataoffset;

		dataoffset = ARR_OVERHEAD_WITHNULLS(ndims, nitems);
		nbytes = dataoffset;

		result = create_array_envelope(ndims, dimv, lbsv, nbytes,
									   elmtype, dataoffset);

		/* create_array_envelope already zeroed the bitmap, so we're done */
	}

	return result;
}


/*
 * UNNEST
 */
Datum
array_unnest(PG_FUNCTION_ARGS)
{
	typedef struct
	{
		ArrayType  *arr;
		int			nextelem;
		int			numelems;
		char	   *elemdataptr;	/* this moves with nextelem */
		bits8	   *arraynullsptr;		/* this does not */
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
	} array_unnest_fctx;

	FuncCallContext *funcctx;
	array_unnest_fctx *fctx;
	MemoryContext oldcontext;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		ArrayType  *arr;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/*
		 * Get the array value and detoast if needed.  We can't do this
		 * earlier because if we have to detoast, we want the detoasted copy
		 * to be in multi_call_memory_ctx, so it will go away when we're done
		 * and not before.	(If no detoast happens, we assume the originally
		 * passed array will stick around till then.)
		 */
		arr = PG_GETARG_ARRAYTYPE_P(0);

		/* allocate memory for user context */
		fctx = (array_unnest_fctx *) palloc(sizeof(array_unnest_fctx));

		/* initialize state */
		fctx->arr = arr;
		fctx->nextelem = 0;
		fctx->numelems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));

		fctx->elemdataptr = ARR_DATA_PTR(arr);
		fctx->arraynullsptr = ARR_NULLBITMAP(arr);

		get_typlenbyvalalign(ARR_ELEMTYPE(arr),
							 &fctx->elmlen,
							 &fctx->elmbyval,
							 &fctx->elmalign);

		funcctx->user_fctx = fctx;
		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();
	fctx = funcctx->user_fctx;

	if (fctx->nextelem < fctx->numelems)
	{
		int			offset = fctx->nextelem++;
		Datum		elem;

		/*
		 * Check for NULL array element
		 */
		if (array_get_isnull(fctx->arraynullsptr, offset))
		{
			fcinfo->isnull = true;
			elem = (Datum) 0;
			/* elemdataptr does not move */
		}
		else
		{
			/*
			 * OK, get the element
			 */
			char	   *ptr = fctx->elemdataptr;

			fcinfo->isnull = false;
			elem = ArrayCast(ptr, fctx->elmbyval, fctx->elmlen);

			/*
			 * Advance elemdataptr over it
			 */
			ptr = att_addlength_pointer(ptr, fctx->elmlen, ptr);
			ptr = (char *) att_align_nominal(ptr, fctx->elmalign);
			fctx->elemdataptr = ptr;
		}

		SRF_RETURN_NEXT(funcctx, elem);
	}
	else
	{
		/* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
	}
}
