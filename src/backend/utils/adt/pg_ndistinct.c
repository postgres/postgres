/*-------------------------------------------------------------------------
 *
 * pg_ndistinct.c
 *		pg_ndistinct data type support.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/pg_ndistinct.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "common/int.h"
#include "common/jsonapi.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "nodes/miscnodes.h"
#include "statistics/extended_stats_internal.h"
#include "statistics/statistics_format.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"

/* Parsing state data */
typedef enum
{
	NDIST_EXPECT_START = 0,
	NDIST_EXPECT_ITEM,
	NDIST_EXPECT_KEY,
	NDIST_EXPECT_ATTNUM_LIST,
	NDIST_EXPECT_ATTNUM,
	NDIST_EXPECT_NDISTINCT,
	NDIST_EXPECT_COMPLETE,
} NDistinctSemanticState;

typedef struct
{
	const char *str;
	NDistinctSemanticState state;

	List	   *distinct_items; /* Accumulated complete MVNDistinctItems */
	Node	   *escontext;

	bool		found_attributes;	/* Item has "attributes" key */
	bool		found_ndistinct;	/* Item has "ndistinct" key */
	List	   *attnum_list;	/* Accumulated attribute numbers */
	int32		ndistinct;
} NDistinctParseState;

/*
 * Invoked at the start of each MVNDistinctItem.
 *
 * The entire JSON document should be one array of MVNDistinctItem objects.
 * If we are anywhere else in the document, it is an error.
 */
static JsonParseErrorType
ndistinct_object_start(void *state)
{
	NDistinctParseState *parse = state;

	switch (parse->state)
	{
		case NDIST_EXPECT_ITEM:
			/* Now we expect to see attributes/ndistinct keys */
			parse->state = NDIST_EXPECT_KEY;
			return JSON_SUCCESS;

		case NDIST_EXPECT_START:
			/* pg_ndistinct must begin with a '[' */
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_ndistinct: \"%s\"", parse->str),
					errdetail("Initial element must be an array."));
			break;

		case NDIST_EXPECT_KEY:
			/* In an object, expecting key */
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_ndistinct: \"%s\"", parse->str),
					errdetail("A key was expected."));
			break;

		case NDIST_EXPECT_ATTNUM_LIST:
			/* Just followed an "attributes" key */
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_ndistinct: \"%s\"", parse->str),
					errdetail("Value of \"%s\" must be an array of attribute numbers.",
							  PG_NDISTINCT_KEY_ATTRIBUTES));
			break;

		case NDIST_EXPECT_ATTNUM:
			/* In an attribute number list, expect only scalar integers */
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_ndistinct: \"%s\"", parse->str),
					errdetail("Attribute lists can only contain attribute numbers."));
			break;

		case NDIST_EXPECT_NDISTINCT:
			/* Just followed an "ndistinct" key */
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_ndistinct: \"%s\"", parse->str),
					errdetail("Value of \"%s\" must be an integer.",
							  PG_NDISTINCT_KEY_NDISTINCT));
			break;

		default:
			elog(ERROR,
				 "object start of \"%s\" found in unexpected parse state: %d.",
				 "pg_ndistinct", (int) parse->state);
			break;
	}

	return JSON_SEM_ACTION_FAILED;
}

/*
 * Invoked at the end of an object.
 *
 * Check to ensure that it was a complete MVNDistinctItem
 */
