/*-------------------------------------------------------------------------
 *
 * arrayfuncs.c
 *	  Support functions for arrays.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/arrayfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <math.h>

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "optimizer/optimizer.h"
#include "port/pg_bitutils.h"
#include "utils/array.h"
#include "utils/arrayaccess.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/selfuncs.h"
#include "utils/typcache.h"


/*
 * GUC parameter
 */
bool		Array_nulls = true;

/*
 * Local definitions
 */
#define ASSGN	 "="

#define AARR_FREE_IF_COPY(array,n) \
	do { \
		if (!VARATT_IS_EXPANDED_HEADER(array)) \
			PG_FREE_IF_COPY(array, n); \
	} while (0)

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

/* Working state for array_iterate() */
typedef struct ArrayIteratorData
{
	/* basic info about the array, set up during array_create_iterator() */
	ArrayType  *arr;			/* array we're iterating through */
	bits8	   *nullbitmap;		/* its null bitmap, if any */
	int			nitems;			/* total number of elements in array */
	int16		typlen;			/* element type's length */
	bool		typbyval;		/* element type's byval property */
	char		typalign;		/* element type's align property */

	/* information about the requested slice size */
	int			slice_ndim;		/* slice dimension, or 0 if not slicing */
	int			slice_len;		/* number of elements per slice */
	int		   *slice_dims;		/* slice dims array */
	int		   *slice_lbound;	/* slice lbound array */
	Datum	   *slice_values;	/* workspace of length slice_len */
	bool	   *slice_nulls;	/* workspace of length slice_len */

	/* current position information, updated on each iteration */
	char	   *data_ptr;		/* our current position in the array */
	int			current_item;	/* the item # we're at in the array */
}			ArrayIteratorData;

static bool array_isspace(char ch);
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
static Datum array_get_element_expanded(Datum arraydatum,
										int nSubscripts, int *indx,
										int arraytyplen,
										int elmlen, bool elmbyval, char elmalign,
										bool *isNull);
static Datum array_set_element_expanded(Datum arraydatum,
										int nSubscripts, int *indx,
										Datum dataValue, bool isNull,
										int arraytyplen,
										int elmlen, bool elmbyval, char elmalign);
static bool array_get_isnull(const bits8 *nullbitmap, int offset);
static void array_set_isnull(bits8 *nullbitmap, int offset, bool isNull);
static Datum ArrayCast(char *value, bool byval, int len);
static int	ArrayCastAndSet(Datum src,
							int typlen, bool typbyval, char typalign,
							char *dest);
static char *array_seek(char *ptr, int offset, bits8 *nullbitmap, int nitems,
						int typlen, bool typbyval, char typalign);
static int	array_nelems_size(char *ptr, int offset, bits8 *nullbitmap,
							  int nitems, int typlen, bool typbyval, char typalign);
static int	array_copy(char *destptr, int nitems,
					   char *srcptr, int offset, bits8 *nullbitmap,
					   int typlen, bool typbyval, char typalign);
static int	array_slice_size(char *arraydataptr, bits8 *arraynullsptr,
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
static ArrayType *create_array_envelope(int ndims, int *dimv, int *lbsv, int nbytes,
										Oid elmtype, int dataoffset);
static ArrayType *array_fill_internal(ArrayType *dims, ArrayType *lbs,
									  Datum value, bool isnull, Oid elmtype,
									  FunctionCallInfo fcinfo);
static ArrayType *array_replace_internal(ArrayType *array,
										 Datum search, bool search_isnull,
										 Datum replace, bool replace_isnull,
										 bool remove, Oid collation,
										 FunctionCallInfo fcinfo);
static int	width_bucket_array_float8(Datum operand, ArrayType *thresholds);
static int	width_bucket_array_fixed(Datum operand,
									 ArrayType *thresholds,
									 Oid collation,
									 TypeCacheEntry *typentry);
static int	width_bucket_array_variable(Datum operand,
										ArrayType *thresholds,
										Oid collation,
										TypeCacheEntry *typentry);


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
	Oid			element_type = PG_GETARG_OID(1);	/* type of an array
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
		while (array_isspace(*p))
			p++;
		if (*p != '[')
			break;				/* no more dimension items */
		p++;
		if (ndim >= MAXDIM)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
							ndim + 1, MAXDIM)));

		for (q = p; isdigit((unsigned char) *q) || (*q == '-') || (*q == '+'); q++)
			 /* skip */ ;
		if (q == p)				/* no digits? */
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed array literal: \"%s\"", string),
					 errdetail("\"[\" must introduce explicitly-specified array dimensions.")));

		if (*q == ':')
		{
			/* [m:n] format */
			*q = '\0';
			lBound[ndim] = atoi(p);
			p = q + 1;
			for (q = p; isdigit((unsigned char) *q) || (*q == '-') || (*q == '+'); q++)
				 /* skip */ ;
			if (q == p)			/* no digits? */
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("malformed array literal: \"%s\"", string),
						 errdetail("Missing array dimension value.")));
		}
		else
		{
			/* [n] format */
			lBound[ndim] = 1;
		}
		if (*q != ']')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed array literal: \"%s\"", string),
					 errdetail("Missing \"%s\" after array dimensions.",
							   "]")));

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
					 errmsg("malformed array literal: \"%s\"", string),
					 errdetail("Array value must start with \"{\" or dimension information.")));
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
					 errmsg("malformed array literal: \"%s\"", string),
					 errdetail("Missing \"%s\" after array dimensions.",
							   ASSGN)));
		p += strlen(ASSGN);
		while (array_isspace(*p))
			p++;

		/*
		 * intuit dimensions from brace structure -- it better match what we
		 * were given
		 */
		if (*p != '{')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed array literal: \"%s\"", string),
					 errdetail("Array contents must start with \"{\".")));
		ndim_braces = ArrayCount(p, dim_braces, typdelim);
		if (ndim_braces != ndim)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed array literal: \"%s\"", string),
					 errdetail("Specified array dimensions do not match array contents.")));
		for (i = 0; i < ndim; ++i)
		{
			if (dim[i] != dim_braces[i])
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("malformed array literal: \"%s\"", string),
						 errdetail("Specified array dimensions do not match array contents.")));
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
	ArrayCheckBounds(ndim, dim, lBound);

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

	/*
	 * This comes from the array's pg_type.typelem (which points to the base
	 * data type's pg_type.oid) and stores system oids in user tables. This
	 * oid must be preserved by binary upgrades.
	 */
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
 * array_isspace() --- a non-locale-dependent isspace()
 *
 * We used to use isspace() for parsing array values, but that has
 * undesirable results: an array value might be silently interpreted
 * differently depending on the locale setting.  Now we just hard-wire
 * the traditional ASCII definition of isspace().
 */
static bool
array_isspace(char ch)
{
	if (ch == ' ' ||
		ch == '\t' ||
		ch == '\n' ||
		ch == '\r' ||
		ch == '\v' ||
		ch == '\f')
		return true;
	return false;
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
		temp[i] = dim[i] = nelems_last[i] = 0;
		nelems[i] = 1;
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
							 errmsg("malformed array literal: \"%s\"", str),
							 errdetail("Unexpected end of input.")));
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
								 errmsg("malformed array literal: \"%s\"", str),
								 errdetail("Unexpected \"%c\" character.",
										   '\\')));
					if (parse_state != ARRAY_QUOTED_ELEM_STARTED)
						parse_state = ARRAY_ELEM_STARTED;
					/* skip the escaped character */
					if (*(ptr + 1))
						ptr++;
					else
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
								 errmsg("malformed array literal: \"%s\"", str),
								 errdetail("Unexpected end of input.")));
					break;
				case '"':

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
								 errmsg("malformed array literal: \"%s\"", str),
								 errdetail("Unexpected array element.")));
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
									 errmsg("malformed array literal: \"%s\"", str),
									 errdetail("Unexpected \"%c\" character.",
											   '{')));
						parse_state = ARRAY_LEVEL_STARTED;
						if (nest_level >= MAXDIM)
							ereport(ERROR,
									(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
									 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
											nest_level + 1, MAXDIM)));
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
									 errmsg("malformed array literal: \"%s\"", str),
									 errdetail("Unexpected \"%c\" character.",
											   '}')));
						parse_state = ARRAY_LEVEL_COMPLETED;
						if (nest_level == 0)
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
									 errmsg("malformed array literal: \"%s\"", str),
									 errdetail("Unmatched \"%c\" character.", '}')));
						nest_level--;

						if (nelems_last[nest_level] != 0 &&
							nelems[nest_level] != nelems_last[nest_level])
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
									 errmsg("malformed array literal: \"%s\"", str),
									 errdetail("Multidimensional arrays must have "
											   "sub-arrays with matching "
											   "dimensions.")));
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
										 errmsg("malformed array literal: \"%s\"", str),
										 errdetail("Unexpected \"%c\" character.",
												   typdelim)));
							if (parse_state == ARRAY_LEVEL_COMPLETED)
								parse_state = ARRAY_LEVEL_DELIMITED;
							else
								parse_state = ARRAY_ELEM_DELIMITED;
							itemdone = true;
							nelems[nest_level - 1]++;
						}
						else if (!array_isspace(*ptr))
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
										 errmsg("malformed array literal: \"%s\"", str),
										 errdetail("Unexpected array element.")));
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
		if (!array_isspace(*ptr++))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("malformed array literal: \"%s\"", str),
					 errdetail("Junk after closing right brace.")));
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
 *	*hasnulls: set true iff there are any null elements.
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
	 * pass to the datatype input routine.  We overwrite each item value
	 * in-place within arrayStr to do this.  srcptr is the current scan point,
	 * and dstptr is where we are copying to.
	 *
	 * We also want to suppress leading and trailing unquoted whitespace. We
	 * use the leadingspace flag to suppress leading space.  Trailing space is
	 * tracked by using dstendptr to point to the last significant output
	 * character.
	 *
	 * The error checking in this routine is mostly pro-forma, since we expect
	 * that ArrayCount() already validated the string.  So we don't bother
	 * with errdetail messages.
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
				case '"':
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
					else if (array_isspace(*srcptr))
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
 * freedata: if true and element type is pass-by-ref, pfree data values
 * referenced by Datums after copying them.
 *
 * If the input data is of varlena type, the caller must have ensured that
 * the values are not toasted.  (Doing it here doesn't work since the
 * caller has already allocated space for the array...)
 */
void
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
	AnyArrayType *v = PG_GETARG_ANY_ARRAY_P(0);
	Oid			element_type = AARR_ELEMTYPE(v);
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
	bool	   *needquotes,
				needdims = false;
	size_t		overall_length;
	int			nitems,
				i,
				j,
				k,
				indx[MAXDIM];
	int			ndim,
			   *dims,
			   *lb;
	array_iter	iter;
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

	ndim = AARR_NDIM(v);
	dims = AARR_DIMS(v);
	lb = AARR_LBOUND(v);
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
	overall_length = 0;

	array_iter_setup(&iter, v);

	for (i = 0; i < nitems; i++)
	{
		Datum		itemvalue;
		bool		isnull;
		bool		needquote;

		/* Get source element, checking for NULL */
		itemvalue = array_iter_next(&iter, &isnull, i,
									typlen, typbyval, typalign);

		if (isnull)
		{
			values[i] = pstrdup("NULL");
			overall_length += 4;
			needquote = false;
		}
		else
		{
			values[i] = OutputFunctionCall(&my_extra->proc, itemvalue);

			/* count data plus backslashes; detect chars needing quotes */
			if (values[i][0] == '\0')
				needquote = true;	/* force quotes for empty string */
			else if (pg_strcasecmp(values[i], "NULL") == 0)
				needquote = true;	/* force quotes for literal NULL */
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
						 array_isspace(ch))
					needquote = true;
			}
		}

		needquotes[i] = needquote;

		/* Count the pair of double quotes, if needed */
		if (needquote)
			overall_length += 2;
		/* and the comma (or other typdelim delimiter) */
		overall_length += 1;
	}

	/*
	 * The very last array element doesn't have a typdelim delimiter after it,
	 * but that's OK; that space is needed for the trailing '\0'.
	 *
	 * Now count total number of curly brace pairs in output string.
	 */
	for (i = j = 0, k = 1; i < ndim; i++)
	{
		j += k, k *= dims[i];
	}
	overall_length += 2 * j;

	/* Format explicit dimensions if required */
	dims_str[0] = '\0';
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
		overall_length += ptr - dims_str;
	}

	/* Now construct the output string */
	retval = (char *) palloc(overall_length);
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
			if (++(indx[i]) < dims[i])
			{
				APPENDCHAR(typdelim);
				break;
			}
			else
			{
				indx[i] = 0;
				APPENDCHAR('}');
			}
		}
		j = i;
	} while (j != -1);

