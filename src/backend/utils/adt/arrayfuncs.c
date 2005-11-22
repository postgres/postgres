/*-------------------------------------------------------------------------
 *
 * arrayfuncs.c
 *	  Support functions for arrays.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/adt/arrayfuncs.c,v 1.123.2.1 2005/11/22 18:23:20 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/tupmacs.h"
#include "catalog/catalog.h"
#include "catalog/pg_type.h"
#include "libpq/pqformat.h"
#include "parser/parse_coerce.h"
#include "parser/parse_oper.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


/*----------
 * A standard varlena array has the following internal structure:
 *	  <size>		- total number of bytes (also, TOAST info flags)
 *	  <ndim>		- number of dimensions of the array
 *	  <flags>		- bit mask of flags
 *	  <elemtype>	- element type OID
 *	  <dim>			- size of each array axis (C array of int)
 *	  <dim_lower>	- lower boundary of each dimension (C array of int)
 *	  <actual data> - whatever is the stored data
 * The actual data starts on a MAXALIGN boundary.  Individual items in the
 * array are aligned as specified by the array element type.
 *
 * NOTE: it is important that array elements of toastable datatypes NOT be
 * toasted, since the tupletoaster won't know they are there.  (We could
 * support compressed toasted items; only out-of-line items are dangerous.
 * However, it seems preferable to store such items uncompressed and allow
 * the toaster to compress the whole array as one input.)
 *
 * There is currently no support for NULL elements in arrays, either.
 * A reasonable (and backwards-compatible) way to add support would be to
 * add a nulls bitmap following the <dim_lower> array, which would be present
 * if needed; and its presence would be signaled by a bit in the flags word.
 *
 *
 * There are also some "fixed-length array" datatypes, such as NAME and
 * POINT.  These are simply a sequence of a fixed number of items each
 * of a fixed-length datatype, with no overhead; the item size must be
 * a multiple of its alignment requirement, because we do no padding.
 * We support subscripting on these types, but array_in() and array_out()
 * only work with varlena arrays.
 *----------
 */


/* ----------
 * Local definitions
 * ----------
 */
#define ASSGN	 "="

#define RETURN_NULL(type)  do { *isNull = true; return (type) 0; } while (0)

static int	ArrayCount(char *str, int *dim, char typdelim);
static Datum *ReadArrayStr(char *arrayStr, const char *origStr,
			 int nitems, int ndim, int *dim,
			 FmgrInfo *inputproc, Oid typioparam, int32 typmod,
			 char typdelim,
			 int typlen, bool typbyval, char typalign,
			 int *nbytes);
static Datum *ReadArrayBinary(StringInfo buf, int nitems,
				FmgrInfo *receiveproc, Oid typioparam, int32 typmod,
				int typlen, bool typbyval, char typalign,
				int *nbytes);
static void CopyArrayEls(char *p, Datum *values, int nitems,
			 int typlen, bool typbyval, char typalign,
			 bool freedata);
static Datum ArrayCast(char *value, bool byval, int len);
static int ArrayCastAndSet(Datum src,
				int typlen, bool typbyval, char typalign,
				char *dest);
static int array_nelems_size(char *ptr, int nitems,
				  int typlen, bool typbyval, char typalign);
static char *array_seek(char *ptr, int nitems,
		   int typlen, bool typbyval, char typalign);
static int array_copy(char *destptr, int nitems, char *srcptr,
		   int typlen, bool typbyval, char typalign);
static int array_slice_size(int ndim, int *dim, int *lb, char *arraydataptr,
				 int *st, int *endp,
				 int typlen, bool typbyval, char typalign);
static void array_extract_slice(int ndim, int *dim, int *lb,
					char *arraydataptr,
					int *st, int *endp, char *destPtr,
					int typlen, bool typbyval, char typalign);
static void array_insert_slice(int ndim, int *dim, int *lb,
				   char *origPtr, int origdatasize,
				   char *destPtr,
				   int *st, int *endp, char *srcPtr,
				   int typlen, bool typbyval, char typalign);
static int	array_cmp(FunctionCallInfo fcinfo);
static Datum array_type_length_coerce_internal(ArrayType *src,
								  int32 desttypmod,
								  bool isExplicit,
								  FmgrInfo *fmgr_info);


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
	int32		nbytes;
	Datum	   *dataPtr;
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

	nitems = ArrayGetNItems(ndim, dim);
	if (nitems == 0)
	{
		/* Return empty array */
		retval = (ArrayType *) palloc0(sizeof(ArrayType));
		retval->size = sizeof(ArrayType);
		retval->elemtype = element_type;
		PG_RETURN_ARRAYTYPE_P(retval);
	}

	if (*p != '{')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("missing left brace")));
	dataPtr = ReadArrayStr(p, string,
						   nitems, ndim, dim, &my_extra->proc, typioparam,
						   typmod, typdelim, typlen, typbyval, typalign,
						   &nbytes);
	nbytes += ARR_OVERHEAD(ndim);
	retval = (ArrayType *) palloc0(nbytes);
	retval->size = nbytes;
	retval->ndim = ndim;
	retval->elemtype = element_type;
	memcpy(ARR_DIMS(retval), dim, ndim * sizeof(int));
	memcpy(ARR_LBOUND(retval), lBound, ndim * sizeof(int));

	CopyArrayEls(ARR_DATA_PTR(retval), dataPtr, nitems,
				 typlen, typbyval, typalign, true);
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

static int
ArrayCount(char *str, int *dim, char typdelim)
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
	char	   *ptr;
	ArrayParseState parse_state = ARRAY_NO_LEVEL;

	for (i = 0; i < MAXDIM; ++i)
	{
		temp[i] = dim[i] = 0;
		nelems_last[i] = nelems[i] = 1;
	}

	/* special case for an empty array */
	if (strcmp(str, "{}") == 0)
		return 0;

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

/*---------------------------------------------------------------------------
 * ReadArrayStr :
 *	 parses the array string pointed by "arrayStr" and converts it to
 *	 internal format. The external format expected is like C array
 *	 declaration. Unspecified elements are initialized to zero for fixed length
 *	 base types and to empty varlena structures for variable length base
 *	 types.  (This is pretty bogus; NULL would be much safer.)
 *
 * result :
 *	 returns a palloc'd array of Datum representations of the array elements.
 *	 If element type is pass-by-ref, the Datums point to palloc'd values.
 *	 *nbytes is set to the amount of data space needed for the array,
 *	 including alignment padding but not including array header overhead.
 *
 *	 CAUTION: the contents of "arrayStr" will be modified!
 *---------------------------------------------------------------------------
 */