static JsonParseErrorType
ndistinct_object_end(void *state)
{
	NDistinctParseState *parse = state;

	int			natts = 0;

	MVNDistinctItem *item;

	if (parse->state != NDIST_EXPECT_KEY)
		elog(ERROR,
			 "object end of \"%s\" found in unexpected parse state: %d.",
			 "pg_ndistinct", (int) parse->state);

	if (!parse->found_attributes)
	{
		errsave(parse->escontext,
				errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				errmsg("malformed pg_ndistinct: \"%s\"", parse->str),
				errdetail("Item must contain \"%s\" key.",
						  PG_NDISTINCT_KEY_ATTRIBUTES));
		return JSON_SEM_ACTION_FAILED;
	}

	if (!parse->found_ndistinct)
	{
		errsave(parse->escontext,
				errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				errmsg("malformed pg_ndistinct: \"%s\"", parse->str),
				errdetail("Item must contain \"%s\" key.",
						  PG_NDISTINCT_KEY_NDISTINCT));
		return JSON_SEM_ACTION_FAILED;
	}

	/*
	 * We need at least two attribute numbers for a ndistinct item, anything
	 * less is malformed.
	 */
	natts = list_length(parse->attnum_list);
	if ((natts < 2) || (natts > STATS_MAX_DIMENSIONS))
	{
		errsave(parse->escontext,
				errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				errmsg("malformed pg_ndistinct: \"%s\"", parse->str),
				errdetail("The \"%s\" key must contain an array of at least %d and no more than %d attributes.",
						  PG_NDISTINCT_KEY_ATTRIBUTES, 2, STATS_MAX_DIMENSIONS));
		return JSON_SEM_ACTION_FAILED;
	}

	/* Create the MVNDistinctItem */
	item = palloc_object(MVNDistinctItem);
	item->nattributes = natts;
	item->attributes = palloc0(natts * sizeof(AttrNumber));
	item->ndistinct = (double) parse->ndistinct;

	for (int i = 0; i < natts; i++)
		item->attributes[i] = (AttrNumber) list_nth_int(parse->attnum_list, i);

	parse->distinct_items = lappend(parse->distinct_items, (void *) item);

	/* reset item state vars */
	list_free(parse->attnum_list);
	parse->attnum_list = NIL;
	parse->ndistinct = 0;
	parse->found_attributes = false;
	parse->found_ndistinct = false;

	/* Now we are looking for the next MVNDistinctItem */
	parse->state = NDIST_EXPECT_ITEM;
	return JSON_SUCCESS;
}


/*
 * Invoked at the start of an array.
 *
 * ndistinct input format has two types of arrays, the outer MVNDistinctItem
 * array and the attribute number array within each MVNDistinctItem.
 */
static JsonParseErrorType
ndistinct_array_start(void *state)
{
	NDistinctParseState *parse = state;

	switch (parse->state)
	{
		case NDIST_EXPECT_ATTNUM_LIST:
			parse->state = NDIST_EXPECT_ATTNUM;
			break;

		case NDIST_EXPECT_START:
			parse->state = NDIST_EXPECT_ITEM;
			break;

		default:
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_ndistinct: \"%s\"", parse->str),
					errdetail("Array has been found at an unexpected location."));
			return JSON_SEM_ACTION_FAILED;
	}

	return JSON_SUCCESS;
}


/*
 * Invoked at the end of an array.
 *
 * Arrays can never be empty.
 */
static JsonParseErrorType
ndistinct_array_end(void *state)
{
	NDistinctParseState *parse = state;

	switch (parse->state)
	{
		case NDIST_EXPECT_ATTNUM:
			if (list_length(parse->attnum_list) > 0)
			{
				/*
				 * The attribute number list is complete, look for more
				 * MVNDistinctItem keys.
				 */
				parse->state = NDIST_EXPECT_KEY;
				return JSON_SUCCESS;
			}

			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_ndistinct: \"%s\"", parse->str),
					errdetail("The \"%s\" key must be a non-empty array.",
							  PG_NDISTINCT_KEY_ATTRIBUTES));
			break;

		case NDIST_EXPECT_ITEM:
			if (list_length(parse->distinct_items) > 0)
			{
				/* Item list is complete, we are done. */
				parse->state = NDIST_EXPECT_COMPLETE;
				return JSON_SUCCESS;
			}

			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_ndistinct: \"%s\"", parse->str),
					errdetail("Item array cannot be empty."));
			break;

		default:

			/*
			 * This can only happen if a case was missed in
			 * ndistinct_array_start().
			 */
			elog(ERROR,
				 "array end of \"%s\" found in unexpected parse state: %d.",
				 "pg_ndistinct", (int) parse->state);
			break;
	}

	return JSON_SEM_ACTION_FAILED;
}