#undef APPENDSTR
#undef APPENDCHAR

	/* Assert that we calculated the string length accurately */
	Assert(overall_length == (p - retval + 1));

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

	/* Check element type recorded in the data */
	element_type = pq_getmsgint(buf, sizeof(Oid));

	/*
	 * From a security standpoint, it doesn't matter whether the input's
	 * element type matches what we expect: the element type's receive
	 * function has to be robust enough to cope with invalid data.  However,
	 * from a user-friendliness standpoint, it's nicer to complain about type
	 * mismatches than to throw "improper binary format" errors.  But there's
	 * a problem: only built-in types have OIDs that are stable enough to
	 * believe that a mismatch is a real issue.  So complain only if both OIDs
	 * are in the built-in range.  Otherwise, carry on with the element type
	 * we "should" be getting.
	 */
	if (element_type != spec_element_type)
	{
		if (element_type < FirstGenbkiObjectId &&
			spec_element_type < FirstGenbkiObjectId)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("binary data has array element type %u (%s) instead of expected %u (%s)",
							element_type,
							format_type_extended(element_type, -1,
												 FORMAT_TYPE_ALLOW_INVALID),
							spec_element_type,
							format_type_extended(spec_element_type, -1,
												 FORMAT_TYPE_ALLOW_INVALID))));
		element_type = spec_element_type;
	}

	for (i = 0; i < ndim; i++)
	{
		dim[i] = pq_getmsgint(buf, 4);
		lBound[i] = pq_getmsgint(buf, 4);
	}

	/* This checks for overflow of array dimensions */
	nitems = ArrayGetNItems(ndim, dim);
	ArrayCheckBounds(ndim, dim, lBound);

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
 *	*hasnulls: set true iff there are any null elements.
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
	AnyArrayType *v = PG_GETARG_ANY_ARRAY_P(0);
	Oid			element_type = AARR_ELEMTYPE(v);
	int			typlen;
	bool		typbyval;
	char		typalign;
	int			nitems,
				i;
	int			ndim,
			   *dim,
			   *lb;
	StringInfoData buf;
	array_iter	iter;
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

	ndim = AARR_NDIM(v);
	dim = AARR_DIMS(v);
	lb = AARR_LBOUND(v);
	nitems = ArrayGetNItems(ndim, dim);

	pq_begintypsend(&buf);

	/* Send the array header information */
	pq_sendint32(&buf, ndim);
	pq_sendint32(&buf, AARR_HASNULL(v) ? 1 : 0);
	pq_sendint32(&buf, element_type);
	for (i = 0; i < ndim; i++)
	{
		pq_sendint32(&buf, dim[i]);
		pq_sendint32(&buf, lb[i]);
	}

	/* Send the array elements using the element's own sendproc */
	array_iter_setup(&iter, v);

	for (i = 0; i < nitems; i++)
	{
		Datum		itemvalue;
		bool		isnull;

		/* Get source element, checking for NULL */
		itemvalue = array_iter_next(&iter, &isnull, i,
									typlen, typbyval, typalign);

		if (isnull)
		{
			/* -1 length means a NULL */
			pq_sendint32(&buf, -1);
		}
		else
		{
			bytea	   *outputbytes;

			outputbytes = SendFunctionCall(&my_extra->proc, itemvalue);
			pq_sendint32(&buf, VARSIZE(outputbytes) - VARHDRSZ);
			pq_sendbytes(&buf, VARDATA(outputbytes),
						 VARSIZE(outputbytes) - VARHDRSZ);
			pfree(outputbytes);
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
	AnyArrayType *v = PG_GETARG_ANY_ARRAY_P(0);

	/* Sanity check: does it look like an array at all? */
	if (AARR_NDIM(v) <= 0 || AARR_NDIM(v) > MAXDIM)
		PG_RETURN_NULL();

	PG_RETURN_INT32(AARR_NDIM(v));
}

/*
 * array_dims :
 *		  returns the dimensions of the array pointed to by "v", as a "text"
 */
Datum
array_dims(PG_FUNCTION_ARGS)
{
	AnyArrayType *v = PG_GETARG_ANY_ARRAY_P(0);
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
	if (AARR_NDIM(v) <= 0 || AARR_NDIM(v) > MAXDIM)
		PG_RETURN_NULL();

	dimv = AARR_DIMS(v);
	lb = AARR_LBOUND(v);

	p = buf;
	for (i = 0; i < AARR_NDIM(v); i++)
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
	AnyArrayType *v = PG_GETARG_ANY_ARRAY_P(0);
	int			reqdim = PG_GETARG_INT32(1);
	int		   *lb;
	int			result;

	/* Sanity check: does it look like an array at all? */
	if (AARR_NDIM(v) <= 0 || AARR_NDIM(v) > MAXDIM)
		PG_RETURN_NULL();

	/* Sanity check: was the requested dim valid */
	if (reqdim <= 0 || reqdim > AARR_NDIM(v))
		PG_RETURN_NULL();

	lb = AARR_LBOUND(v);
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
	AnyArrayType *v = PG_GETARG_ANY_ARRAY_P(0);
	int			reqdim = PG_GETARG_INT32(1);
	int		   *dimv,
			   *lb;
	int			result;

	/* Sanity check: does it look like an array at all? */
	if (AARR_NDIM(v) <= 0 || AARR_NDIM(v) > MAXDIM)
		PG_RETURN_NULL();

	/* Sanity check: was the requested dim valid */
	if (reqdim <= 0 || reqdim > AARR_NDIM(v))
		PG_RETURN_NULL();

	lb = AARR_LBOUND(v);
	dimv = AARR_DIMS(v);

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
	AnyArrayType *v = PG_GETARG_ANY_ARRAY_P(0);
	int			reqdim = PG_GETARG_INT32(1);
	int		   *dimv;
	int			result;

	/* Sanity check: does it look like an array at all? */
	if (AARR_NDIM(v) <= 0 || AARR_NDIM(v) > MAXDIM)
		PG_RETURN_NULL();

	/* Sanity check: was the requested dim valid */
	if (reqdim <= 0 || reqdim > AARR_NDIM(v))
		PG_RETURN_NULL();

	dimv = AARR_DIMS(v);

	result = dimv[reqdim - 1];

	PG_RETURN_INT32(result);
}

/*
 * array_cardinality:
 *		returns the total number of elements in an array
 */
Datum
array_cardinality(PG_FUNCTION_ARGS)
{
	AnyArrayType *v = PG_GETARG_ANY_ARRAY_P(0);

	PG_RETURN_INT32(ArrayGetNItems(AARR_NDIM(v), AARR_DIMS(v)));
}


/*
 * array_get_element :
 *	  This routine takes an array datum and a subscript array and returns
 *	  the referenced item as a Datum.  Note that for a pass-by-reference
 *	  datatype, the returned Datum is a pointer into the array object.
 *
 * This handles both ordinary varlena arrays and fixed-length arrays.
 *
 * Inputs:
 *	arraydatum: the array object (mustn't be NULL)
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
array_get_element(Datum arraydatum,
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
		arraydataptr = (char *) DatumGetPointer(arraydatum);
		arraynullsptr = NULL;
	}
	else if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(arraydatum)))
	{
		/* expanded array: let's do this in a separate function */
		return array_get_element_expanded(arraydatum,
										  nSubscripts,
										  indx,
										  arraytyplen,
										  elmlen,
										  elmbyval,
										  elmalign,
										  isNull);
	}
	else
	{
		/* detoast array if necessary, producing normal varlena input */
		ArrayType  *array = DatumGetArrayTypeP(arraydatum);

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
 * Implementation of array_get_element() for an expanded array
 */
static Datum
array_get_element_expanded(Datum arraydatum,
						   int nSubscripts, int *indx,
						   int arraytyplen,
						   int elmlen, bool elmbyval, char elmalign,
						   bool *isNull)
{
	ExpandedArrayHeader *eah;
	int			i,
				ndim,
			   *dim,
			   *lb,
				offset;
	Datum	   *dvalues;
	bool	   *dnulls;

	eah = (ExpandedArrayHeader *) DatumGetEOHP(arraydatum);
	Assert(eah->ea_magic == EA_MAGIC);

	/* sanity-check caller's info against object */
	Assert(arraytyplen == -1);
	Assert(elmlen == eah->typlen);
	Assert(elmbyval == eah->typbyval);
	Assert(elmalign == eah->typalign);

	ndim = eah->ndims;
	dim = eah->dims;
	lb = eah->lbound;

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
	 * Deconstruct array if we didn't already.  Note that we apply this even
	 * if the input is nominally read-only: it should be safe enough.
	 */
	deconstruct_expanded_array(eah);

	dvalues = eah->dvalues;
	dnulls = eah->dnulls;

	/*
	 * Check for NULL array element
	 */
	if (dnulls && dnulls[offset])
	{
		*isNull = true;
		return (Datum) 0;
	}

	/*
	 * OK, get the element.  It's OK to return a pass-by-ref value as a
	 * pointer into the expanded array, for the same reason that regular
	 * array_get_element can return a pointer into flat arrays: the value is
	 * assumed not to change for as long as the Datum reference can exist.
	 */
	*isNull = false;
	return dvalues[offset];
}

/*
 * array_get_slice :
 *		   This routine takes an array and a range of indices (upperIndx and
 *		   lowerIndx), creates a new array structure for the referred elements
 *		   and returns a pointer to it.
 *
 * This handles both ordinary varlena arrays and fixed-length arrays.
 *
 * Inputs:
 *	arraydatum: the array object (mustn't be NULL)
 *	nSubscripts: number of subscripts supplied (must be same for upper/lower)
 *	upperIndx[]: the upper subscript values
 *	lowerIndx[]: the lower subscript values
 *	upperProvided[]: true for provided upper subscript values
 *	lowerProvided[]: true for provided lower subscript values
 *	arraytyplen: pg_type.typlen for the array type
 *	elmlen: pg_type.typlen for the array's element type
 *	elmbyval: pg_type.typbyval for the array's element type
 *	elmalign: pg_type.typalign for the array's element type
 *
 * Outputs:
 *	The return value is the new array Datum (it's never NULL)
 *
 * Omitted upper and lower subscript values are replaced by the corresponding
 * array bound.
 *
 * NOTE: we assume it is OK to scribble on the provided subscript arrays
 * lowerIndx[] and upperIndx[]; also, these arrays must be of size MAXDIM
 * even when nSubscripts is less.  These are generally just temporaries.
 */
Datum
array_get_slice(Datum arraydatum,
				int nSubscripts,
				int *upperIndx,
				int *lowerIndx,
				bool *upperProvided,
				bool *lowerProvided,
				int arraytyplen,
				int elmlen,
				bool elmbyval,
				char elmalign)
{
	ArrayType  *array;
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
		arraydataptr = (char *) DatumGetPointer(arraydatum);
		arraynullsptr = NULL;
	}
	else
	{
		/* detoast input array if necessary */
		array = DatumGetArrayTypeP(arraydatum);

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
		return PointerGetDatum(construct_empty_array(elemtype));

	for (i = 0; i < nSubscripts; i++)
	{
		if (!lowerProvided[i] || lowerIndx[i] < lb[i])
			lowerIndx[i] = lb[i];
		if (!upperProvided[i] || upperIndx[i] >= (dim[i] + lb[i]))
			upperIndx[i] = dim[i] + lb[i] - 1;
		if (lowerIndx[i] > upperIndx[i])
			return PointerGetDatum(construct_empty_array(elemtype));
	}
	/* fill any missing subscript positions with full array range */
	for (; i < ndim; i++)
	{
		lowerIndx[i] = lb[i];
		upperIndx[i] = dim[i] + lb[i] - 1;
		if (lowerIndx[i] > upperIndx[i])
			return PointerGetDatum(construct_empty_array(elemtype));
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

	newarray = (ArrayType *) palloc0(bytes);
	SET_VARSIZE(newarray, bytes);
	newarray->ndim = ndim;
	newarray->dataoffset = dataoffset;
	newarray->elemtype = elemtype;
	memcpy(ARR_DIMS(newarray), span, ndim * sizeof(int));

	/*
	 * Lower bounds of the new array are set to 1.  Formerly (before 7.3) we
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

	return PointerGetDatum(newarray);
}

/*
 * array_set_element :
 *		  This routine sets the value of one array element (specified by
 *		  a subscript array) to a new value specified by "dataValue".
 *
 * This handles both ordinary varlena arrays and fixed-length arrays.
 *
 * Inputs:
 *	arraydatum: the initial array object (mustn't be NULL)
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
 *		  modified entry.  The original array object is not changed,
 *		  unless what is passed is a read-write reference to an expanded
 *		  array object; in that case the expanded array is updated in-place.
 *
 * For one-dimensional arrays only, we allow the array to be extended
 * by assigning to a position outside the existing subscript range; any
 * positions between the existing elements and the new one are set to NULLs.
 * (XXX TODO: allow a corresponding behavior for multidimensional arrays)
 *
 * NOTE: For assignments, we throw an error for invalid subscripts etc,
 * rather than returning a NULL as the fetch operations do.
 */
Datum
array_set_element(Datum arraydatum,
				  int nSubscripts,
				  int *indx,
				  Datum dataValue,
				  bool isNull,
				  int arraytyplen,
				  int elmlen,
				  bool elmbyval,
				  char elmalign)
{
	ArrayType  *array;
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
		char	   *resultarray;

		if (nSubscripts != 1)
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("wrong number of array subscripts")));

		if (indx[0] < 0 || indx[0] >= arraytyplen / elmlen)
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("array subscript out of range")));

		if (isNull)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("cannot assign null value to an element of a fixed-length array")));

		resultarray = (char *) palloc(arraytyplen);
		memcpy(resultarray, DatumGetPointer(arraydatum), arraytyplen);
		elt_ptr = (char *) resultarray + indx[0] * elmlen;
		ArrayCastAndSet(dataValue, elmlen, elmbyval, elmalign, elt_ptr);
		return PointerGetDatum(resultarray);
	}

	if (nSubscripts <= 0 || nSubscripts > MAXDIM)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("wrong number of array subscripts")));

	/* make sure item to be inserted is not toasted */
	if (elmlen == -1 && !isNull)
		dataValue = PointerGetDatum(PG_DETOAST_DATUM(dataValue));

	if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(arraydatum)))
	{
		/* expanded array: let's do this in a separate function */
		return array_set_element_expanded(arraydatum,
										  nSubscripts,
										  indx,
										  dataValue,
										  isNull,
										  arraytyplen,
										  elmlen,
										  elmbyval,
										  elmalign);
	}

	/* detoast input array if necessary */
	array = DatumGetArrayTypeP(arraydatum);

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

		return PointerGetDatum(construct_md_array(&dataValue, &isNull,
												  nSubscripts, dim, lb,
												  elmtype,
												  elmlen, elmbyval, elmalign));
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
				newhasnulls = true; /* will insert nulls */
		}
		if (indx[0] >= (dim[0] + lb[0]))
		{
			addedafter = indx[0] - (dim[0] + lb[0]) + 1;
			dim[0] += addedafter;
			if (addedafter > 1)
				newhasnulls = true; /* will insert nulls */
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

	/* This checks for overflow of the array dimensions */
	newnitems = ArrayGetNItems(ndim, dim);
	ArrayCheckBounds(ndim, dim, lb);

	/*
	 * Compute sizes of items and areas to copy
	 */
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
	newarray = (ArrayType *) palloc0(newsize);
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

	return PointerGetDatum(newarray);
}