static Datum *
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
			 int *nbytes)
{
	int			i,
				nest_level = 0;
	Datum	   *values;
	char	   *srcptr;
	bool		in_quotes = false;
	bool		eoArray = false;
	int			totbytes;
	int			indx[MAXDIM],
				prod[MAXDIM];

	mda_get_prod(ndim, dim, prod);
	values = (Datum *) palloc0(nitems * sizeof(Datum));
	MemSet(indx, 0, sizeof(indx));

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

		values[i] = FunctionCall3(inputproc,
								  CStringGetDatum(itemstart),
								  ObjectIdGetDatum(typioparam),
								  Int32GetDatum(typmod));
	}

	/*
	 * Initialize any unset items and compute total data space needed
	 */
	if (typlen > 0)
	{
		totbytes = nitems * att_align(typlen, typalign);
		if (!typbyval)
			for (i = 0; i < nitems; i++)
				if (values[i] == (Datum) 0)
					values[i] = PointerGetDatum(palloc0(typlen));
	}
	else
	{
		Assert(!typbyval);
		totbytes = 0;
		for (i = 0; i < nitems; i++)
		{
			if (values[i] != (Datum) 0)
			{
				/* let's just make sure data is not toasted */
				if (typlen == -1)
					values[i] = PointerGetDatum(PG_DETOAST_DATUM(values[i]));
				totbytes = att_addlength(totbytes, typlen, values[i]);
				totbytes = att_align(totbytes, typalign);
			}
			else if (typlen == -1)
			{
				/* dummy varlena value (XXX bogus, see notes above) */
				values[i] = PointerGetDatum(palloc(sizeof(int32)));
				VARATT_SIZEP(DatumGetPointer(values[i])) = sizeof(int32);
				totbytes += sizeof(int32);
				totbytes = att_align(totbytes, typalign);
			}
			else
			{
				/* dummy cstring value */
				Assert(typlen == -2);
				values[i] = PointerGetDatum(palloc(1));
				*((char *) DatumGetPointer(values[i])) = '\0';
				totbytes += 1;
				totbytes = att_align(totbytes, typalign);
			}
		}
	}
	*nbytes = totbytes;
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
 * the values are not toasted.	(Doing it here doesn't work since the
 * caller has already allocated space for the array...)
 *----------
 */
static void
CopyArrayEls(char *p,
			 Datum *values,
			 int nitems,
			 int typlen,
			 bool typbyval,
			 char typalign,
			 bool freedata)
{
	int			i;

	if (typbyval)
		freedata = false;

	for (i = 0; i < nitems; i++)
	{
		p += ArrayCastAndSet(values[i], typlen, typbyval, typalign, p);
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
	Oid			element_type;
	int			typlen;
	bool		typbyval;
	char		typalign;
	char		typdelim;
	char	   *p,
			   *tmp,
			   *retval,
			  **values,

	/*
	 * 33 per dim since we assume 15 digits per number + ':' +'[]'
	 *
	 * +2 allows for assignment operator + trailing null
	 */
				dims_str[(MAXDIM * 33) + 2];
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

	element_type = ARR_ELEMTYPE(v);

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
		my_extra->element_type = InvalidOid;
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
	p = ARR_DATA_PTR(v);
	overall_length = 1;			/* don't forget to count \0 at end. */

	for (i = 0; i < nitems; i++)
	{
		Datum		itemvalue;
		bool		needquote;

		itemvalue = fetch_att(p, typbyval, typlen);
		values[i] = DatumGetCString(FunctionCall1(&my_extra->proc,
												  itemvalue));
		p = att_addlength(p, typlen, PointerGetDatum(p));
		p = (char *) att_align(p, typalign);

		/* count data plus backslashes; detect chars needing quotes */
		if (values[i][0] == '\0')
			needquote = true;	/* force quotes for empty string */
		else
			needquote = false;

		for (tmp = values[i]; *tmp != '\0'; tmp++)
		{
			char		ch = *tmp;

			overall_length += 1;
			if (ch == '"' || ch == '\\')
			{
				needquote = true;
#ifndef TCL_ARRAYS
				overall_length += 1;
#endif
			}
			else if (ch == '{' || ch == '}' || ch == typdelim ||
					 isspace((unsigned char) ch))
				needquote = true;
		}

		needquotes[i] = needquote;

		/* Count the pair of double quotes, if needed */
		if (needquote)
			overall_length += 2;

		/* and the comma */
		overall_length += 1;
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
#ifndef TCL_ARRAYS
			for (tmp = values[k]; *tmp; tmp++)
			{
				char		ch = *tmp;

				if (ch == '"' || ch == '\\')
					*p++ = '\\';
				*p++ = ch;
			}
			*p = '\0';
#else
			APPENDSTR(values[k]);
#endif
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

/*---------------------------------------------------------------------
 * array_recv :
 *		  converts an array from the external binary format to
 *		  its internal format.
 * return value :
 *		  the internal representation of the input array
 *--------------------------------------------------------------------
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
	int32		nbytes;
	Datum	   *dataPtr;
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
	if (flags != 0)
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
		retval = (ArrayType *) palloc0(sizeof(ArrayType));
		retval->size = sizeof(ArrayType);
		retval->elemtype = element_type;
		PG_RETURN_ARRAYTYPE_P(retval);
	}

	typlen = my_extra->typlen;
	typbyval = my_extra->typbyval;
	typalign = my_extra->typalign;
	typioparam = my_extra->typioparam;

	dataPtr = ReadArrayBinary(buf, nitems, &my_extra->proc,
							  typioparam, typmod,
							  typlen, typbyval, typalign,
							  &nbytes);
	nbytes += ARR_OVERHEAD(ndim);

	retval = (ArrayType *) palloc0(nbytes);
	retval->size = nbytes;
	retval->ndim = ndim;
	retval->elemtype = element_type;
	memcpy(ARR_DIMS(retval), dim, ndim * sizeof(int));
	memcpy(ARR_LBOUND(retval), lBound, ndim * sizeof(int));

	CopyArrayEls(ARR_DATA_PTR(retval), dataPtr, nitems,
				 typlen, typbyval, typalign, true);
	pfree(dataPtr);

	PG_RETURN_ARRAYTYPE_P(retval);
}

/*---------------------------------------------------------------------------
 * ReadArrayBinary:
 *	 collect the data elements of an array being read in binary style.
 * result :
 *	 returns a palloc'd array of Datum representations of the array elements.
 *	 If element type is pass-by-ref, the Datums point to palloc'd values.
 *	 *nbytes is set to the amount of data space needed for the array,
 *	 including alignment padding but not including array header overhead.
 *---------------------------------------------------------------------------
 */
static Datum *
ReadArrayBinary(StringInfo buf,
				int nitems,
				FmgrInfo *receiveproc,
				Oid typioparam,
				int32 typmod,
				int typlen,
				bool typbyval,
				char typalign,
				int *nbytes)
{
	Datum	   *values;
	int			i;

	values = (Datum *) palloc(nitems * sizeof(Datum));

	for (i = 0; i < nitems; i++)
	{
		int			itemlen;
		StringInfoData elem_buf;
		char		csave;

		/* Get and check the item length */
		itemlen = pq_getmsgint(buf, 4);
		if (itemlen < 0 || itemlen > (buf->len - buf->cursor))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("insufficient data left in message")));

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
		values[i] = FunctionCall3(receiveproc,
								  PointerGetDatum(&elem_buf),
								  ObjectIdGetDatum(typioparam),
								  Int32GetDatum(typmod));

		/* Trouble if it didn't eat the whole buffer */
		if (elem_buf.cursor != itemlen)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("improper binary format in array element %d",
							i + 1)));

		buf->data[buf->cursor] = csave;
	}

	/*
	 * Compute total data space needed
	 */
	if (typlen > 0)
		*nbytes = nitems * att_align(typlen, typalign);
	else
	{
		Assert(!typbyval);
		*nbytes = 0;
		for (i = 0; i < nitems; i++)
		{
			/* let's just make sure data is not toasted */
			if (typlen == -1)
				values[i] = PointerGetDatum(PG_DETOAST_DATUM(values[i]));
			*nbytes = att_addlength(*nbytes, typlen, values[i]);
			*nbytes = att_align(*nbytes, typalign);
		}
	}

	return values;
}