/*
 * Invoked at the start of a key/value field.
 *
 * The valid keys for the MVNDistinctItem object are:
 *   - attributes
 *   - ndistinct
 */
static JsonParseErrorType
ndistinct_object_field_start(void *state, char *fname, bool isnull)
{
	NDistinctParseState *parse = state;

	if (strcmp(fname, PG_NDISTINCT_KEY_ATTRIBUTES) == 0)
	{
		if (parse->found_attributes)
		{
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_ndistinct: \"%s\"", parse->str),
					errdetail("Multiple \"%s\" keys are not allowed.",
							  PG_NDISTINCT_KEY_ATTRIBUTES));
			return JSON_SEM_ACTION_FAILED;
		}
		parse->found_attributes = true;
		parse->state = NDIST_EXPECT_ATTNUM_LIST;
		return JSON_SUCCESS;
	}

	if (strcmp(fname, PG_NDISTINCT_KEY_NDISTINCT) == 0)
	{
		if (parse->found_ndistinct)
		{
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_ndistinct: \"%s\"", parse->str),
					errdetail("Multiple \"%s\" keys are not allowed.",
							  PG_NDISTINCT_KEY_NDISTINCT));
			return JSON_SEM_ACTION_FAILED;
		}
		parse->found_ndistinct = true;
		parse->state = NDIST_EXPECT_NDISTINCT;
		return JSON_SUCCESS;
	}

	errsave(parse->escontext,
			errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			errmsg("malformed pg_ndistinct: \"%s\"", parse->str),
			errdetail("Only allowed keys are \"%s\" and \"%s\".",
					  PG_NDISTINCT_KEY_ATTRIBUTES,
					  PG_NDISTINCT_KEY_NDISTINCT));
	return JSON_SEM_ACTION_FAILED;
}

/*
 * Invoked at the start of an array element.
 *
 * The overall structure of the datatype is an array, but there are also
 * arrays as the value of every attributes key.
 */
static JsonParseErrorType
ndistinct_array_element_start(void *state, bool isnull)
{
	const NDistinctParseState *parse = state;

	switch (parse->state)
	{
		case NDIST_EXPECT_ATTNUM:
			if (!isnull)
				return JSON_SUCCESS;

			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_ndistinct: \"%s\"", parse->str),
					errdetail("Attribute number array cannot be null."));
			break;

		case NDIST_EXPECT_ITEM:
			if (!isnull)
				return JSON_SUCCESS;

			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_ndistinct: \"%s\"", parse->str),
					errdetail("Item list elements cannot be null."));

			break;

		default:
			elog(ERROR,
				 "array element start of \"%s\" found in unexpected parse state: %d.",
				 "pg_ndistinct", (int) parse->state);
			break;
	}

	return JSON_SEM_ACTION_FAILED;
}

/*
 * Test for valid subsequent attribute number.
 *
 * If the previous value is positive, then current value must either be
 * greater than the previous value, or negative.
 *
 * If the previous value is negative, then the value must be less than
 * the previous value.
 *
 * Duplicate values are obviously not allowed, but that is already covered
 * by the rules listed above.
 */
static bool
valid_subsequent_attnum(AttrNumber prev, AttrNumber cur)
{
	Assert(prev != 0);

	if (prev > 0)
		return ((cur > prev) || (cur < 0));

	return (cur < prev);
}

/*
 * Handle scalar events from the ndistinct input parser.
 *
 * Override integer parse error messages and replace them with errors
 * specific to the context.
 */