/*
 * Implementation of array_set_element() for an expanded array
 *
 * Note: as with any operation on a read/write expanded object, we must
 * take pains not to leave the object in a corrupt state if we fail partway
 * through.
 */
static Datum
array_set_element_expanded(Datum arraydatum,
						   int nSubscripts, int *indx,
						   Datum dataValue, bool isNull,
						   int arraytyplen,
						   int elmlen, bool elmbyval, char elmalign)
{
	ExpandedArrayHeader *eah;
	Datum	   *dvalues;
	bool	   *dnulls;
	int			i,
				ndim,
				dim[MAXDIM],
				lb[MAXDIM],
				offset;
	bool		dimschanged,
				newhasnulls;
	int			addedbefore,
				addedafter;
	char	   *oldValue;

	/* Convert to R/W object if not so already */
	eah = DatumGetExpandedArray(arraydatum);

	/* Sanity-check caller's info against object; we don't use it otherwise */
	Assert(arraytyplen == -1);
	Assert(elmlen == eah->typlen);
	Assert(elmbyval == eah->typbyval);
	Assert(elmalign == eah->typalign);

	/*
	 * Copy dimension info into local storage.  This allows us to modify the
	 * dimensions if needed, while not messing up the expanded value if we
	 * fail partway through.
	 */
	ndim = eah->ndims;
	Assert(ndim >= 0 && ndim <= MAXDIM);
	memcpy(dim, eah->dims, ndim * sizeof(int));
	memcpy(lb, eah->lbound, ndim * sizeof(int));
	dimschanged = false;

	/*
	 * if number of dims is zero, i.e. an empty array, create an array with
	 * nSubscripts dimensions, and set the lower bounds to the supplied
	 * subscripts.
	 */
	if (ndim == 0)
	{
		/*
		 * Allocate adequate space for new dimension info.  This is harmless
		 * if we fail later.
		 */
		Assert(nSubscripts > 0 && nSubscripts <= MAXDIM);
		eah->dims = (int *) MemoryContextAllocZero(eah->hdr.eoh_context,
												   nSubscripts * sizeof(int));
		eah->lbound = (int *) MemoryContextAllocZero(eah->hdr.eoh_context,
													 nSubscripts * sizeof(int));

		/* Update local copies of dimension info */
		ndim = nSubscripts;
		for (i = 0; i < nSubscripts; i++)
		{
			dim[i] = 0;
			lb[i] = indx[i];
		}
		dimschanged = true;
	}
	else if (ndim != nSubscripts)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("wrong number of array subscripts")));

	/*
	 * Deconstruct array if we didn't already.  (Someday maybe add a special
	 * case path for fixed-length, no-nulls cases, where we can overwrite an
	 * element in place without ever deconstructing.  But today is not that
	 * day.)
	 */
	deconstruct_expanded_array(eah);

	/*
	 * Copy new element into array's context, if needed (we assume it's
	 * already detoasted, so no junk should be created).  Doing this before
	 * we've made any significant changes ensures that our behavior is sane
	 * even when the source is a reference to some element of this same array.
	 * If we fail further down, this memory is leaked, but that's reasonably
	 * harmless.
	 */
	if (!eah->typbyval && !isNull)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(eah->hdr.eoh_context);

		dataValue = datumCopy(dataValue, false, eah->typlen);
		MemoryContextSwitchTo(oldcxt);
	}

	dvalues = eah->dvalues;
	dnulls = eah->dnulls;

	newhasnulls = ((dnulls != NULL) || isNull);
	addedbefore = addedafter = 0;

	/*
	 * Check subscripts (this logic matches original array_set_element)
	 */
	if (ndim == 1)
	{
		if (indx[0] < lb[0])
		{
			addedbefore = lb[0] - indx[0];
			dim[0] += addedbefore;
			lb[0] = indx[0];
			dimschanged = true;
			if (addedbefore > 1)
				newhasnulls = true; /* will insert nulls */
		}
		if (indx[0] >= (dim[0] + lb[0]))
		{
			addedafter = indx[0] - (dim[0] + lb[0]) + 1;
			dim[0] += addedafter;
			dimschanged = true;
			if (addedafter > 1)
				newhasnulls = true; /* will insert nulls */
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

	/* Check for overflow of the array dimensions */
	if (dimschanged)
	{
		(void) ArrayGetNItems(ndim, dim);
		ArrayCheckBounds(ndim, dim, lb);
	}

	/* Now we can calculate linear offset of target item in array */
	offset = ArrayGetOffset(nSubscripts, dim, lb, indx);

	/* Physically enlarge existing dvalues/dnulls arrays if needed */
	if (dim[0] > eah->dvalueslen)
	{
		/* We want some extra space if we're enlarging */
		int			newlen = dim[0] + dim[0] / 8;

		newlen = Max(newlen, dim[0]);	/* integer overflow guard */
		eah->dvalues = dvalues = (Datum *)
			repalloc(dvalues, newlen * sizeof(Datum));
		if (dnulls)
			eah->dnulls = dnulls = (bool *)
				repalloc(dnulls, newlen * sizeof(bool));
		eah->dvalueslen = newlen;
	}

	/*
	 * If we need a nulls bitmap and don't already have one, create it, being
	 * sure to mark all existing entries as not null.
	 */
	if (newhasnulls && dnulls == NULL)
		eah->dnulls = dnulls = (bool *)
			MemoryContextAllocZero(eah->hdr.eoh_context,
								   eah->dvalueslen * sizeof(bool));

	/*
	 * We now have all the needed space allocated, so we're ready to make
	 * irreversible changes.  Be very wary of allowing failure below here.
	 */

	/* Flattened value will no longer represent array accurately */
	eah->fvalue = NULL;
	/* And we don't know the flattened size either */
	eah->flat_size = 0;

	/* Update dimensionality info if needed */
	if (dimschanged)
	{
		eah->ndims = ndim;
		memcpy(eah->dims, dim, ndim * sizeof(int));
		memcpy(eah->lbound, lb, ndim * sizeof(int));
	}

	/* Reposition items if needed, and fill addedbefore items with nulls */
	if (addedbefore > 0)
	{
		memmove(dvalues + addedbefore, dvalues, eah->nelems * sizeof(Datum));
		for (i = 0; i < addedbefore; i++)
			dvalues[i] = (Datum) 0;
		if (dnulls)
		{
			memmove(dnulls + addedbefore, dnulls, eah->nelems * sizeof(bool));
			for (i = 0; i < addedbefore; i++)
				dnulls[i] = true;
		}
		eah->nelems += addedbefore;
	}

	/* fill addedafter items with nulls */
	if (addedafter > 0)
	{
		for (i = 0; i < addedafter; i++)
			dvalues[eah->nelems + i] = (Datum) 0;
		if (dnulls)
		{
			for (i = 0; i < addedafter; i++)
				dnulls[eah->nelems + i] = true;
		}
		eah->nelems += addedafter;
	}

	/* Grab old element value for pfree'ing, if needed. */
	if (!eah->typbyval && (dnulls == NULL || !dnulls[offset]))
		oldValue = (char *) DatumGetPointer(dvalues[offset]);
	else
		oldValue = NULL;

	/* And finally we can insert the new element. */
	dvalues[offset] = dataValue;
	if (dnulls)
		dnulls[offset] = isNull;

	/*
	 * Free old element if needed; this keeps repeated element replacements
	 * from bloating the array's storage.  If the pfree somehow fails, it
	 * won't corrupt the array.
	 */
	if (oldValue)
	{
		/* Don't try to pfree a part of the original flat array */
		if (oldValue < eah->fstartptr || oldValue >= eah->fendptr)
			pfree(oldValue);
	}

	/* Done, return standard TOAST pointer for object */
	return EOHPGetRWDatum(&eah->hdr);
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
 *	arraydatum: the initial array object (mustn't be NULL)
 *	nSubscripts: number of subscripts supplied (must be same for upper/lower)
 *	upperIndx[]: the upper subscript values
 *	lowerIndx[]: the lower subscript values
 *	upperProvided[]: true for provided upper subscript values
 *	lowerProvided[]: true for provided lower subscript values
 *	srcArrayDatum: the source for the inserted values
 *	isNull: indicates whether srcArrayDatum is NULL
 *	arraytyplen: pg_type.typlen for the array type
 *	elmlen: pg_type.typlen for the array's element type
 *	elmbyval: pg_type.typbyval for the array's element type
 *	elmalign: pg_type.typalign for the array's element type
 *
 * Result:
 *		  A new array is returned, just like the old except for the
 *		  modified range.  The original array object is not changed.
 *
 * Omitted upper and lower subscript values are replaced by the corresponding
 * array bound.
 *
 * For one-dimensional arrays only, we allow the array to be extended
 * by assigning to positions outside the existing subscript range; any
 * positions between the existing elements and the new ones are set to NULLs.
 * (XXX TODO: allow a corresponding behavior for multidimensional arrays)
 *
 * NOTE: we assume it is OK to scribble on the provided index arrays
 * lowerIndx[] and upperIndx[]; also, these arrays must be of size MAXDIM
 * even when nSubscripts is less.  These are generally just temporaries.
 *
 * NOTE: For assignments, we throw an error for silly subscripts etc,
 * rather than returning a NULL or empty array as the fetch operations do.
 */
Datum
array_set_slice(Datum arraydatum,
				int nSubscripts,
				int *upperIndx,
				int *lowerIndx,
				bool *upperProvided,
				bool *lowerProvided,
				Datum srcArrayDatum,
				bool isNull,
				int arraytyplen,
				int elmlen,
				bool elmbyval,
				char elmalign)
{
	ArrayType  *array;
	ArrayType  *srcArray;
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
		return arraydatum;

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
	array = DatumGetArrayTypeP(arraydatum);
	srcArray = DatumGetArrayTypeP(srcArrayDatum);

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
			if (!upperProvided[i] || !lowerProvided[i])
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("array slice subscript must provide both boundaries"),
						 errdetail("When assigning to a slice of an empty array value,"
								   " slice boundaries must be fully specified.")));

			dim[i] = 1 + upperIndx[i] - lowerIndx[i];
			lb[i] = lowerIndx[i];
		}

		/* complain if too few source items; we ignore extras, however */
		if (nelems < ArrayGetNItems(nSubscripts, dim))
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("source array too small")));

		return PointerGetDatum(construct_md_array(dvalues, dnulls, nSubscripts,
												  dim, lb, elmtype,
												  elmlen, elmbyval, elmalign));
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
		if (!lowerProvided[0])
			lowerIndx[0] = lb[0];
		if (!upperProvided[0])
			upperIndx[0] = dim[0] + lb[0] - 1;
		if (lowerIndx[0] > upperIndx[0])
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("upper bound cannot be less than lower bound")));
		if (lowerIndx[0] < lb[0])
		{
			if (upperIndx[0] < lb[0] - 1)
				newhasnulls = true; /* will insert nulls */
			addedbefore = lb[0] - lowerIndx[0];
			dim[0] += addedbefore;
			lb[0] = lowerIndx[0];
		}
		if (upperIndx[0] >= (dim[0] + lb[0]))
		{
			if (lowerIndx[0] > (dim[0] + lb[0]))
				newhasnulls = true; /* will insert nulls */
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
			if (!lowerProvided[i])
				lowerIndx[i] = lb[i];
			if (!upperProvided[i])
				upperIndx[i] = dim[i] + lb[i] - 1;
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
	ArrayCheckBounds(ndim, dim, lb);

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
		lenbefore = lenafter = 0;	/* keep compiler quiet */
		itemsbefore = itemsafter = nolditems = 0;
	}
	else
	{
		/*
		 * here we must allow for possibility of slice larger than orig array
		 * and/or not adjacent to orig array subscripts
		 */
		int			oldlb = ARR_LBOUND(array)[0];
		int			oldub = oldlb + ARR_DIMS(array)[0] - 1;
		int			slicelb = Max(oldlb, lowerIndx[0]);
		int			sliceub = Min(oldub, upperIndx[0]);
		char	   *oldarraydata = ARR_DATA_PTR(array);
		bits8	   *oldarraybitmap = ARR_NULLBITMAP(array);

		/* count/size of old array entries that will go before the slice */
		itemsbefore = Min(slicelb, oldub + 1) - oldlb;
		lenbefore = array_nelems_size(oldarraydata, 0, oldarraybitmap,
									  itemsbefore,
									  elmlen, elmbyval, elmalign);
		/* count/size of old array entries that will be replaced by slice */
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
		/* count/size of old array entries that will go after the slice */
		itemsafter = oldub + 1 - Max(sliceub + 1, oldlb);
		lenafter = olddatasize - lenbefore - olditemsize;
	}

	newsize = overheadlen + olddatasize - olditemsize + newitemsize;

	newarray = (ArrayType *) palloc0(newsize);
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

	return PointerGetDatum(newarray);
}