/*-------------------------------------------------------------------------
 * array_send :
 *		   takes the internal representation of an array and returns a bytea
 *		  containing the array in its external binary format.
 *-------------------------------------------------------------------------
 */
Datum
array_send(PG_FUNCTION_ARGS)
{
	ArrayType  *v = PG_GETARG_ARRAYTYPE_P(0);
	Oid			element_type;
	int			typlen;
	bool		typbyval;
	char		typalign;
	char	   *p;
	int			nitems,
				i;
	int			ndim,
			   *dim;
	StringInfoData buf;
	ArrayMetaState *my_extra;

	/* Get information about the element type and the array dimensions */
	element_type = ARR_ELEMTYPE(v);

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
		my_extra->element_type = InvalidOid;
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
	pq_sendint(&buf, v->flags, 4);
	pq_sendint(&buf, element_type, sizeof(Oid));
	for (i = 0; i < ndim; i++)
	{
		pq_sendint(&buf, ARR_DIMS(v)[i], 4);
		pq_sendint(&buf, ARR_LBOUND(v)[i], 4);
	}

	/* Send the array elements using the element's own sendproc */
	p = ARR_DATA_PTR(v);
	for (i = 0; i < nitems; i++)
	{
		Datum		itemvalue;
		bytea	   *outputbytes;

		itemvalue = fetch_att(p, typbyval, typlen);

		outputbytes = DatumGetByteaP(FunctionCall1(&my_extra->proc,
												   itemvalue));
		/* We assume the result will not have been toasted */
		pq_sendint(&buf, VARSIZE(outputbytes) - VARHDRSZ, 4);
		pq_sendbytes(&buf, VARDATA(outputbytes),
					 VARSIZE(outputbytes) - VARHDRSZ);
		pfree(outputbytes);

		p = att_addlength(p, typlen, PointerGetDatum(p));
		p = (char *) att_align(p, typalign);
	}

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
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

/*-----------------------------------------------------------------------------
 * array_lower :
 *		returns the lower dimension, of the DIM requested, for
 *		the array pointed to by "v", as an int4
 *----------------------------------------------------------------------------
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

/*-----------------------------------------------------------------------------
 * array_upper :
 *		returns the upper dimension, of the DIM requested, for
 *		the array pointed to by "v", as an int4
 *----------------------------------------------------------------------------
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
		  int arraylen,
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

	if (array == NULL)
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
		/* detoast input array if necessary */
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

	retptr = array_seek(arraydataptr, offset, elmlen, elmbyval, elmalign);

	*isNull = false;
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
				int arraylen,
				int elmlen,
				bool elmbyval,
				char elmalign,
				bool *isNull)
{
	int			i,
				ndim,
			   *dim,
			   *lb,
			   *newlb;
	int			fixedDim[1],
				fixedLb[1];
	char	   *arraydataptr;
	ArrayType  *newarray;
	int			bytes,
				span[MAXDIM];

	if (array == NULL)
		RETURN_NULL(ArrayType *);

	if (arraylen > 0)
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
		 * fixed-length arrays -- these are assumed to be 1-d, 0-based XXX
		 * where would we get the correct ELEMTYPE from?
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
		/* detoast input array if necessary */
		array = DatumGetArrayTypeP(PointerGetDatum(array));

		ndim = ARR_NDIM(array);
		dim = ARR_DIMS(array);
		lb = ARR_LBOUND(array);
		arraydataptr = ARR_DATA_PTR(array);
	}

	/*
	 * Check provided subscripts.  A slice exceeding the current array limits
	 * is silently truncated to the array limits.  If we end up with an empty
	 * slice, return NULL (should it be an empty array instead?)
	 */
	if (ndim < nSubscripts || ndim <= 0 || ndim > MAXDIM)
		RETURN_NULL(ArrayType *);

	for (i = 0; i < nSubscripts; i++)
	{
		if (lowerIndx[i] < lb[i])
			lowerIndx[i] = lb[i];
		if (upperIndx[i] >= (dim[i] + lb[i]))
			upperIndx[i] = dim[i] + lb[i] - 1;
		if (lowerIndx[i] > upperIndx[i])
			RETURN_NULL(ArrayType *);
	}
	/* fill any missing subscript positions with full array range */
	for (; i < ndim; i++)
	{
		lowerIndx[i] = lb[i];
		upperIndx[i] = dim[i] + lb[i] - 1;
		if (lowerIndx[i] > upperIndx[i])
			RETURN_NULL(ArrayType *);
	}

	mda_get_range(ndim, span, lowerIndx, upperIndx);

	bytes = array_slice_size(ndim, dim, lb, arraydataptr,
							 lowerIndx, upperIndx,
							 elmlen, elmbyval, elmalign);
	bytes += ARR_OVERHEAD(ndim);

	newarray = (ArrayType *) palloc(bytes);
	newarray->size = bytes;
	newarray->ndim = ndim;
	newarray->flags = 0;
	newarray->elemtype = ARR_ELEMTYPE(array);
	memcpy(ARR_DIMS(newarray), span, ndim * sizeof(int));

	/*
	 * Lower bounds of the new array are set to 1.	Formerly (before 7.3) we
	 * copied the given lowerIndx values ... but that seems confusing.
	 */
	newlb = ARR_LBOUND(newarray);
	for (i = 0; i < ndim; i++)
		newlb[i] = 1;

	array_extract_slice(ndim, dim, lb, arraydataptr,
						lowerIndx, upperIndx, ARR_DATA_PTR(newarray),
						elmlen, elmbyval, elmalign);

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
		  int arraylen,
		  int elmlen,
		  bool elmbyval,
		  char elmalign,
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

	if (array == NULL)
		RETURN_NULL(ArrayType *);

	if (arraylen > 0)
	{
		/*
		 * fixed-length arrays -- these are assumed to be 1-d, 0-based. We
		 * cannot extend them, either.
		 */
		if (nSubscripts != 1)
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("invalid array subscripts")));

		if (indx[0] < 0 || indx[0] * elmlen >= arraylen)
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("invalid array subscripts")));

		newarray = (ArrayType *) palloc(arraylen);
		memcpy(newarray, array, arraylen);
		elt_ptr = (char *) newarray + indx[0] * elmlen;
		ArrayCastAndSet(dataValue, elmlen, elmbyval, elmalign, elt_ptr);
		return newarray;
	}

	/* make sure item to be inserted is not toasted */
	if (elmlen == -1)
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

		return construct_md_array(&dataValue, nSubscripts, dim, lb, elmtype,
								  elmlen, elmbyval, elmalign);
	}

	if (ndim != nSubscripts || ndim <= 0 || ndim > MAXDIM)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("invalid array subscripts")));

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
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("invalid array subscripts")));
		}
		if (indx[i] >= (dim[i] + lb[i]))
		{
			if (ndim == 1 && indx[i] == (dim[i] + lb[i]))
			{
				dim[i]++;
				extendafter = true;
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("invalid array subscripts")));
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
		elt_ptr = array_seek(ARR_DATA_PTR(array), offset,
							 elmlen, elmbyval, elmalign);
		lenbefore = (int) (elt_ptr - ARR_DATA_PTR(array));
		olditemlen = att_addlength(0, elmlen, PointerGetDatum(elt_ptr));
		olditemlen = att_align(olditemlen, elmalign);
		lenafter = (int) (olddatasize - lenbefore - olditemlen);
	}

	newitemlen = att_addlength(0, elmlen, dataValue);
	newitemlen = att_align(newitemlen, elmalign);

	newsize = overheadlen + lenbefore + newitemlen + lenafter;

	/*
	 * OK, do the assignment
	 */
	newarray = (ArrayType *) palloc(newsize);
	newarray->size = newsize;
	newarray->ndim = ndim;
	newarray->flags = 0;
	newarray->elemtype = ARR_ELEMTYPE(array);
	memcpy(ARR_DIMS(newarray), dim, ndim * sizeof(int));
	memcpy(ARR_LBOUND(newarray), lb, ndim * sizeof(int));
	memcpy((char *) newarray + overheadlen,
		   (char *) array + overheadlen,
		   lenbefore);
	memcpy((char *) newarray + overheadlen + lenbefore + newitemlen,
		   (char *) array + overheadlen + lenbefore + olditemlen,
		   lenafter);

	ArrayCastAndSet(dataValue, elmlen, elmbyval, elmalign,
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
 * NOTE: we assume it is OK to scribble on the provided index arrays
 * lowerIndx[] and upperIndx[].  These are generally just temporaries.
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
				int arraylen,
				int elmlen,
				bool elmbyval,
				char elmalign,
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

	if (array == NULL)
		RETURN_NULL(ArrayType *);
	if (srcArray == NULL)
		return array;

	if (arraylen > 0)
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
		int			nelems;
		Oid			elmtype = ARR_ELEMTYPE(array);

		deconstruct_array(srcArray, elmtype, elmlen, elmbyval, elmalign,
						  &dvalues, &nelems);

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

		return construct_md_array(dvalues, nSubscripts, dim, lb, elmtype,
								  elmlen, elmbyval, elmalign);
	}

	if (ndim < nSubscripts || ndim <= 0 || ndim > MAXDIM)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("invalid array subscripts")));

	/* copy dim/lb since we may modify them */
	memcpy(dim, ARR_DIMS(array), ndim * sizeof(int));
	memcpy(lb, ARR_LBOUND(array), ndim * sizeof(int));

	/*
	 * Check provided subscripts.  A slice exceeding the current array limits
	 * throws an error, *except* in the 1-D case where we will extend the
	 * array as long as no hole is created. An empty slice is an error, too.
	 */
	for (i = 0; i < nSubscripts; i++)
	{
		if (lowerIndx[i] > upperIndx[i])
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("invalid array subscripts")));
		if (lowerIndx[i] < lb[i])
		{
			if (ndim == 1 && upperIndx[i] >= lb[i] - 1)
			{
				dim[i] += lb[i] - lowerIndx[i];
				lb[i] = lowerIndx[i];
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("invalid array subscripts")));
		}
		if (upperIndx[i] >= (dim[i] + lb[i]))
		{
			if (ndim == 1 && lowerIndx[i] <= (dim[i] + lb[i]))
				dim[i] = upperIndx[i] - lb[i] + 1;
			else
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("invalid array subscripts")));
		}
	}
	/* fill any missing subscript positions with full array range */
	for (; i < ndim; i++)
	{
		lowerIndx[i] = lb[i];
		upperIndx[i] = dim[i] + lb[i] - 1;
		if (lowerIndx[i] > upperIndx[i])
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("invalid array subscripts")));
	}

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
	newitemsize = array_nelems_size(ARR_DATA_PTR(srcArray), nsrcitems,
									elmlen, elmbyval, elmalign);
	overheadlen = ARR_OVERHEAD(ndim);
	olddatasize = ARR_SIZE(array) - overheadlen;
	if (ndim > 1)
	{
		/*
		 * here we do not need to cope with extension of the array; it would
		 * be a lot more complicated if we had to do so...
		 */
		olditemsize = array_slice_size(ndim, dim, lb, ARR_DATA_PTR(array),
									   lowerIndx, upperIndx,
									   elmlen, elmbyval, elmalign);
		lenbefore = lenafter = 0;		/* keep compiler quiet */
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

		lenbefore = array_nelems_size(oldarraydata, slicelb - oldlb,
									  elmlen, elmbyval, elmalign);
		if (slicelb > sliceub)
			olditemsize = 0;
		else
			olditemsize = array_nelems_size(oldarraydata + lenbefore,
											sliceub - slicelb + 1,
											elmlen, elmbyval, elmalign);
		lenafter = olddatasize - lenbefore - olditemsize;
	}

	newsize = overheadlen + olddatasize - olditemsize + newitemsize;

	newarray = (ArrayType *) palloc(newsize);
	newarray->size = newsize;
	newarray->ndim = ndim;
	newarray->flags = 0;
	newarray->elemtype = ARR_ELEMTYPE(array);
	memcpy(ARR_DIMS(newarray), dim, ndim * sizeof(int));
	memcpy(ARR_LBOUND(newarray), lb, ndim * sizeof(int));

	if (ndim > 1)
	{
		/*
		 * here we do not need to cope with extension of the array; it would
		 * be a lot more complicated if we had to do so...
		 */
		array_insert_slice(ndim, dim, lb, ARR_DATA_PTR(array), olddatasize,
						   ARR_DATA_PTR(newarray),
						   lowerIndx, upperIndx, ARR_DATA_PTR(srcArray),
						   elmlen, elmbyval, elmalign);
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
 * NB: caller must assure that input array is not NULL.  Currently,
 * any additional parameters passed to fn() may not be specified as NULL
 * either.
 */
Datum
array_map(FunctionCallInfo fcinfo, Oid inpType, Oid retType,
		  ArrayMapState *amstate)
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
	char		inp_typalign;
	int			typlen;
	bool		typbyval;
	char		typalign;
	char	   *s;
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
		result = (ArrayType *) palloc0(sizeof(ArrayType));
		result->size = sizeof(ArrayType);
		result->elemtype = retType;
		PG_RETURN_ARRAYTYPE_P(result);
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

	/* Allocate temporary array for new values */
	values = (Datum *) palloc(nitems * sizeof(Datum));

	/* Loop over source data */
	s = (char *) ARR_DATA_PTR(v);
	for (i = 0; i < nitems; i++)
	{
		/* Get source element */
		elt = fetch_att(s, inp_typbyval, inp_typlen);

		s = att_addlength(s, inp_typlen, PointerGetDatum(s));
		s = (char *) att_align(s, inp_typalign);

		/*
		 * Apply the given function to source elt and extra args.
		 *
		 * We assume the extra args are non-NULL, so need not check whether
		 * fn() is strict.	Would need to do more work here to support arrays
		 * containing nulls, too.
		 */
		fcinfo->arg[0] = elt;
		fcinfo->argnull[0] = false;
		fcinfo->isnull = false;
		values[i] = FunctionCallInvoke(fcinfo);
		if (fcinfo->isnull)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("null array elements not supported")));

		/* Ensure data is not toasted */
		if (typlen == -1)
			values[i] = PointerGetDatum(PG_DETOAST_DATUM(values[i]));

		/* Update total result size */
		nbytes = att_addlength(nbytes, typlen, values[i]);
		nbytes = att_align(nbytes, typalign);
	}

	/* Allocate and initialize the result array */
	nbytes += ARR_OVERHEAD(ndim);
	result = (ArrayType *) palloc0(nbytes);

	result->size = nbytes;
	result->ndim = ndim;
	result->elemtype = retType;
	memcpy(ARR_DIMS(result), ARR_DIMS(v), 2 * ndim * sizeof(int));

	/*
	 * Note: do not risk trying to pfree the results of the called function
	 */
	CopyArrayEls(ARR_DATA_PTR(result), values, nitems,
				 typlen, typbyval, typalign, false);
	pfree(values);

	PG_RETURN_ARRAYTYPE_P(result);
}