static JsonParseErrorType
ndistinct_scalar(void *state, char *token, JsonTokenType tokentype)
{
	NDistinctParseState *parse = state;
	AttrNumber	attnum;
	ErrorSaveContext escontext = {T_ErrorSaveContext};

	switch (parse->state)
	{
		case NDIST_EXPECT_ATTNUM:
			attnum = pg_strtoint16_safe(token, (Node *) &escontext);

			if (escontext.error_occurred)
			{
				errsave(parse->escontext,
						errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						errmsg("malformed pg_ndistinct: \"%s\"", parse->str),
						errdetail("Key \"%s\" has an incorrect value.", PG_NDISTINCT_KEY_ATTRIBUTES));
				return JSON_SEM_ACTION_FAILED;
			}

			/*
			 * The attribute number cannot be zero a negative number beyond
			 * the number of the possible expressions.
			 */
			if (attnum == 0 || attnum < (0 - STATS_MAX_DIMENSIONS))
			{
				errsave(parse->escontext,
						errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						errmsg("malformed pg_ndistinct: \"%s\"", parse->str),
						errdetail("Invalid \"%s\" element has been found: %d.",
								  PG_NDISTINCT_KEY_ATTRIBUTES, attnum));
				return JSON_SEM_ACTION_FAILED;
			}

			if (list_length(parse->attnum_list) > 0)
			{
				const AttrNumber prev = llast_int(parse->attnum_list);

				if (!valid_subsequent_attnum(prev, attnum))
				{
					errsave(parse->escontext,
							errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							errmsg("malformed pg_ndistinct: \"%s\"", parse->str),
							errdetail("Invalid \"%s\" element has been found: %d cannot follow %d.",
									  PG_NDISTINCT_KEY_ATTRIBUTES, attnum, prev));
					return JSON_SEM_ACTION_FAILED;
				}
			}

			parse->attnum_list = lappend_int(parse->attnum_list, (int) attnum);
			return JSON_SUCCESS;

		case NDIST_EXPECT_NDISTINCT:

			/*
			 * While the structure dictates that ndistinct is a double
			 * precision floating point, it has always been an integer in the
			 * output generated.  Therefore, we parse it as an integer here.
			 */
			parse->ndistinct = pg_strtoint32_safe(token, (Node *) &escontext);

			if (!escontext.error_occurred)
			{
				parse->state = NDIST_EXPECT_KEY;
				return JSON_SUCCESS;
			}

			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_ndistinct: \"%s\"", parse->str),
					errdetail("Key \"%s\" has an incorrect value.",
							  PG_NDISTINCT_KEY_NDISTINCT));
			break;

		default:
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_ndistinct: \"%s\"", parse->str),
					errdetail("Unexpected scalar has been found."));
			break;
	}

	return JSON_SEM_ACTION_FAILED;
}

/*
 * Compare the attribute arrays of two MVNDistinctItem values,
 * looking for duplicate sets. Return true if a duplicate set is found.
 *
 * The arrays are required to be in canonical order (all positive numbers
 * in ascending order first, followed by all negative numbers in descending
 * order) so it's safe to compare the attrnums in order, stopping at the
 * first difference.
 */
static bool
item_attributes_eq(const MVNDistinctItem *a, const MVNDistinctItem *b)
{
	if (a->nattributes != b->nattributes)
		return false;

	for (int i = 0; i < a->nattributes; i++)
	{
		if (a->attributes[i] != b->attributes[i])
			return false;
	}

	return true;
}

/*
 * Ensure that an attribute number appears as one of the attribute numbers
 * in a MVNDistinctItem.
 */
static bool
item_has_attnum(const MVNDistinctItem *item, AttrNumber attnum)
{
	for (int i = 0; i < item->nattributes; i++)
	{
		if (attnum == item->attributes[i])
			return true;
	}
	return false;
}

/*
 * Ensure that the attributes in MVNDistinctItem A are a subset of the
 * reference MVNDistinctItem B.
 */
static bool
item_is_attnum_subset(const MVNDistinctItem *item,
					  const MVNDistinctItem *refitem)
{
	for (int i = 0; i < item->nattributes; i++)
	{
		if (!item_has_attnum(refitem, item->attributes[i]))
			return false;
	}
	return true;
}

/*
 * Generate a string representing an array of attribute numbers.
 *
 * Freeing the allocated string is the responsibility of the caller.
 */
static char *
item_attnum_list(const MVNDistinctItem *item)
{
	StringInfoData str;

	initStringInfo(&str);

	appendStringInfo(&str, "%d", item->attributes[0]);

	for (int i = 1; i < item->nattributes; i++)
		appendStringInfo(&str, ", %d", item->attributes[i]);

	return str.data;
}