/*
 * array_ref : backwards compatibility wrapper for array_get_element
 *
 * This only works for detoasted/flattened varlena arrays, since the array
 * argument is declared as "ArrayType *".  However there's enough code like
 * that to justify preserving this API.
 */
Datum
array_ref(ArrayType *array, int nSubscripts, int *indx,
		  int arraytyplen, int elmlen, bool elmbyval, char elmalign,
		  bool *isNull)
{
	return array_get_element(PointerGetDatum(array), nSubscripts, indx,
							 arraytyplen, elmlen, elmbyval, elmalign,
							 isNull);
}

/*
 * array_set : backwards compatibility wrapper for array_set_element
 *
 * This only works for detoasted/flattened varlena arrays, since the array
 * argument and result are declared as "ArrayType *".  However there's enough
 * code like that to justify preserving this API.
 */
ArrayType *
array_set(ArrayType *array, int nSubscripts, int *indx,
		  Datum dataValue, bool isNull,
		  int arraytyplen, int elmlen, bool elmbyval, char elmalign)
{
	return DatumGetArrayTypeP(array_set_element(PointerGetDatum(array),
												nSubscripts, indx,
												dataValue, isNull,
												arraytyplen,
												elmlen, elmbyval, elmalign));
}

/*
 * array_map()
 *
 * Map an array through an arbitrary expression.  Return a new array with
 * the same dimensions and each source element transformed by the given,
 * already-compiled expression.  Each source element is placed in the
 * innermost_caseval/innermost_casenull fields of the ExprState.
 *
 * Parameters are:
 * * arrayd: Datum representing array argument.
 * * exprstate: ExprState representing the per-element transformation.
 * * econtext: context for expression evaluation.
 * * retType: OID of element type of output array.  This must be the same as,
 *	 or binary-compatible with, the result type of the expression.  It might
 *	 be different from the input array's element type.
 * * amstate: workspace for array_map.  Must be zeroed by caller before
 *	 first call, and not touched after that.
 *
 * It is legitimate to pass a freshly-zeroed ArrayMapState on each call,
 * but better performance can be had if the state can be preserved across
 * a series of calls.
 *
 * NB: caller must assure that input array is not NULL.  NULL elements in
 * the array are OK however.
 * NB: caller should be running in econtext's per-tuple memory context.
 */
Datum
array_map(Datum arrayd,
		  ExprState *exprstate, ExprContext *econtext,
		  Oid retType, ArrayMapState *amstate)
{
	AnyArrayType *v = DatumGetAnyArrayP(arrayd);
	ArrayType  *result;
	Datum	   *values;
	bool	   *nulls;
	int		   *dim;
	int			ndim;
	int			nitems;
	int			i;
	int32		nbytes = 0;
	int32		dataoffset;
	bool		hasnulls;
	Oid			inpType;
	int			inp_typlen;
	bool		inp_typbyval;
	char		inp_typalign;
	int			typlen;
	bool		typbyval;
	char		typalign;
	array_iter	iter;
	ArrayMetaState *inp_extra;
	ArrayMetaState *ret_extra;
	Datum	   *transform_source = exprstate->innermost_caseval;
	bool	   *transform_source_isnull = exprstate->innermost_casenull;

	inpType = AARR_ELEMTYPE(v);
	ndim = AARR_NDIM(v);
	dim = AARR_DIMS(v);
	nitems = ArrayGetNItems(ndim, dim);

	/* Check for empty array */
	if (nitems <= 0)
	{
		/* Return empty array */
		return PointerGetDatum(construct_empty_array(retType));
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
	array_iter_setup(&iter, v);
	hasnulls = false;

	for (i = 0; i < nitems; i++)
	{
		/* Get source element, checking for NULL */
		*transform_source =
			array_iter_next(&iter, transform_source_isnull, i,
							inp_typlen, inp_typbyval, inp_typalign);

		/* Apply the given expression to source element */
		values[i] = ExecEvalExpr(exprstate, econtext, &nulls[i]);

		if (nulls[i])
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
	}

	/* Allocate and fill the result array */
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
	result = (ArrayType *) palloc0(nbytes);
	SET_VARSIZE(result, nbytes);
	result->ndim = ndim;
	result->dataoffset = dataoffset;
	result->elemtype = retType;
	memcpy(ARR_DIMS(result), AARR_DIMS(v), ndim * sizeof(int));
	memcpy(ARR_LBOUND(result), AARR_LBOUND(v), ndim * sizeof(int));

	CopyArrayEls(result,
				 values, nulls, nitems,
				 typlen, typbyval, typalign,
				 false);

	/*
	 * Note: do not risk trying to pfree the results of the called expression
	 */
	pfree(values);
	pfree(nulls);

	return PointerGetDatum(result);
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
 * Also note the result will be 0-D not 1-D if nelems = 0.
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
 * Also note the result will be 0-D not ndims-D if any dims[i] = 0.
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

	/* This checks for overflow of the array dimensions */
	nelems = ArrayGetNItems(ndims, dims);
	ArrayCheckBounds(ndims, dims, lbs);

	/* if ndims <= 0 or any dims[i] == 0, return empty array */
	if (nelems <= 0)
		return construct_empty_array(elmtype);

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
	result = (ArrayType *) palloc0(nbytes);
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

	result = (ArrayType *) palloc0(sizeof(ArrayType));
	SET_VARSIZE(result, sizeof(ArrayType));
	result->ndim = 0;
	result->dataoffset = 0;
	result->elemtype = elmtype;
	return result;
}

/*
 * construct_empty_expanded_array: make an empty expanded array
 * given only type information.  (metacache can be NULL if not needed.)
 */
ExpandedArrayHeader *
construct_empty_expanded_array(Oid element_type,
							   MemoryContext parentcontext,
							   ArrayMetaState *metacache)
{
	ArrayType  *array = construct_empty_array(element_type);
	Datum		d;

	d = expand_array(PointerGetDatum(array), parentcontext, metacache);
	pfree(array);
	return (ExpandedArrayHeader *) DatumGetEOHP(d);
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
		*nullsp = nulls = (bool *) palloc0(nelems * sizeof(bool));
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
 * array_contains_nulls --- detect whether an array has any null elements
 *
 * This gives an accurate answer, whereas testing ARR_HASNULL only tells
 * if the array *might* contain a null.
 */
bool
array_contains_nulls(ArrayType *array)
{
	int			nelems;
	bits8	   *bitmap;
	int			bitmask;

	/* Easy answer if there's no null bitmap */
	if (!ARR_HASNULL(array))
		return false;

	nelems = ArrayGetNItems(ARR_NDIM(array), ARR_DIMS(array));

	bitmap = ARR_NULLBITMAP(array);

	/* check whole bytes of the bitmap byte-at-a-time */
	while (nelems >= 8)
	{
		if (*bitmap != 0xFF)
			return true;
		bitmap++;
		nelems -= 8;
	}

	/* check last partial byte */
	bitmask = 1;
	while (nelems > 0)
	{
		if ((*bitmap & bitmask) == 0)
			return true;
		bitmask <<= 1;
		nelems--;
	}

	return false;
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
	LOCAL_FCINFO(locfcinfo, 2);
	AnyArrayType *array1 = PG_GETARG_ANY_ARRAY_P(0);
	AnyArrayType *array2 = PG_GETARG_ANY_ARRAY_P(1);
	Oid			collation = PG_GET_COLLATION();
	int			ndims1 = AARR_NDIM(array1);
	int			ndims2 = AARR_NDIM(array2);
	int		   *dims1 = AARR_DIMS(array1);
	int		   *dims2 = AARR_DIMS(array2);
	int		   *lbs1 = AARR_LBOUND(array1);
	int		   *lbs2 = AARR_LBOUND(array2);
	Oid			element_type = AARR_ELEMTYPE(array1);
	bool		result = true;
	int			nitems;
	TypeCacheEntry *typentry;
	int			typlen;
	bool		typbyval;
	char		typalign;
	array_iter	it1;
	array_iter	it2;
	int			i;

	if (element_type != AARR_ELEMTYPE(array2))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot compare arrays of different element types")));

	/* fast path if the arrays do not have the same dimensionality */
	if (ndims1 != ndims2 ||
		memcmp(dims1, dims2, ndims1 * sizeof(int)) != 0 ||
		memcmp(lbs1, lbs2, ndims1 * sizeof(int)) != 0)
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
		InitFunctionCallInfoData(*locfcinfo, &typentry->eq_opr_finfo, 2,
								 collation, NULL, NULL);

		/* Loop over source data */
		nitems = ArrayGetNItems(ndims1, dims1);
		array_iter_setup(&it1, array1);
		array_iter_setup(&it2, array2);

		for (i = 0; i < nitems; i++)
		{
			Datum		elt1;
			Datum		elt2;
			bool		isnull1;
			bool		isnull2;
			bool		oprresult;

			/* Get elements, checking for NULL */
			elt1 = array_iter_next(&it1, &isnull1, i,
								   typlen, typbyval, typalign);
			elt2 = array_iter_next(&it2, &isnull2, i,
								   typlen, typbyval, typalign);

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
			 * Apply the operator to the element pair; treat NULL as false
			 */
			locfcinfo->args[0].value = elt1;
			locfcinfo->args[0].isnull = false;
			locfcinfo->args[1].value = elt2;
			locfcinfo->args[1].isnull = false;
			locfcinfo->isnull = false;
			oprresult = DatumGetBool(FunctionCallInvoke(locfcinfo));
			if (locfcinfo->isnull || !oprresult)
			{
				result = false;
				break;
			}
		}
	}

	/* Avoid leaking memory when handed toasted input. */
	AARR_FREE_IF_COPY(array1, 0);
	AARR_FREE_IF_COPY(array2, 1);

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
	LOCAL_FCINFO(locfcinfo, 2);
	AnyArrayType *array1 = PG_GETARG_ANY_ARRAY_P(0);
	AnyArrayType *array2 = PG_GETARG_ANY_ARRAY_P(1);
	Oid			collation = PG_GET_COLLATION();
	int			ndims1 = AARR_NDIM(array1);
	int			ndims2 = AARR_NDIM(array2);
	int		   *dims1 = AARR_DIMS(array1);
	int		   *dims2 = AARR_DIMS(array2);
	int			nitems1 = ArrayGetNItems(ndims1, dims1);
	int			nitems2 = ArrayGetNItems(ndims2, dims2);
	Oid			element_type = AARR_ELEMTYPE(array1);
	int			result = 0;
	TypeCacheEntry *typentry;
	int			typlen;
	bool		typbyval;
	char		typalign;
	int			min_nitems;
	array_iter	it1;
	array_iter	it2;
	int			i;

	if (element_type != AARR_ELEMTYPE(array2))
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
	InitFunctionCallInfoData(*locfcinfo, &typentry->cmp_proc_finfo, 2,
							 collation, NULL, NULL);

	/* Loop over source data */
	min_nitems = Min(nitems1, nitems2);
	array_iter_setup(&it1, array1);
	array_iter_setup(&it2, array2);

	for (i = 0; i < min_nitems; i++)
	{
		Datum		elt1;
		Datum		elt2;
		bool		isnull1;
		bool		isnull2;
		int32		cmpresult;

		/* Get elements, checking for NULL */
		elt1 = array_iter_next(&it1, &isnull1, i, typlen, typbyval, typalign);
		elt2 = array_iter_next(&it2, &isnull2, i, typlen, typbyval, typalign);

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
		locfcinfo->args[0].value = elt1;
		locfcinfo->args[0].isnull = false;
		locfcinfo->args[1].value = elt2;
		locfcinfo->args[1].isnull = false;
		cmpresult = DatumGetInt32(FunctionCallInvoke(locfcinfo));

		/* We don't expect comparison support functions to return null */
		Assert(!locfcinfo->isnull);

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
	 * additional rules to sort by dimensionality.  The relative significance
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
			for (i = 0; i < ndims1; i++)
			{
				if (dims1[i] != dims2[i])
				{
					result = (dims1[i] < dims2[i]) ? -1 : 1;
					break;
				}
			}
			if (result == 0)
			{
				int		   *lbound1 = AARR_LBOUND(array1);
				int		   *lbound2 = AARR_LBOUND(array2);

				for (i = 0; i < ndims1; i++)
				{
					if (lbound1[i] != lbound2[i])
					{
						result = (lbound1[i] < lbound2[i]) ? -1 : 1;
						break;
					}
				}
			}
		}
	}

	/* Avoid leaking memory when handed toasted input. */
	AARR_FREE_IF_COPY(array1, 0);
	AARR_FREE_IF_COPY(array2, 1);

	return result;
}


/*-----------------------------------------------------------------------------
 * array hashing
 *		Hash the elements and combine the results.
 *----------------------------------------------------------------------------
 */