/*----------
 * construct_array	--- simple method for constructing an array object
 *
 * elems: array of Datum items to become the array contents
 * nelems: number of items
 * elmtype, elmlen, elmbyval, elmalign: info for the datatype of the items
 *
 * A palloc'd 1-D array object is constructed and returned.  Note that
 * elem values will be copied into the object even if pass-by-ref type.
 * NULL element values are not supported.
 *
 * NOTE: it would be cleaner to look up the elmlen/elmbval/elmalign info
 * from the system catalogs, given the elmtype.  However, the caller is
 * in a better position to cache this info across multiple uses, or even
 * to hard-wire values if the element type is hard-wired.
 *----------
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

	return construct_md_array(elems, 1, dims, lbs,
							  elmtype, elmlen, elmbyval, elmalign);
}

/*----------
 * construct_md_array	--- simple method for constructing an array object
 *							with arbitrary dimensions
 *
 * elems: array of Datum items to become the array contents
 * ndims: number of dimensions
 * dims: integer array with size of each dimension
 * lbs: integer array with lower bound of each dimension
 * elmtype, elmlen, elmbyval, elmalign: info for the datatype of the items
 *
 * A palloc'd ndims-D array object is constructed and returned.  Note that
 * elem values will be copied into the object even if pass-by-ref type.
 * NULL element values are not supported.
 *
 * NOTE: it would be cleaner to look up the elmlen/elmbval/elmalign info
 * from the system catalogs, given the elmtype.  However, the caller is
 * in a better position to cache this info across multiple uses, or even
 * to hard-wire values if the element type is hard-wired.
 *----------
 */