/*
 * Attempt to build and serialize the MVNDistinct object.
 *
 * This can only be executed after the completion of the JSON parsing.
 *
 * In the event of an error, set the error context and return NULL.
 */
static bytea *
build_mvndistinct(NDistinctParseState *parse, char *str)
{
	MVNDistinct *ndistinct;
	int			nitems = list_length(parse->distinct_items);
	bytea	   *bytes;
	int			item_most_attrs = 0;
	int			item_most_attrs_idx = 0;

	switch (parse->state)
	{
		case NDIST_EXPECT_COMPLETE:

			/*
			 * Parsing has ended correctly and we should have a list of items.
			 * If we don't, something has been done wrong in one of the
			 * earlier parsing steps.
			 */
			if (nitems == 0)
				elog(ERROR,
					 "cannot have empty item list after parsing success.");
			break;

		case NDIST_EXPECT_START:
			/* blank */
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_ndistinct: \"%s\"", str),
					errdetail("Value cannot be empty."));
			return NULL;

		default:
			/* Unexpected end-state. */
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_ndistinct: \"%s\"", str),
					errdetail("Unexpected end state has been found: %d.", parse->state));
			return NULL;
	}

	ndistinct = palloc(offsetof(MVNDistinct, items) +
					   nitems * sizeof(MVNDistinctItem));

	ndistinct->magic = STATS_NDISTINCT_MAGIC;
	ndistinct->type = STATS_NDISTINCT_TYPE_BASIC;
	ndistinct->nitems = nitems;

	for (int i = 0; i < nitems; i++)
	{
		MVNDistinctItem *item = list_nth(parse->distinct_items, i);

		/*
		 * Ensure that this item does not duplicate the attributes of any
		 * pre-existing item.
		 */
		for (int j = 0; j < i; j++)
		{
			if (item_attributes_eq(item, &ndistinct->items[j]))
			{
				char	   *s = item_attnum_list(item);

				errsave(parse->escontext,
						errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						errmsg("malformed pg_ndistinct: \"%s\"", str),
						errdetail("Duplicated \"%s\" array has been found: [%s].",
								  PG_NDISTINCT_KEY_ATTRIBUTES, s));
				pfree(s);
				return NULL;
			}
		}

		ndistinct->items[i].ndistinct = item->ndistinct;
		ndistinct->items[i].nattributes = item->nattributes;

		/*
		 * This transfers free-ing responsibility from the distinct_items list
		 * to the ndistinct object.
		 */
		ndistinct->items[i].attributes = item->attributes;

		/*
		 * Keep track of the first longest attribute list. All other attribute
		 * lists must be a subset of this list.
		 */
		if (item->nattributes > item_most_attrs)
		{
			item_most_attrs = item->nattributes;
			item_most_attrs_idx = i;
		}
	}

	/*
	 * Verify that all the sets of attribute numbers are a proper subset of
	 * the longest set recorded.  This acts as an extra sanity check based on
	 * the input given.  Note that this still needs to be cross-checked with
	 * the extended statistics objects this would be assigned to, but it
	 * provides one extra layer of protection.
	 */
	for (int i = 0; i < nitems; i++)
	{
		if (i == item_most_attrs_idx)
			continue;

		if (!item_is_attnum_subset(&ndistinct->items[i],
								   &ndistinct->items[item_most_attrs_idx]))
		{
			const MVNDistinctItem *item = &ndistinct->items[i];
			const MVNDistinctItem *refitem = &ndistinct->items[item_most_attrs_idx];
			char	   *item_list = item_attnum_list(item);
			char	   *refitem_list = item_attnum_list(refitem);

			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_ndistinct: \"%s\"", str),
					errdetail("\"%s\" array [%s] must be a subset of array [%s].",
							  PG_NDISTINCT_KEY_ATTRIBUTES,
							  item_list, refitem_list));
			pfree(item_list);
			pfree(refitem_list);
			return NULL;
		}
	}

	bytes = statext_ndistinct_serialize(ndistinct);

	/*
	 * Free the attribute lists, before the ndistinct itself.
	 */
	for (int i = 0; i < nitems; i++)
		pfree(ndistinct->items[i].attributes);
	pfree(ndistinct);

	return bytes;
}