Datum
hash_array(PG_FUNCTION_ARGS)
{
	LOCAL_FCINFO(locfcinfo, 1);
	AnyArrayType *array = PG_GETARG_ANY_ARRAY_P(0);
	int			ndims = AARR_NDIM(array);
	int		   *dims = AARR_DIMS(array);
	Oid			element_type = AARR_ELEMTYPE(array);
	uint32		result = 1;
	int			nitems;
	TypeCacheEntry *typentry;
	int			typlen;
	bool		typbyval;
	char		typalign;
	int			i;
	array_iter	iter;

	/*
	 * We arrange to look up the hash function only once per series of calls,
	 * assuming the element type doesn't change underneath us.  The typcache
	 * is used so that we have no memory leakage when being used as an index
	 * support function.
	 */
	typentry = (TypeCacheEntry *) fcinfo->flinfo->fn_extra;
	if (typentry == NULL ||
		typentry->type_id != element_type)
	{
		typentry = lookup_type_cache(element_type,
									 TYPECACHE_HASH_PROC_FINFO);
		if (!OidIsValid(typentry->hash_proc_finfo.fn_oid) && element_type != RECORDOID)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("could not identify a hash function for type %s",
							format_type_be(element_type))));

		/*
		 * The type cache doesn't believe that record is hashable (see
		 * cache_record_field_properties()), but since we're here, we're
		 * committed to hashing, so we can assume it does.  Worst case, if any
		 * components of the record don't support hashing, we will fail at
		 * execution.
		 */
		if (element_type == RECORDOID)
		{
			MemoryContext oldcontext;
			TypeCacheEntry *record_typentry;

			oldcontext = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);

			/*
			 * Make fake type cache entry structure.  Note that we can't just
			 * modify typentry, since that points directly into the type
			 * cache.
			 */
			record_typentry = palloc0(sizeof(*record_typentry));
			record_typentry->type_id = element_type;

			/* fill in what we need below */
			record_typentry->typlen = typentry->typlen;
			record_typentry->typbyval = typentry->typbyval;
			record_typentry->typalign = typentry->typalign;
			fmgr_info(F_HASH_RECORD, &record_typentry->hash_proc_finfo);

			MemoryContextSwitchTo(oldcontext);

			typentry = record_typentry;
		}

		fcinfo->flinfo->fn_extra = (void *) typentry;
	}

	typlen = typentry->typlen;
	typbyval = typentry->typbyval;
	typalign = typentry->typalign;

	/*
	 * apply the hash function to each array element.
	 */
	InitFunctionCallInfoData(*locfcinfo, &typentry->hash_proc_finfo, 1,
							 PG_GET_COLLATION(), NULL, NULL);

	/* Loop over source data */
	nitems = ArrayGetNItems(ndims, dims);
	array_iter_setup(&iter, array);

	for (i = 0; i < nitems; i++)
	{
		Datum		elt;
		bool		isnull;
		uint32		elthash;

		/* Get element, checking for NULL */
		elt = array_iter_next(&iter, &isnull, i, typlen, typbyval, typalign);

		if (isnull)
		{
			/* Treat nulls as having hashvalue 0 */
			elthash = 0;
		}
		else
		{
			/* Apply the hash function */
			locfcinfo->args[0].value = elt;
			locfcinfo->args[0].isnull = false;
			elthash = DatumGetUInt32(FunctionCallInvoke(locfcinfo));
			/* We don't expect hash functions to return null */
			Assert(!locfcinfo->isnull);
		}

		/*
		 * Combine hash values of successive elements by multiplying the
		 * current value by 31 and adding on the new element's hash value.
		 *
		 * The result is a sum in which each element's hash value is
		 * multiplied by a different power of 31. This is modulo 2^32
		 * arithmetic, and the powers of 31 modulo 2^32 form a cyclic group of
		 * order 2^27. So for arrays of up to 2^27 elements, each element's
		 * hash value is multiplied by a different (odd) number, resulting in
		 * a good mixing of all the elements' hash values.
		 */
		result = (result << 5) - result + elthash;
	}

	/* Avoid leaking memory when handed toasted input. */
	AARR_FREE_IF_COPY(array, 0);

	PG_RETURN_UINT32(result);
}

/*
 * Returns 64-bit value by hashing a value to a 64-bit value, with a seed.
 * Otherwise, similar to hash_array.
 */
Datum
hash_array_extended(PG_FUNCTION_ARGS)
{
	LOCAL_FCINFO(locfcinfo, 2);
	AnyArrayType *array = PG_GETARG_ANY_ARRAY_P(0);
	uint64		seed = PG_GETARG_INT64(1);
	int			ndims = AARR_NDIM(array);
	int		   *dims = AARR_DIMS(array);
	Oid			element_type = AARR_ELEMTYPE(array);
	uint64		result = 1;
	int			nitems;
	TypeCacheEntry *typentry;
	int			typlen;
	bool		typbyval;
	char		typalign;
	int			i;
	array_iter	iter;

	typentry = (TypeCacheEntry *) fcinfo->flinfo->fn_extra;
	if (typentry == NULL ||
		typentry->type_id != element_type)
	{
		typentry = lookup_type_cache(element_type,
									 TYPECACHE_HASH_EXTENDED_PROC_FINFO);
		if (!OidIsValid(typentry->hash_extended_proc_finfo.fn_oid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("could not identify an extended hash function for type %s",
							format_type_be(element_type))));
		fcinfo->flinfo->fn_extra = (void *) typentry;
	}
	typlen = typentry->typlen;
	typbyval = typentry->typbyval;
	typalign = typentry->typalign;

	InitFunctionCallInfoData(*locfcinfo, &typentry->hash_extended_proc_finfo, 2,
							 PG_GET_COLLATION(), NULL, NULL);

	/* Loop over source data */
	nitems = ArrayGetNItems(ndims, dims);
	array_iter_setup(&iter, array);

	for (i = 0; i < nitems; i++)
	{
		Datum		elt;
		bool		isnull;
		uint64		elthash;

		/* Get element, checking for NULL */
		elt = array_iter_next(&iter, &isnull, i, typlen, typbyval, typalign);

		if (isnull)
		{
			elthash = 0;
		}
		else
		{
			/* Apply the hash function */
			locfcinfo->args[0].value = elt;
			locfcinfo->args[0].isnull = false;
			locfcinfo->args[1].value = Int64GetDatum(seed);
			locfcinfo->args[1].isnull = false;
			elthash = DatumGetUInt64(FunctionCallInvoke(locfcinfo));
			/* We don't expect hash functions to return null */
			Assert(!locfcinfo->isnull);
		}

		result = (result << 5) - result + elthash;
	}

	AARR_FREE_IF_COPY(array, 0);

	PG_RETURN_UINT64(result);
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
array_contain_compare(AnyArrayType *array1, AnyArrayType *array2, Oid collation,
					  bool matchall, void **fn_extra)
{
	LOCAL_FCINFO(locfcinfo, 2);
	bool		result = matchall;
	Oid			element_type = AARR_ELEMTYPE(array1);
	TypeCacheEntry *typentry;
	int			nelems1;
	Datum	   *values2;
	bool	   *nulls2;
	int			nelems2;
	int			typlen;
	bool		typbyval;
	char		typalign;
	int			i;
	int			j;
	array_iter	it1;

	if (element_type != AARR_ELEMTYPE(array2))
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
	if (VARATT_IS_EXPANDED_HEADER(array2))
	{
		/* This should be safe even if input is read-only */
		deconstruct_expanded_array(&(array2->xpn));
		values2 = array2->xpn.dvalues;
		nulls2 = array2->xpn.dnulls;
		nelems2 = array2->xpn.nelems;
	}
	else
		deconstruct_array((ArrayType *) array2,
						  element_type, typlen, typbyval, typalign,
						  &values2, &nulls2, &nelems2);

	/*
	 * Apply the comparison operator to each pair of array elements.
	 */
	InitFunctionCallInfoData(*locfcinfo, &typentry->eq_opr_finfo, 2,
							 collation, NULL, NULL);

	/* Loop over source data */
	nelems1 = ArrayGetNItems(AARR_NDIM(array1), AARR_DIMS(array1));
	array_iter_setup(&it1, array1);

	for (i = 0; i < nelems1; i++)
	{
		Datum		elt1;
		bool		isnull1;

		/* Get element, checking for NULL */
		elt1 = array_iter_next(&it1, &isnull1, i, typlen, typbyval, typalign);

		/*
		 * We assume that the comparison operator is strict, so a NULL can't
		 * match anything.  XXX this diverges from the "NULL=NULL" behavior of
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
			bool		isnull2 = nulls2 ? nulls2[j] : false;
			bool		oprresult;

			if (isnull2)
				continue;		/* can't match */

			/*
			 * Apply the operator to the element pair; treat NULL as false
			 */
			locfcinfo->args[0].value = elt1;
			locfcinfo->args[0].isnull = false;
			locfcinfo->args[1].value = elt2;
			locfcinfo->args[1].isnull = false;
			locfcinfo->isnull = false;
			oprresult = DatumGetBool(FunctionCallInvoke(locfcinfo));
			if (!locfcinfo->isnull && oprresult)
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

	return result;
}

Datum
arrayoverlap(PG_FUNCTION_ARGS)
{
	AnyArrayType *array1 = PG_GETARG_ANY_ARRAY_P(0);
	AnyArrayType *array2 = PG_GETARG_ANY_ARRAY_P(1);
	Oid			collation = PG_GET_COLLATION();
	bool		result;

	result = array_contain_compare(array1, array2, collation, false,
								   &fcinfo->flinfo->fn_extra);

	/* Avoid leaking memory when handed toasted input. */
	AARR_FREE_IF_COPY(array1, 0);
	AARR_FREE_IF_COPY(array2, 1);

	PG_RETURN_BOOL(result);
}

Datum
arraycontains(PG_FUNCTION_ARGS)
{
	AnyArrayType *array1 = PG_GETARG_ANY_ARRAY_P(0);
	AnyArrayType *array2 = PG_GETARG_ANY_ARRAY_P(1);
	Oid			collation = PG_GET_COLLATION();
	bool		result;

	result = array_contain_compare(array2, array1, collation, true,
								   &fcinfo->flinfo->fn_extra);

	/* Avoid leaking memory when handed toasted input. */
	AARR_FREE_IF_COPY(array1, 0);
	AARR_FREE_IF_COPY(array2, 1);

	PG_RETURN_BOOL(result);
}

Datum
arraycontained(PG_FUNCTION_ARGS)
{
	AnyArrayType *array1 = PG_GETARG_ANY_ARRAY_P(0);
	AnyArrayType *array2 = PG_GETARG_ANY_ARRAY_P(1);
	Oid			collation = PG_GET_COLLATION();
	bool		result;

	result = array_contain_compare(array1, array2, collation, true,
								   &fcinfo->flinfo->fn_extra);

	/* Avoid leaking memory when handed toasted input. */
	AARR_FREE_IF_COPY(array1, 0);
	AARR_FREE_IF_COPY(array2, 1);

	PG_RETURN_BOOL(result);
}


/*-----------------------------------------------------------------------------
 * Array iteration functions
 *		These functions are used to iterate efficiently through arrays
 *-----------------------------------------------------------------------------
 */

/*
 * array_create_iterator --- set up to iterate through an array
 *
 * If slice_ndim is zero, we will iterate element-by-element; the returned
 * datums are of the array's element type.
 *
 * If slice_ndim is 1..ARR_NDIM(arr), we will iterate by slices: the
 * returned datums are of the same array type as 'arr', but of size
 * equal to the rightmost N dimensions of 'arr'.
 *
 * The passed-in array must remain valid for the lifetime of the iterator.
 */
ArrayIterator
array_create_iterator(ArrayType *arr, int slice_ndim, ArrayMetaState *mstate)
{
	ArrayIterator iterator = palloc0(sizeof(ArrayIteratorData));

	/*
	 * Sanity-check inputs --- caller should have got this right already
	 */
	Assert(PointerIsValid(arr));
	if (slice_ndim < 0 || slice_ndim > ARR_NDIM(arr))
		elog(ERROR, "invalid arguments to array_create_iterator");

	/*
	 * Remember basic info about the array and its element type
	 */
	iterator->arr = arr;
	iterator->nullbitmap = ARR_NULLBITMAP(arr);
	iterator->nitems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));

	if (mstate != NULL)
	{
		Assert(mstate->element_type == ARR_ELEMTYPE(arr));

		iterator->typlen = mstate->typlen;
		iterator->typbyval = mstate->typbyval;
		iterator->typalign = mstate->typalign;
	}
	else
		get_typlenbyvalalign(ARR_ELEMTYPE(arr),
							 &iterator->typlen,
							 &iterator->typbyval,
							 &iterator->typalign);

	/*
	 * Remember the slicing parameters.
	 */
	iterator->slice_ndim = slice_ndim;

	if (slice_ndim > 0)
	{
		/*
		 * Get pointers into the array's dims and lbound arrays to represent
		 * the dims/lbound arrays of a slice.  These are the same as the
		 * rightmost N dimensions of the array.
		 */
		iterator->slice_dims = ARR_DIMS(arr) + ARR_NDIM(arr) - slice_ndim;
		iterator->slice_lbound = ARR_LBOUND(arr) + ARR_NDIM(arr) - slice_ndim;

		/*
		 * Compute number of elements in a slice.
		 */
		iterator->slice_len = ArrayGetNItems(slice_ndim,
											 iterator->slice_dims);

		/*
		 * Create workspace for building sub-arrays.
		 */
		iterator->slice_values = (Datum *)
			palloc(iterator->slice_len * sizeof(Datum));
		iterator->slice_nulls = (bool *)
			palloc(iterator->slice_len * sizeof(bool));
	}

	/*
	 * Initialize our data pointer and linear element number.  These will
	 * advance through the array during array_iterate().
	 */
	iterator->data_ptr = ARR_DATA_PTR(arr);
	iterator->current_item = 0;

	return iterator;
}