ArrayType *
construct_md_array(Datum *elems,
				   int ndims,
				   int *dims,
				   int *lbs,
				   Oid elmtype, int elmlen, bool elmbyval, char elmalign)
{
	ArrayType  *result;
	int			nbytes;
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
	{
		/* Allocate and initialize 0-D result array */
		result = (ArrayType *) palloc0(sizeof(ArrayType));
		result->size = sizeof(ArrayType);
		result->elemtype = elmtype;
		return result;
	}

	nelems = ArrayGetNItems(ndims, dims);

	/* compute required space */
	if (elmlen > 0)
		nbytes = nelems * att_align(elmlen, elmalign);
	else
	{
		Assert(!elmbyval);
		nbytes = 0;
		for (i = 0; i < nelems; i++)
		{
			/* make sure data is not toasted */
			if (elmlen == -1)
				elems[i] = PointerGetDatum(PG_DETOAST_DATUM(elems[i]));
			nbytes = att_addlength(nbytes, elmlen, elems[i]);
			nbytes = att_align(nbytes, elmalign);
		}
	}

	/* Allocate and initialize ndims-D result array */
	nbytes += ARR_OVERHEAD(ndims);
	result = (ArrayType *) palloc(nbytes);

	result->size = nbytes;
	result->ndim = ndims;
	result->flags = 0;
	result->elemtype = elmtype;
	memcpy(ARR_DIMS(result), dims, ndims * sizeof(int));
	memcpy(ARR_LBOUND(result), lbs, ndims * sizeof(int));
	CopyArrayEls(ARR_DATA_PTR(result), elems, nelems,
				 elmlen, elmbyval, elmalign, false);

	return result;
}