/*
 * pg_ndistinct_in
 *		input routine for type pg_ndistinct.
 */
Datum
pg_ndistinct_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	NDistinctParseState parse_state;
	JsonParseErrorType result;
	JsonLexContext *lex;
	JsonSemAction sem_action;
	bytea	   *bytes = NULL;

	/* initialize semantic state */
	parse_state.str = str;
	parse_state.state = NDIST_EXPECT_START;
	parse_state.distinct_items = NIL;
	parse_state.escontext = fcinfo->context;
	parse_state.found_attributes = false;
	parse_state.found_ndistinct = false;
	parse_state.attnum_list = NIL;
	parse_state.ndistinct = 0;

	/* set callbacks */
	sem_action.semstate = (void *) &parse_state;
	sem_action.object_start = ndistinct_object_start;
	sem_action.object_end = ndistinct_object_end;
	sem_action.array_start = ndistinct_array_start;
	sem_action.array_end = ndistinct_array_end;
	sem_action.object_field_start = ndistinct_object_field_start;
	sem_action.object_field_end = NULL;
	sem_action.array_element_start = ndistinct_array_element_start;
	sem_action.array_element_end = NULL;
	sem_action.scalar = ndistinct_scalar;

	lex = makeJsonLexContextCstringLen(NULL, str, strlen(str),
									   PG_UTF8, true);
	result = pg_parse_json(lex, &sem_action);
	freeJsonLexContext(lex);

	if (result == JSON_SUCCESS)
		bytes = build_mvndistinct(&parse_state, str);

	list_free(parse_state.attnum_list);
	list_free_deep(parse_state.distinct_items);

	if (bytes)
		PG_RETURN_BYTEA_P(bytes);

	/*
	 * If escontext already set, just use that. Anything else is a generic
	 * JSON parse error.
	 */
	if (!SOFT_ERROR_OCCURRED(parse_state.escontext))
		errsave(parse_state.escontext,
				errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				errmsg("malformed pg_ndistinct: \"%s\"", str),
				errdetail("Input data must be valid JSON."));

	PG_RETURN_NULL();
}

/*
 * pg_ndistinct_out
 *		output routine for type pg_ndistinct
 *
 * Produces a human-readable representation of the value.
 */
Datum
pg_ndistinct_out(PG_FUNCTION_ARGS)
{
	bytea	   *data = PG_GETARG_BYTEA_PP(0);
	MVNDistinct *ndist = statext_ndistinct_deserialize(data);
	int			i;
	StringInfoData str;

	initStringInfo(&str);
	appendStringInfoChar(&str, '[');

	for (i = 0; i < ndist->nitems; i++)
	{
		MVNDistinctItem item = ndist->items[i];

		if (i > 0)
			appendStringInfoString(&str, ", ");

		if (item.nattributes <= 0)
			elog(ERROR, "invalid zero-length attribute array in MVNDistinct");

		appendStringInfo(&str, "{\"" PG_NDISTINCT_KEY_ATTRIBUTES "\": [%d",
						 item.attributes[0]);

		for (int j = 1; j < item.nattributes; j++)
			appendStringInfo(&str, ", %d", item.attributes[j]);

		appendStringInfo(&str, "], \"" PG_NDISTINCT_KEY_NDISTINCT "\": %d}",
						 (int) item.ndistinct);
	}

	appendStringInfoChar(&str, ']');

	PG_RETURN_CSTRING(str.data);
}

/*
 * pg_ndistinct_recv
 *		binary input routine for type pg_ndistinct
 */
Datum
pg_ndistinct_recv(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "pg_ndistinct")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}

/*
 * pg_ndistinct_send
 *		binary output routine for type pg_ndistinct
 *
 * n-distinct is serialized into a bytea value, so let's send that.
 */
Datum
pg_ndistinct_send(PG_FUNCTION_ARGS)
{
	return byteasend(fcinfo);
}