/*
 * Iterate through the array referenced by 'iterator'.
 *
 * As long as there is another element (or slice), return it into
 * *value / *isnull, and return true.  Return false when no more data.
 */
bool
array_iterate(ArrayIterator iterator, Datum *value, bool *isnull)
{
	/* Done if we have reached the end of the array */
	if (iterator->current_item >= iterator->nitems)
		return false;

	if (iterator->slice_ndim == 0)
	{
		/*
		 * Scalar case: return one element.
		 */
		if (array_get_isnull(iterator->nullbitmap, iterator->current_item++))
		{
			*isnull = true;
			*value = (Datum) 0;
		}
		else
		{
			/* non-NULL, so fetch the individual Datum to return */
			char	   *p = iterator->data_ptr;

			*isnull = false;
			*value = fetch_att(p, iterator->typbyval, iterator->typlen);

			/* Move our data pointer forward to the next element */
			p = att_addlength_pointer(p, iterator->typlen, p);
			p = (char *) att_align_nominal(p, iterator->typalign);
			iterator->data_ptr = p;
		}
	}
	else
	{
		/*
		 * Slice case: build and return an array of the requested size.
		 */
		ArrayType  *result;
		Datum	   *values = iterator->slice_values;
		bool	   *nulls = iterator->slice_nulls;
		char	   *p = iterator->data_ptr;
		int			i;

		for (i = 0; i < iterator->slice_len; i++)
		{
			if (array_get_isnull(iterator->nullbitmap,
								 iterator->current_item++))
			{
				nulls[i] = true;
				values[i] = (Datum) 0;
			}
			else
			{
				nulls[i] = false;
				values[i] = fetch_att(p, iterator->typbyval, iterator->typlen);

				/* Move our data pointer forward to the next element */
				p = att_addlength_pointer(p, iterator->typlen, p);
				p = (char *) att_align_nominal(p, iterator->typalign);
			}
		}

		iterator->data_ptr = p;

		result = construct_md_array(values,
									nulls,
									iterator->slice_ndim,
									iterator->slice_dims,
									iterator->slice_lbound,
									ARR_ELEMTYPE(iterator->arr),
									iterator->typlen,
									iterator->typbyval,
									iterator->typalign);

		*isnull = false;
		*value = PointerGetDatum(result);
	}

	return true;
}

/*
 * Release an ArrayIterator data structure
 */
void
array_free_iterator(ArrayIterator iterator)
{
	if (iterator->slice_ndim > 0)
	{
		pfree(iterator->slice_values);
		pfree(iterator->slice_nulls);
	}
	pfree(iterator);
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
 * to make it worth worrying too much.  For the moment, KISS.
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
 * those same dimensions is to be constructed.  destArray must already
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
 * initArrayResult - initialize an empty ArrayBuildState
 *
 *	element_type is the array element type (must be a valid array element type)
 *	rcontext is where to keep working state
 *	subcontext is a flag determining whether to use a separate memory context
 *
 * Note: there are two common schemes for using accumArrayResult().
 * In the older scheme, you start with a NULL ArrayBuildState pointer, and
 * call accumArrayResult once per element.  In this scheme you end up with
 * a NULL pointer if there were no elements, which you need to special-case.
 * In the newer scheme, call initArrayResult and then call accumArrayResult
 * once per element.  In this scheme you always end with a non-NULL pointer
 * that you can pass to makeArrayResult; you get an empty array if there
 * were no elements.  This is preferred if an empty array is what you want.
 *
 * It's possible to choose whether to create a separate memory context for the
 * array build state, or whether to allocate it directly within rcontext.
 *
 * When there are many concurrent small states (e.g. array_agg() using hash
 * aggregation of many small groups), using a separate memory context for each
 * one may result in severe memory bloat. In such cases, use the same memory
 * context to initialize all such array build states, and pass
 * subcontext=false.
 *
 * In cases when the array build states have different lifetimes, using a
 * single memory context is impractical. Instead, pass subcontext=true so that
 * the array build states can be freed individually.
 */
ArrayBuildState *
initArrayResult(Oid element_type, MemoryContext rcontext, bool subcontext)
{
	ArrayBuildState *astate;
	MemoryContext arr_context = rcontext;

	/* Make a temporary context to hold all the junk */
	if (subcontext)
		arr_context = AllocSetContextCreate(rcontext,
											"accumArrayResult",
											ALLOCSET_DEFAULT_SIZES);

	astate = (ArrayBuildState *)
		MemoryContextAlloc(arr_context, sizeof(ArrayBuildState));
	astate->mcontext = arr_context;
	astate->private_cxt = subcontext;
	astate->alen = (subcontext ? 64 : 8);	/* arbitrary starting array size */
	astate->dvalues = (Datum *)
		MemoryContextAlloc(arr_context, astate->alen * sizeof(Datum));
	astate->dnulls = (bool *)
		MemoryContextAlloc(arr_context, astate->alen * sizeof(bool));
	astate->nelems = 0;
	astate->element_type = element_type;
	get_typlenbyvalalign(element_type,
						 &astate->typlen,
						 &astate->typbyval,
						 &astate->typalign);

	return astate;
}

/*
 * accumArrayResult - accumulate one (more) Datum for an array result
 *
 *	astate is working state (can be NULL on first call)
 *	dvalue/disnull represent the new Datum to append to the array
 *	element_type is the Datum's type (must be a valid array element type)
 *	rcontext is where to keep working state
 */
ArrayBuildState *
accumArrayResult(ArrayBuildState *astate,
				 Datum dvalue, bool disnull,
				 Oid element_type,
				 MemoryContext rcontext)
{
	MemoryContext oldcontext;

	if (astate == NULL)
	{
		/* First time through --- initialize */
		astate = initArrayResult(element_type, rcontext, true);
	}
	else
	{
		Assert(astate->element_type == element_type);
	}

	oldcontext = MemoryContextSwitchTo(astate->mcontext);

	/* enlarge dvalues[]/dnulls[] if needed */
	if (astate->nelems >= astate->alen)
	{
		astate->alen *= 2;
		astate->dvalues = (Datum *)
			repalloc(astate->dvalues, astate->alen * sizeof(Datum));
		astate->dnulls = (bool *)
			repalloc(astate->dnulls, astate->alen * sizeof(bool));
	}

	/*
	 * Ensure pass-by-ref stuff is copied into mcontext; and detoast it too if
	 * it's varlena.  (You might think that detoasting is not needed here
	 * because construct_md_array can detoast the array elements later.
	 * However, we must not let construct_md_array modify the ArrayBuildState
	 * because that would mean array_agg_finalfn damages its input, which is
	 * verboten.  Also, this way frequently saves one copying step.)
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
 * Note: only releases astate if it was initialized within a separate memory
 * context (i.e. using subcontext=true when calling initArrayResult).
 *
 *	astate is working state (must not be NULL)
 *	rcontext is where to construct result
 */
Datum
makeArrayResult(ArrayBuildState *astate,
				MemoryContext rcontext)
{
	int			ndims;
	int			dims[1];
	int			lbs[1];

	/* If no elements were presented, we want to create an empty array */
	ndims = (astate->nelems > 0) ? 1 : 0;
	dims[0] = astate->nelems;
	lbs[0] = 1;

	return makeMdArrayResult(astate, ndims, dims, lbs, rcontext,
							 astate->private_cxt);
}

/*
 * makeMdArrayResult - produce multi-D final result of accumArrayResult
 *
 * beware: no check that specified dimensions match the number of values
 * accumulated.
 *
 * Note: if the astate was not initialized within a separate memory context
 * (that is, initArrayResult was called with subcontext=false), then using
 * release=true is illegal. Instead, release astate along with the rest of its
 * context when appropriate.
 *
 *	astate is working state (must not be NULL)
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
	{
		Assert(astate->private_cxt);
		MemoryContextDelete(astate->mcontext);
	}

	return PointerGetDatum(result);
}

/*
 * The following three functions provide essentially the same API as
 * initArrayResult/accumArrayResult/makeArrayResult, but instead of accepting
 * inputs that are array elements, they accept inputs that are arrays and
 * produce an output array having N+1 dimensions.  The inputs must all have
 * identical dimensionality as well as element type.
 */

/*
 * initArrayResultArr - initialize an empty ArrayBuildStateArr
 *
 *	array_type is the array type (must be a valid varlena array type)
 *	element_type is the type of the array's elements (lookup if InvalidOid)
 *	rcontext is where to keep working state
 *	subcontext is a flag determining whether to use a separate memory context
 */
ArrayBuildStateArr *
initArrayResultArr(Oid array_type, Oid element_type, MemoryContext rcontext,
				   bool subcontext)
{
	ArrayBuildStateArr *astate;
	MemoryContext arr_context = rcontext;	/* by default use the parent ctx */

	/* Lookup element type, unless element_type already provided */
	if (!OidIsValid(element_type))
	{
		element_type = get_element_type(array_type);

		if (!OidIsValid(element_type))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("data type %s is not an array type",
							format_type_be(array_type))));
	}

	/* Make a temporary context to hold all the junk */
	if (subcontext)
		arr_context = AllocSetContextCreate(rcontext,
											"accumArrayResultArr",
											ALLOCSET_DEFAULT_SIZES);

	/* Note we initialize all fields to zero */
	astate = (ArrayBuildStateArr *)
		MemoryContextAllocZero(arr_context, sizeof(ArrayBuildStateArr));
	astate->mcontext = arr_context;
	astate->private_cxt = subcontext;

	/* Save relevant datatype information */
	astate->array_type = array_type;
	astate->element_type = element_type;

	return astate;
}

/*
 * accumArrayResultArr - accumulate one (more) sub-array for an array result
 *
 *	astate is working state (can be NULL on first call)
 *	dvalue/disnull represent the new sub-array to append to the array
 *	array_type is the array type (must be a valid varlena array type)
 *	rcontext is where to keep working state
 */
ArrayBuildStateArr *
accumArrayResultArr(ArrayBuildStateArr *astate,
					Datum dvalue, bool disnull,
					Oid array_type,
					MemoryContext rcontext)
{
	ArrayType  *arg;
	MemoryContext oldcontext;
	int		   *dims,
			   *lbs,
				ndims,
				nitems,
				ndatabytes;
	char	   *data;
	int			i;

	/*
	 * We disallow accumulating null subarrays.  Another plausible definition
	 * is to ignore them, but callers that want that can just skip calling
	 * this function.
	 */
	if (disnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("cannot accumulate null arrays")));

	/* Detoast input array in caller's context */
	arg = DatumGetArrayTypeP(dvalue);

	if (astate == NULL)
		astate = initArrayResultArr(array_type, InvalidOid, rcontext, true);
	else
		Assert(astate->array_type == array_type);

	oldcontext = MemoryContextSwitchTo(astate->mcontext);

	/* Collect this input's dimensions */
	ndims = ARR_NDIM(arg);
	dims = ARR_DIMS(arg);
	lbs = ARR_LBOUND(arg);
	data = ARR_DATA_PTR(arg);
	nitems = ArrayGetNItems(ndims, dims);
	ndatabytes = ARR_SIZE(arg) - ARR_DATA_OFFSET(arg);

	if (astate->ndims == 0)
	{
		/* First input; check/save the dimensionality info */

		/* Should we allow empty inputs and just produce an empty output? */
		if (ndims == 0)
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("cannot accumulate empty arrays")));
		if (ndims + 1 > MAXDIM)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
							ndims + 1, MAXDIM)));

		/*
		 * The output array will have n+1 dimensions, with the ones after the
		 * first matching the input's dimensions.
		 */
		astate->ndims = ndims + 1;
		astate->dims[0] = 0;
		memcpy(&astate->dims[1], dims, ndims * sizeof(int));
		astate->lbs[0] = 1;
		memcpy(&astate->lbs[1], lbs, ndims * sizeof(int));

		/* Allocate at least enough data space for this item */
		astate->abytes = pg_nextpower2_32(Max(1024, ndatabytes + 1));
		astate->data = (char *) palloc(astate->abytes);
	}
	else
	{
		/* Second or later input: must match first input's dimensionality */
		if (astate->ndims != ndims + 1)
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("cannot accumulate arrays of different dimensionality")));
		for (i = 0; i < ndims; i++)
		{
			if (astate->dims[i + 1] != dims[i] || astate->lbs[i + 1] != lbs[i])
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("cannot accumulate arrays of different dimensionality")));
		}

		/* Enlarge data space if needed */
		if (astate->nbytes + ndatabytes > astate->abytes)
		{
			astate->abytes = Max(astate->abytes * 2,
								 astate->nbytes + ndatabytes);
			astate->data = (char *) repalloc(astate->data, astate->abytes);
		}
	}

	/*
	 * Copy the data portion of the sub-array.  Note we assume that the
	 * advertised data length of the sub-array is properly aligned.  We do not
	 * have to worry about detoasting elements since whatever's in the
	 * sub-array should be OK already.
	 */
	memcpy(astate->data + astate->nbytes, data, ndatabytes);
	astate->nbytes += ndatabytes;

	/* Deal with null bitmap if needed */
	if (astate->nullbitmap || ARR_HASNULL(arg))
	{
		int			newnitems = astate->nitems + nitems;

		if (astate->nullbitmap == NULL)
		{
			/*
			 * First input with nulls; we must retrospectively handle any
			 * previous inputs by marking all their items non-null.
			 */
			astate->aitems = pg_nextpower2_32(Max(256, newnitems + 1));
			astate->nullbitmap = (bits8 *) palloc((astate->aitems + 7) / 8);
			array_bitmap_copy(astate->nullbitmap, 0,
							  NULL, 0,
							  astate->nitems);
		}
		else if (newnitems > astate->aitems)
		{
			astate->aitems = Max(astate->aitems * 2, newnitems);
			astate->nullbitmap = (bits8 *)
				repalloc(astate->nullbitmap, (astate->aitems + 7) / 8);
		}
		array_bitmap_copy(astate->nullbitmap, astate->nitems,
						  ARR_NULLBITMAP(arg), 0,
						  nitems);
	}

	astate->nitems += nitems;
	astate->dims[0] += 1;

	MemoryContextSwitchTo(oldcontext);

	/* Release detoasted copy if any */
	if ((Pointer) arg != DatumGetPointer(dvalue))
		pfree(arg);

	return astate;
}