/*----------
 * deconstruct_array  --- simple method for extracting data from an array
 *
 * array: array object to examine (must not be NULL)
 * elmtype, elmlen, elmbyval, elmalign: info for the datatype of the items
 * elemsp: return value, set to point to palloc'd array of Datum values
 * nelemsp: return value, set to number of extracted values
 *
 * If array elements are pass-by-ref data type, the returned Datums will
 * be pointers into the array object.
 *
 * NOTE: it would be cleaner to look up the elmlen/elmbval/elmalign info
 * from the system catalogs, given the elmtype.  However, in most current
 * uses the type is hard-wired into the caller and so we can save a lookup
 * cycle by hard-wiring the type info as well.
 *----------
 */
void
deconstruct_array(ArrayType *array,
				  Oid elmtype,
				  int elmlen, bool elmbyval, char elmalign,
				  Datum **elemsp, int *nelemsp)
{
	Datum	   *elems;
	int			nelems;
	char	   *p;
	int			i;

	Assert(ARR_ELEMTYPE(array) == elmtype);

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
		p = att_addlength(p, elmlen, PointerGetDatum(p));
		p = (char *) att_align(p, elmalign);
	}
}


/*-----------------------------------------------------------------------------
 * array_eq :
 *		  compares two arrays for equality
 * result :
 *		  returns true if the arrays are equal, false otherwise.
 *
 * Note: we do not use array_cmp here, since equality may be meaningful in
 * datatypes that don't have a total ordering (and hence no btree support).
 *-----------------------------------------------------------------------------
 */
Datum
array_eq(PG_FUNCTION_ARGS)
{
	ArrayType  *array1 = PG_GETARG_ARRAYTYPE_P(0);
	ArrayType  *array2 = PG_GETARG_ARRAYTYPE_P(1);
	char	   *p1 = (char *) ARR_DATA_PTR(array1);
	char	   *p2 = (char *) ARR_DATA_PTR(array2);
	int			ndims1 = ARR_NDIM(array1);
	int			ndims2 = ARR_NDIM(array2);
	int		   *dims1 = ARR_DIMS(array1);
	int		   *dims2 = ARR_DIMS(array2);
	int			nitems1 = ArrayGetNItems(ndims1, dims1);
	int			nitems2 = ArrayGetNItems(ndims2, dims2);
	Oid			element_type = ARR_ELEMTYPE(array1);
	bool		result = true;
	TypeCacheEntry *typentry;
	int			typlen;
	bool		typbyval;
	char		typalign;
	int			i;
	FunctionCallInfoData locfcinfo;

	if (element_type != ARR_ELEMTYPE(array2))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot compare arrays of different element types")));

	/* fast path if the arrays do not have the same number of elements */
	if (nitems1 != nitems2)
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
		for (i = 0; i < nitems1; i++)
		{
			Datum		elt1;
			Datum		elt2;
			bool		oprresult;

			/* Get element pair */
			elt1 = fetch_att(p1, typbyval, typlen);
			elt2 = fetch_att(p2, typbyval, typlen);

			p1 = att_addlength(p1, typlen, PointerGetDatum(p1));
			p1 = (char *) att_align(p1, typalign);

			p2 = att_addlength(p2, typlen, PointerGetDatum(p2));
			p2 = (char *) att_align(p2, typalign);

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
	char	   *p1 = (char *) ARR_DATA_PTR(array1);
	char	   *p2 = (char *) ARR_DATA_PTR(array2);
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
	for (i = 0; i < min_nitems; i++)
	{
		Datum		elt1;
		Datum		elt2;
		int32		cmpresult;

		/* Get element pair */
		elt1 = fetch_att(p1, typbyval, typlen);
		elt2 = fetch_att(p2, typbyval, typlen);

		p1 = att_addlength(p1, typlen, PointerGetDatum(p1));
		p1 = (char *) att_align(p1, typalign);

		p2 = att_addlength(p2, typlen, PointerGetDatum(p2));
		p2 = (char *) att_align(p2, typalign);

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

	if ((result == 0) && (nitems1 != nitems2))
		result = (nitems1 < nitems2) ? -1 : 1;

	/* Avoid leaking memory when handed toasted input. */
	PG_FREE_IF_COPY(array1, 0);
	PG_FREE_IF_COPY(array2, 1);

	return result;
}


/***************************************************************************/
/******************|		  Support  Routines			  |*****************/
/***************************************************************************/

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
		inc = att_align(typlen, typalign);
	}
	else
	{
		Assert(!typbyval);
		inc = att_addlength(0, typlen, src);
		memmove(dest, DatumGetPointer(src), inc);
		inc = att_align(inc, typalign);
	}

	return inc;
}

/*
 * Compute total size of the nitems array elements starting at *ptr
 */