/*
 * makeArrayResultArr - produce N+1-D final result of accumArrayResultArr
 *
 *	astate is working state (must not be NULL)
 *	rcontext is where to construct result
 *	release is true if okay to release working state
 */
Datum
makeArrayResultArr(ArrayBuildStateArr *astate,
				   MemoryContext rcontext,
				   bool release)
{
	ArrayType  *result;
	MemoryContext oldcontext;

	/* Build the final array result in rcontext */
	oldcontext = MemoryContextSwitchTo(rcontext);

	if (astate->ndims == 0)
	{
		/* No inputs, return empty array */
		result = construct_empty_array(astate->element_type);
	}
	else
	{
		int			dataoffset,
					nbytes;

		/* Check for overflow of the array dimensions */
		(void) ArrayGetNItems(astate->ndims, astate->dims);
		ArrayCheckBounds(astate->ndims, astate->dims, astate->lbs);

		/* Compute required space */
		nbytes = astate->nbytes;
		if (astate->nullbitmap != NULL)
		{
			dataoffset = ARR_OVERHEAD_WITHNULLS(astate->ndims, astate->nitems);
			nbytes += dataoffset;
		}
		else
		{
			dataoffset = 0;
			nbytes += ARR_OVERHEAD_NONULLS(astate->ndims);
		}

		result = (ArrayType *) palloc0(nbytes);
		SET_VARSIZE(result, nbytes);
		result->ndim = astate->ndims;
		result->dataoffset = dataoffset;
		result->elemtype = astate->element_type;

		memcpy(ARR_DIMS(result), astate->dims, astate->ndims * sizeof(int));
		memcpy(ARR_LBOUND(result), astate->lbs, astate->ndims * sizeof(int));
		memcpy(ARR_DATA_PTR(result), astate->data, astate->nbytes);

		if (astate->nullbitmap != NULL)
			array_bitmap_copy(ARR_NULLBITMAP(result), 0,
							  astate->nullbitmap, 0,
							  astate->nitems);
	}

	MemoryContextSwitchTo(oldcontext);

	/* Clean up all the junk */
	if (release)
	{
		Assert(astate->private_cxt);
		MemoryContextDelete(astate->mcontext);
	}

	return PointerGetDatum(result);
}

/*
 * The following three functions provide essentially the same API as
 * initArrayResult/accumArrayResult/makeArrayResult, but can accept either
 * scalar or array inputs, invoking the appropriate set of functions above.
 */

/*
 * initArrayResultAny - initialize an empty ArrayBuildStateAny
 *
 *	input_type is the input datatype (either element or array type)
 *	rcontext is where to keep working state
 *	subcontext is a flag determining whether to use a separate memory context
 */
ArrayBuildStateAny *
initArrayResultAny(Oid input_type, MemoryContext rcontext, bool subcontext)
{
	ArrayBuildStateAny *astate;
	Oid			element_type = get_element_type(input_type);

	if (OidIsValid(element_type))
	{
		/* Array case */
		ArrayBuildStateArr *arraystate;

		arraystate = initArrayResultArr(input_type, InvalidOid, rcontext, subcontext);
		astate = (ArrayBuildStateAny *)
			MemoryContextAlloc(arraystate->mcontext,
							   sizeof(ArrayBuildStateAny));
		astate->scalarstate = NULL;
		astate->arraystate = arraystate;
	}
	else
	{
		/* Scalar case */
		ArrayBuildState *scalarstate;

		/* Let's just check that we have a type that can be put into arrays */
		Assert(OidIsValid(get_array_type(input_type)));

		scalarstate = initArrayResult(input_type, rcontext, subcontext);
		astate = (ArrayBuildStateAny *)
			MemoryContextAlloc(scalarstate->mcontext,
							   sizeof(ArrayBuildStateAny));
		astate->scalarstate = scalarstate;
		astate->arraystate = NULL;
	}

	return astate;
}

/*
 * accumArrayResultAny - accumulate one (more) input for an array result
 *
 *	astate is working state (can be NULL on first call)
 *	dvalue/disnull represent the new input to append to the array
 *	input_type is the input datatype (either element or array type)
 *	rcontext is where to keep working state
 */
ArrayBuildStateAny *
accumArrayResultAny(ArrayBuildStateAny *astate,
					Datum dvalue, bool disnull,
					Oid input_type,
					MemoryContext rcontext)
{
	if (astate == NULL)
		astate = initArrayResultAny(input_type, rcontext, true);

	if (astate->scalarstate)
		(void) accumArrayResult(astate->scalarstate,
								dvalue, disnull,
								input_type, rcontext);
	else
		(void) accumArrayResultArr(astate->arraystate,
								   dvalue, disnull,
								   input_type, rcontext);

	return astate;
}

/*
 * makeArrayResultAny - produce final result of accumArrayResultAny
 *
 *	astate is working state (must not be NULL)
 *	rcontext is where to construct result
 *	release is true if okay to release working state
 */
Datum
makeArrayResultAny(ArrayBuildStateAny *astate,
				   MemoryContext rcontext, bool release)
{
	Datum		result;

	if (astate->scalarstate)
	{
		/* Must use makeMdArrayResult to support "release" parameter */
		int			ndims;
		int			dims[1];
		int			lbs[1];

		/* If no elements were presented, we want to create an empty array */
		ndims = (astate->scalarstate->nelems > 0) ? 1 : 0;
		dims[0] = astate->scalarstate->nelems;
		lbs[0] = 1;

		result = makeMdArrayResult(astate->scalarstate, ndims, dims, lbs,
								   rcontext, release);
	}
	else
	{
		result = makeArrayResultArr(astate->arraystate,
									rcontext, release);
	}
	return result;
}


Datum
array_larger(PG_FUNCTION_ARGS)
{
	if (array_cmp(fcinfo) > 0)
		PG_RETURN_DATUM(PG_GETARG_DATUM(0));
	else
		PG_RETURN_DATUM(PG_GETARG_DATUM(1));
}

Datum
array_smaller(PG_FUNCTION_ARGS)
{
	if (array_cmp(fcinfo) < 0)
		PG_RETURN_DATUM(PG_GETARG_DATUM(0));
	else
		PG_RETURN_DATUM(PG_GETARG_DATUM(1));
}


typedef struct generate_subscripts_fctx
{
	int32		lower;
	int32		upper;
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
		AnyArrayType *v = PG_GETARG_ANY_ARRAY_P(0);
		int			reqdim = PG_GETARG_INT32(1);
		int		   *lb,
				   *dimv;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/* Sanity check: does it look like an array at all? */
		if (AARR_NDIM(v) <= 0 || AARR_NDIM(v) > MAXDIM)
			SRF_RETURN_DONE(funcctx);

		/* Sanity check: was the requested dim valid */
		if (reqdim <= 0 || reqdim > AARR_NDIM(v))
			SRF_RETURN_DONE(funcctx);

		/*
		 * switch to memory context appropriate for multiple function calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		fctx = (generate_subscripts_fctx *) palloc(sizeof(generate_subscripts_fctx));

		lb = AARR_LBOUND(v);
		dimv = AARR_DIMS(v);

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
				 errmsg("dimension array or low bound array cannot be null")));

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
				 errmsg("dimension array or low bound array cannot be null")));

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
	if (ARR_NDIM(dims) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("wrong number of array subscripts"),
				 errdetail("Dimension array must be one dimensional.")));

	if (array_contains_nulls(dims))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("dimension values cannot be null")));

	dimv = (int *) ARR_DATA_PTR(dims);
	ndims = (ARR_NDIM(dims) > 0) ? ARR_DIMS(dims)[0] : 0;

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
		if (ARR_NDIM(lbs) > 1)
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("wrong number of array subscripts"),
					 errdetail("Dimension array must be one dimensional.")));

		if (array_contains_nulls(lbs))
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("dimension values cannot be null")));

		if (ndims != ((ARR_NDIM(lbs) > 0) ? ARR_DIMS(lbs)[0] : 0))
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

	/* This checks for overflow of the array dimensions */
	nitems = ArrayGetNItems(ndims, dimv);
	ArrayCheckBounds(ndims, dimv, lbsv);

	/* fast track for empty array */
	if (nitems <= 0)
		return construct_empty_array(elmtype);

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
		array_iter	iter;
		int			nextelem;
		int			numelems;
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
		AnyArrayType *arr;

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
		 * and not before.  (If no detoast happens, we assume the originally
		 * passed array will stick around till then.)
		 */
		arr = PG_GETARG_ANY_ARRAY_P(0);

		/* allocate memory for user context */
		fctx = (array_unnest_fctx *) palloc(sizeof(array_unnest_fctx));

		/* initialize state */
		array_iter_setup(&fctx->iter, arr);
		fctx->nextelem = 0;
		fctx->numelems = ArrayGetNItems(AARR_NDIM(arr), AARR_DIMS(arr));

		if (VARATT_IS_EXPANDED_HEADER(arr))
		{
			/* we can just grab the type data from expanded array */
			fctx->elmlen = arr->xpn.typlen;
			fctx->elmbyval = arr->xpn.typbyval;
			fctx->elmalign = arr->xpn.typalign;
		}
		else
			get_typlenbyvalalign(AARR_ELEMTYPE(arr),
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

		elem = array_iter_next(&fctx->iter, &fcinfo->isnull, offset,
							   fctx->elmlen, fctx->elmbyval, fctx->elmalign);

		SRF_RETURN_NEXT(funcctx, elem);
	}
	else
	{
		/* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * Planner support function for array_unnest(anyarray)
 */
Datum
array_unnest_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestRows))
	{
		/* Try to estimate the number of rows returned */
		SupportRequestRows *req = (SupportRequestRows *) rawreq;

		if (is_funcclause(req->node))	/* be paranoid */
		{
			List	   *args = ((FuncExpr *) req->node)->args;
			Node	   *arg1;

			/* We can use estimated argument values here */
			arg1 = estimate_expression_value(req->root, linitial(args));

			req->rows = estimate_array_length(arg1);
			ret = (Node *) req;
		}
	}

	PG_RETURN_POINTER(ret);
}


/*
 * array_replace/array_remove support
 *
 * Find all array entries matching (not distinct from) search/search_isnull,
 * and delete them if remove is true, else replace them with
 * replace/replace_isnull.  Comparisons are done using the specified
 * collation.  fcinfo is passed only for caching purposes.
 */