static int
array_nelems_size(char *ptr, int nitems,
				  int typlen, bool typbyval, char typalign)
{
	char	   *origptr;
	int			i;

	/* fixed-size elements? */
	if (typlen > 0)
		return nitems * att_align(typlen, typalign);

	Assert(!typbyval);
	origptr = ptr;
	for (i = 0; i < nitems; i++)
	{
		ptr = att_addlength(ptr, typlen, PointerGetDatum(ptr));
		ptr = (char *) att_align(ptr, typalign);
	}
	return ptr - origptr;
}

/*
 * Advance ptr over nitems array elements
 */
static char *
array_seek(char *ptr, int nitems,
		   int typlen, bool typbyval, char typalign)
{
	return ptr + array_nelems_size(ptr, nitems,
								   typlen, typbyval, typalign);
}

/*
 * Copy nitems array elements from srcptr to destptr
 *
 * Returns number of bytes copied
 */
static int
array_copy(char *destptr, int nitems, char *srcptr,
		   int typlen, bool typbyval, char typalign)
{
	int			numbytes = array_nelems_size(srcptr, nitems,
											 typlen, typbyval, typalign);

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
				 int *st, int *endp,
				 int typlen, bool typbyval, char typalign)
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
	if (typlen > 0)
		return ArrayGetNItems(ndim, span) * att_align(typlen, typalign);

	/* Else gotta do it the hard way */
	st_pos = ArrayGetOffset(ndim, dim, lb, st);
	ptr = array_seek(arraydataptr, st_pos,
					 typlen, typbyval, typalign);
	mda_get_prod(ndim, dim, prod);
	mda_get_offset_values(ndim, dist, prod, span);
	for (i = 0; i < ndim; i++)
		indx[i] = 0;
	j = ndim - 1;
	do
	{
		ptr = array_seek(ptr, dist[j],
						 typlen, typbyval, typalign);
		inc = att_addlength(0, typlen, PointerGetDatum(ptr));
		inc = att_align(inc, typalign);
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
					int *st,
					int *endp,
					char *destPtr,
					int typlen,
					bool typbyval,
					char typalign)
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
	srcPtr = array_seek(arraydataptr, st_pos,
						typlen, typbyval, typalign);
	mda_get_prod(ndim, dim, prod);
	mda_get_range(ndim, span, st, endp);
	mda_get_offset_values(ndim, dist, prod, span);
	for (i = 0; i < ndim; i++)
		indx[i] = 0;
	j = ndim - 1;
	do
	{
		srcPtr = array_seek(srcPtr, dist[j],
							typlen, typbyval, typalign);
		inc = array_copy(destPtr, 1, srcPtr,
						 typlen, typbyval, typalign);
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
				   int *st,
				   int *endp,
				   char *srcPtr,
				   int typlen,
				   bool typbyval,
				   char typalign)
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
	inc = array_copy(destPtr, st_pos, origPtr,
					 typlen, typbyval, typalign);
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
		inc = array_copy(destPtr, dist[j], origPtr,
						 typlen, typbyval, typalign);
		destPtr += inc;
		origPtr += inc;
		/* Copy new element at this slice position */
		inc = array_copy(destPtr, 1, srcPtr,
						 typlen, typbyval, typalign);
		destPtr += inc;
		srcPtr += inc;
		/* Advance over old element at this slice position */
		origPtr = array_seek(origPtr, 1,
							 typlen, typbyval, typalign);
	} while ((j = mda_next_tuple(ndim, indx, span)) != -1);

	/* don't miss any data at the end */
	memcpy(destPtr, origPtr, origEndpoint - origPtr);
}

/*
 * array_type_coerce -- allow explicit or assignment coercion from
 * one array type to another.
 *
 * array_type_length_coerce -- the same, for cases where both type and length
 * coercion are done by a single function on the element type.
 *
 * Caller should have already verified that the source element type can be
 * coerced into the target element type.
 */
Datum
array_type_coerce(PG_FUNCTION_ARGS)
{
	ArrayType  *src = PG_GETARG_ARRAYTYPE_P(0);
	FmgrInfo   *fmgr_info = fcinfo->flinfo;

	return array_type_length_coerce_internal(src, -1, false, fmgr_info);
}

Datum
array_type_length_coerce(PG_FUNCTION_ARGS)
{
	ArrayType  *src = PG_GETARG_ARRAYTYPE_P(0);
	int32		desttypmod = PG_GETARG_INT32(1);
	bool		isExplicit = PG_GETARG_BOOL(2);
	FmgrInfo   *fmgr_info = fcinfo->flinfo;

	return array_type_length_coerce_internal(src, desttypmod,
											 isExplicit, fmgr_info);
}

static Datum
array_type_length_coerce_internal(ArrayType *src,
								  int32 desttypmod,
								  bool isExplicit,
								  FmgrInfo *fmgr_info)
{
	Oid			src_elem_type = ARR_ELEMTYPE(src);
	typedef struct
	{
		Oid			srctype;
		Oid			desttype;
		FmgrInfo	coerce_finfo;
		ArrayMapState amstate;
	} atc_extra;
	atc_extra  *my_extra;
	FunctionCallInfoData locfcinfo;

	/*
	 * We arrange to look up the coercion function only once per series of
	 * calls, assuming the input data type doesn't change underneath us.
	 * (Output type can't change.)
	 */
	my_extra = (atc_extra *) fmgr_info->fn_extra;
	if (my_extra == NULL)
	{
		fmgr_info->fn_extra = MemoryContextAllocZero(fmgr_info->fn_mcxt,
													 sizeof(atc_extra));
		my_extra = (atc_extra *) fmgr_info->fn_extra;
	}

	if (my_extra->srctype != src_elem_type)
	{
		Oid			tgt_type = get_fn_expr_rettype(fmgr_info);
		Oid			tgt_elem_type;
		Oid			funcId;

		if (tgt_type == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not determine target array type")));

		tgt_elem_type = get_element_type(tgt_type);
		if (tgt_elem_type == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("target type is not an array")));

		/*
		 * We don't deal with domain constraints yet, so bail out. This isn't
		 * currently a problem, because we also don't support arrays of domain
		 * type elements either. But in the future we might. At that point
		 * consideration should be given to removing the check below and
		 * adding a domain constraints check to the coercion.
		 */
		if (getBaseType(tgt_elem_type) != tgt_elem_type)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("array coercion to domain type elements not "
							"currently supported")));

		if (!find_coercion_pathway(tgt_elem_type, src_elem_type,
								   COERCION_EXPLICIT, &funcId))
		{
			/* should never happen, but check anyway */
			elog(ERROR, "no conversion function from %s to %s",
				 format_type_be(src_elem_type),
				 format_type_be(tgt_elem_type));
		}
		if (OidIsValid(funcId))
			fmgr_info_cxt(funcId, &my_extra->coerce_finfo, fmgr_info->fn_mcxt);
		else
			my_extra->coerce_finfo.fn_oid = InvalidOid;
		my_extra->srctype = src_elem_type;
		my_extra->desttype = tgt_elem_type;
	}

	/*
	 * If it's binary-compatible, modify the element type in the array header,
	 * but otherwise leave the array as we received it.
	 */
	if (my_extra->coerce_finfo.fn_oid == InvalidOid)
	{
		ArrayType  *result;

		result = (ArrayType *) DatumGetPointer(datumCopy(PointerGetDatum(src),
														 false, -1));
		ARR_ELEMTYPE(result) = my_extra->desttype;
		PG_RETURN_ARRAYTYPE_P(result);
	}

	/*
	 * Use array_map to apply the function to each array element.
	 *
	 * We pass on the desttypmod and isExplicit flags whether or not the
	 * function wants them.
	 */
	InitFunctionCallInfoData(locfcinfo, &my_extra->coerce_finfo, 3,
							 NULL, NULL);
	locfcinfo.arg[0] = PointerGetDatum(src);
	locfcinfo.arg[1] = Int32GetDatum(desttypmod);
	locfcinfo.arg[2] = BoolGetDatum(isExplicit);
	locfcinfo.argnull[0] = false;
	locfcinfo.argnull[1] = false;
	locfcinfo.argnull[2] = false;

	return array_map(&locfcinfo, my_extra->srctype, my_extra->desttype,
					 &my_extra->amstate);
}

/*
 * array_length_coerce -- apply the element type's length-coercion routine
 *		to each element of the given array.
 */
Datum
array_length_coerce(PG_FUNCTION_ARGS)
{
	ArrayType  *v = PG_GETARG_ARRAYTYPE_P(0);
	int32		desttypmod = PG_GETARG_INT32(1);
	bool		isExplicit = PG_GETARG_BOOL(2);
	FmgrInfo   *fmgr_info = fcinfo->flinfo;
	typedef struct
	{
		Oid			elemtype;
		FmgrInfo	coerce_finfo;
		ArrayMapState amstate;
	} alc_extra;
	alc_extra  *my_extra;
	FunctionCallInfoData locfcinfo;

	/* If no typmod is provided, shortcircuit the whole thing */
	if (desttypmod < 0)
		PG_RETURN_ARRAYTYPE_P(v);

	/*
	 * We arrange to look up the element type's coercion function only once
	 * per series of calls, assuming the element type doesn't change
	 * underneath us.
	 */
	my_extra = (alc_extra *) fmgr_info->fn_extra;
	if (my_extra == NULL)
	{
		fmgr_info->fn_extra = MemoryContextAllocZero(fmgr_info->fn_mcxt,
													 sizeof(alc_extra));
		my_extra = (alc_extra *) fmgr_info->fn_extra;
	}

	if (my_extra->elemtype != ARR_ELEMTYPE(v))
	{
		Oid			funcId;

		funcId = find_typmod_coercion_function(ARR_ELEMTYPE(v));

		if (OidIsValid(funcId))
			fmgr_info_cxt(funcId, &my_extra->coerce_finfo, fmgr_info->fn_mcxt);
		else
			my_extra->coerce_finfo.fn_oid = InvalidOid;
		my_extra->elemtype = ARR_ELEMTYPE(v);
	}

	/*
	 * If we didn't find a coercion function, return the array unmodified
	 * (this should not happen in the normal course of things, but might
	 * happen if this function is called manually).
	 */
	if (my_extra->coerce_finfo.fn_oid == InvalidOid)
		PG_RETURN_ARRAYTYPE_P(v);

	/*
	 * Use array_map to apply the function to each array element.
	 *
	 * Note: we pass isExplicit whether or not the function wants it ...
	 */
	InitFunctionCallInfoData(locfcinfo, &my_extra->coerce_finfo, 3,
							 NULL, NULL);
	locfcinfo.arg[0] = PointerGetDatum(v);
	locfcinfo.arg[1] = Int32GetDatum(desttypmod);
	locfcinfo.arg[2] = BoolGetDatum(isExplicit);
	locfcinfo.argnull[0] = false;
	locfcinfo.argnull[1] = false;
	locfcinfo.argnull[2] = false;

	return array_map(&locfcinfo, ARR_ELEMTYPE(v), ARR_ELEMTYPE(v),
					 &my_extra->amstate);
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
		astate->dvalues = (Datum *)
			palloc(ARRAY_ELEMS_CHUNKSIZE * sizeof(Datum));
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
		/* enlarge dvalues[] if needed */
		if ((astate->nelems % ARRAY_ELEMS_CHUNKSIZE) == 0)
			astate->dvalues = (Datum *)
				repalloc(astate->dvalues,
				   (astate->nelems + ARRAY_ELEMS_CHUNKSIZE) * sizeof(Datum));
	}

	if (disnull)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("null array elements not supported")));

	/* Use datumCopy to ensure pass-by-ref stuff is copied into mcontext */
	astate->dvalues[astate->nelems++] =
		datumCopy(dvalue, astate->typbyval, astate->typlen);

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

	return makeMdArrayResult(astate, 1, dims, lbs, rcontext);
}

/*
 * makeMdArrayResult - produce multi-D final result of accumArrayResult
 *
 * beware: no check that specified dimensions match the number of values
 * accumulated.
 *
 *	astate is working state (not NULL)
 *	rcontext is where to construct result
 */
Datum
makeMdArrayResult(ArrayBuildState *astate,
				  int ndims,
				  int *dims,
				  int *lbs,
				  MemoryContext rcontext)
{
	ArrayType  *result;
	MemoryContext oldcontext;

	/* Build the final array result in rcontext */
	oldcontext = MemoryContextSwitchTo(rcontext);

	result = construct_md_array(astate->dvalues,
								ndims,
								dims,
								lbs,
								astate->element_type,
								astate->typlen,
								astate->typbyval,
								astate->typalign);

	MemoryContextSwitchTo(oldcontext);

	/* Clean up all the junk */
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