static ArrayType *
array_replace_internal(ArrayType *array,
					   Datum search, bool search_isnull,
					   Datum replace, bool replace_isnull,
					   bool remove, Oid collation,
					   FunctionCallInfo fcinfo)
{
	LOCAL_FCINFO(locfcinfo, 2);
	ArrayType  *result;
	Oid			element_type;
	Datum	   *values;
	bool	   *nulls;
	int		   *dim;
	int			ndim;
	int			nitems,
				nresult;
	int			i;
	int32		nbytes = 0;
	int32		dataoffset;
	bool		hasnulls;
	int			typlen;
	bool		typbyval;
	char		typalign;
	char	   *arraydataptr;
	bits8	   *bitmap;
	int			bitmask;
	bool		changed = false;
	TypeCacheEntry *typentry;

	element_type = ARR_ELEMTYPE(array);
	ndim = ARR_NDIM(array);
	dim = ARR_DIMS(array);
	nitems = ArrayGetNItems(ndim, dim);

	/* Return input array unmodified if it is empty */
	if (nitems <= 0)
		return array;

	/*
	 * We can't remove elements from multi-dimensional arrays, since the
	 * result might not be rectangular.
	 */
	if (remove && ndim > 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("removing elements from multidimensional arrays is not supported")));

	/*
	 * We arrange to look up the equality function only once per series of
	 * calls, assuming the element type doesn't change underneath us.
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
	 * Detoast values if they are toasted.  The replacement value must be
	 * detoasted for insertion into the result array, while detoasting the
	 * search value only once saves cycles.
	 */
	if (typlen == -1)
	{
		if (!search_isnull)
			search = PointerGetDatum(PG_DETOAST_DATUM(search));
		if (!replace_isnull)
			replace = PointerGetDatum(PG_DETOAST_DATUM(replace));
	}

	/* Prepare to apply the comparison operator */
	InitFunctionCallInfoData(*locfcinfo, &typentry->eq_opr_finfo, 2,
							 collation, NULL, NULL);

	/* Allocate temporary arrays for new values */
	values = (Datum *) palloc(nitems * sizeof(Datum));
	nulls = (bool *) palloc(nitems * sizeof(bool));

	/* Loop over source data */
	arraydataptr = ARR_DATA_PTR(array);
	bitmap = ARR_NULLBITMAP(array);
	bitmask = 1;
	hasnulls = false;
	nresult = 0;

	for (i = 0; i < nitems; i++)
	{
		Datum		elt;
		bool		isNull;
		bool		oprresult;
		bool		skip = false;

		/* Get source element, checking for NULL */
		if (bitmap && (*bitmap & bitmask) == 0)
		{
			isNull = true;
			/* If searching for NULL, we have a match */
			if (search_isnull)
			{
				if (remove)
				{
					skip = true;
					changed = true;
				}
				else if (!replace_isnull)
				{
					values[nresult] = replace;
					isNull = false;
					changed = true;
				}
			}
		}
		else
		{
			isNull = false;
			elt = fetch_att(arraydataptr, typbyval, typlen);
			arraydataptr = att_addlength_datum(arraydataptr, typlen, elt);
			arraydataptr = (char *) att_align_nominal(arraydataptr, typalign);

			if (search_isnull)
			{
				/* no match possible, keep element */
				values[nresult] = elt;
			}
			else
			{
				/*
				 * Apply the operator to the element pair; treat NULL as false
				 */
				locfcinfo->args[0].value = elt;
				locfcinfo->args[0].isnull = false;
				locfcinfo->args[1].value = search;
				locfcinfo->args[1].isnull = false;
				locfcinfo->isnull = false;
				oprresult = DatumGetBool(FunctionCallInvoke(locfcinfo));
				if (locfcinfo->isnull || !oprresult)
				{
					/* no match, keep element */
					values[nresult] = elt;
				}
				else
				{
					/* match, so replace or delete */
					changed = true;
					if (remove)
						skip = true;
					else
					{
						values[nresult] = replace;
						isNull = replace_isnull;
					}
				}
			}
		}

		if (!skip)
		{
			nulls[nresult] = isNull;
			if (isNull)
				hasnulls = true;
			else
			{
				/* Update total result size */
				nbytes = att_addlength_datum(nbytes, typlen, values[nresult]);
				nbytes = att_align_nominal(nbytes, typalign);
				/* check for overflow of total request */
				if (!AllocSizeIsValid(nbytes))
					ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("array size exceeds the maximum allowed (%d)",
									(int) MaxAllocSize)));
			}
			nresult++;
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

	/*
	 * If not changed just return the original array
	 */
	if (!changed)
	{
		pfree(values);
		pfree(nulls);
		return array;
	}

	/* If all elements were removed return an empty array */
	if (nresult == 0)
	{
		pfree(values);
		pfree(nulls);
		return construct_empty_array(element_type);
	}

	/* Allocate and initialize the result array */
	if (hasnulls)
	{
		dataoffset = ARR_OVERHEAD_WITHNULLS(ndim, nresult);
		nbytes += dataoffset;
	}
	else
	{
		dataoffset = 0;			/* marker for no null bitmap */
		nbytes += ARR_OVERHEAD_NONULLS(ndim);
	}
	result = (ArrayType *) palloc0(nbytes);
	SET_VARSIZE(result, nbytes);
	result->ndim = ndim;
	result->dataoffset = dataoffset;
	result->elemtype = element_type;
	memcpy(ARR_DIMS(result), ARR_DIMS(array), ndim * sizeof(int));
	memcpy(ARR_LBOUND(result), ARR_LBOUND(array), ndim * sizeof(int));

	if (remove)
	{
		/* Adjust the result length */
		ARR_DIMS(result)[0] = nresult;
	}

	/* Insert data into result array */
	CopyArrayEls(result,
				 values, nulls, nresult,
				 typlen, typbyval, typalign,
				 false);

	pfree(values);
	pfree(nulls);

	return result;
}

/*
 * Remove any occurrences of an element from an array
 *
 * If used on a multi-dimensional array this will raise an error.
 */
Datum
array_remove(PG_FUNCTION_ARGS)
{
	ArrayType  *array;
	Datum		search = PG_GETARG_DATUM(1);
	bool		search_isnull = PG_ARGISNULL(1);

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	array = PG_GETARG_ARRAYTYPE_P(0);

	array = array_replace_internal(array,
								   search, search_isnull,
								   (Datum) 0, true,
								   true, PG_GET_COLLATION(),
								   fcinfo);
	PG_RETURN_ARRAYTYPE_P(array);
}

/*
 * Replace any occurrences of an element in an array
 */
Datum
array_replace(PG_FUNCTION_ARGS)
{
	ArrayType  *array;
	Datum		search = PG_GETARG_DATUM(1);
	bool		search_isnull = PG_ARGISNULL(1);
	Datum		replace = PG_GETARG_DATUM(2);
	bool		replace_isnull = PG_ARGISNULL(2);

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	array = PG_GETARG_ARRAYTYPE_P(0);

	array = array_replace_internal(array,
								   search, search_isnull,
								   replace, replace_isnull,
								   false, PG_GET_COLLATION(),
								   fcinfo);
	PG_RETURN_ARRAYTYPE_P(array);
}

/*
 * Implements width_bucket(anyelement, anyarray).
 *
 * 'thresholds' is an array containing lower bound values for each bucket;
 * these must be sorted from smallest to largest, or bogus results will be
 * produced.  If N thresholds are supplied, the output is from 0 to N:
 * 0 is for inputs < first threshold, N is for inputs >= last threshold.
 */
Datum
width_bucket_array(PG_FUNCTION_ARGS)
{
	Datum		operand = PG_GETARG_DATUM(0);
	ArrayType  *thresholds = PG_GETARG_ARRAYTYPE_P(1);
	Oid			collation = PG_GET_COLLATION();
	Oid			element_type = ARR_ELEMTYPE(thresholds);
	int			result;

	/* Check input */
	if (ARR_NDIM(thresholds) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("thresholds must be one-dimensional array")));

	if (array_contains_nulls(thresholds))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("thresholds array must not contain NULLs")));

	/* We have a dedicated implementation for float8 data */
	if (element_type == FLOAT8OID)
		result = width_bucket_array_float8(operand, thresholds);
	else
	{
		TypeCacheEntry *typentry;

		/* Cache information about the input type */
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

		/*
		 * We have separate implementation paths for fixed- and variable-width
		 * types, since indexing the array is a lot cheaper in the first case.
		 */
		if (typentry->typlen > 0)
			result = width_bucket_array_fixed(operand, thresholds,
											  collation, typentry);
		else
			result = width_bucket_array_variable(operand, thresholds,
												 collation, typentry);
	}

	/* Avoid leaking memory when handed toasted input. */
	PG_FREE_IF_COPY(thresholds, 1);

	PG_RETURN_INT32(result);
}

/*
 * width_bucket_array for float8 data.
 */
static int
width_bucket_array_float8(Datum operand, ArrayType *thresholds)
{
	float8		op = DatumGetFloat8(operand);
	float8	   *thresholds_data;
	int			left;
	int			right;

	/*
	 * Since we know the array contains no NULLs, we can just index it
	 * directly.
	 */
	thresholds_data = (float8 *) ARR_DATA_PTR(thresholds);

	left = 0;
	right = ArrayGetNItems(ARR_NDIM(thresholds), ARR_DIMS(thresholds));

	/*
	 * If the probe value is a NaN, it's greater than or equal to all possible
	 * threshold values (including other NaNs), so we need not search.  Note
	 * that this would give the same result as searching even if the array
	 * contains multiple NaNs (as long as they're correctly sorted), since the
	 * loop logic will find the rightmost of multiple equal threshold values.
	 */
	if (isnan(op))
		return right;

	/* Find the bucket */
	while (left < right)
	{
		int			mid = (left + right) / 2;

		if (isnan(thresholds_data[mid]) || op < thresholds_data[mid])
			right = mid;
		else
			left = mid + 1;
	}

	return left;
}

/*
 * width_bucket_array for generic fixed-width data types.
 */
static int
width_bucket_array_fixed(Datum operand,
						 ArrayType *thresholds,
						 Oid collation,
						 TypeCacheEntry *typentry)
{
	LOCAL_FCINFO(locfcinfo, 2);
	char	   *thresholds_data;
	int			typlen = typentry->typlen;
	bool		typbyval = typentry->typbyval;
	int			left;
	int			right;

	/*
	 * Since we know the array contains no NULLs, we can just index it
	 * directly.
	 */
	thresholds_data = (char *) ARR_DATA_PTR(thresholds);

	InitFunctionCallInfoData(*locfcinfo, &typentry->cmp_proc_finfo, 2,
							 collation, NULL, NULL);

	/* Find the bucket */
	left = 0;
	right = ArrayGetNItems(ARR_NDIM(thresholds), ARR_DIMS(thresholds));
	while (left < right)
	{
		int			mid = (left + right) / 2;
		char	   *ptr;
		int32		cmpresult;

		ptr = thresholds_data + mid * typlen;

		locfcinfo->args[0].value = operand;
		locfcinfo->args[0].isnull = false;
		locfcinfo->args[1].value = fetch_att(ptr, typbyval, typlen);
		locfcinfo->args[1].isnull = false;

		cmpresult = DatumGetInt32(FunctionCallInvoke(locfcinfo));

		/* We don't expect comparison support functions to return null */
		Assert(!locfcinfo->isnull);

		if (cmpresult < 0)
			right = mid;
		else
			left = mid + 1;
	}

	return left;
}

/*
 * width_bucket_array for generic variable-width data types.
 */
static int
width_bucket_array_variable(Datum operand,
							ArrayType *thresholds,
							Oid collation,
							TypeCacheEntry *typentry)
{
	LOCAL_FCINFO(locfcinfo, 2);
	char	   *thresholds_data;
	int			typlen = typentry->typlen;
	bool		typbyval = typentry->typbyval;
	char		typalign = typentry->typalign;
	int			left;
	int			right;

	thresholds_data = (char *) ARR_DATA_PTR(thresholds);

	InitFunctionCallInfoData(*locfcinfo, &typentry->cmp_proc_finfo, 2,
							 collation, NULL, NULL);

	/* Find the bucket */
	left = 0;
	right = ArrayGetNItems(ARR_NDIM(thresholds), ARR_DIMS(thresholds));
	while (left < right)
	{
		int			mid = (left + right) / 2;
		char	   *ptr;
		int			i;
		int32		cmpresult;

		/* Locate mid'th array element by advancing from left element */
		ptr = thresholds_data;
		for (i = left; i < mid; i++)
		{
			ptr = att_addlength_pointer(ptr, typlen, ptr);
			ptr = (char *) att_align_nominal(ptr, typalign);
		}

		locfcinfo->args[0].value = operand;
		locfcinfo->args[0].isnull = false;
		locfcinfo->args[1].value = fetch_att(ptr, typbyval, typlen);
		locfcinfo->args[1].isnull = false;

		cmpresult = DatumGetInt32(FunctionCallInvoke(locfcinfo));

		/* We don't expect comparison support functions to return null */
		Assert(!locfcinfo->isnull);

		if (cmpresult < 0)
			right = mid;
		else
		{
			left = mid + 1;

			/*
			 * Move the thresholds pointer to match new "left" index, so we
			 * don't have to seek over those elements again.  This trick
			 * ensures we do only O(N) array indexing work, not O(N^2).
			 */
			ptr = att_addlength_pointer(ptr, typlen, ptr);
			thresholds_data = (char *) att_align_nominal(ptr, typalign);
		}
	}

	return left;
}

/*
 * Trim the last N elements from an array by building an appropriate slice.
 * Only the first dimension is trimmed.
 */
Datum
trim_array(PG_FUNCTION_ARGS)
{
	ArrayType  *v = PG_GETARG_ARRAYTYPE_P(0);
	int			n = PG_GETARG_INT32(1);
	int			array_length = (ARR_NDIM(v) > 0) ? ARR_DIMS(v)[0] : 0;
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	int			lower[MAXDIM];
	int			upper[MAXDIM];
	bool		lowerProvided[MAXDIM];
	bool		upperProvided[MAXDIM];
	Datum		result;

	/* Per spec, throw an error if out of bounds */
	if (n < 0 || n > array_length)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_ELEMENT_ERROR),
				 errmsg("number of elements to trim must be between 0 and %d",
						array_length)));

	/* Set all the bounds as unprovided except the first upper bound */
	memset(lowerProvided, false, sizeof(lowerProvided));
	memset(upperProvided, false, sizeof(upperProvided));
	if (ARR_NDIM(v) > 0)
	{
		upper[0] = ARR_LBOUND(v)[0] + array_length - n - 1;
		upperProvided[0] = true;
	}

	/* Fetch the needed information about the element type */
	get_typlenbyvalalign(ARR_ELEMTYPE(v), &elmlen, &elmbyval, &elmalign);

	/* Get the slice */
	result = array_get_slice(PointerGetDatum(v), 1,
							 upper, lower, upperProvided, lowerProvided,
							 -1, elmlen, elmbyval, elmalign);

	PG_RETURN_DATUM(result);
}
