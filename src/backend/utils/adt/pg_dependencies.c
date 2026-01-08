/*-------------------------------------------------------------------------
 *
 * pg_dependencies.c
 *		pg_dependencies data type support.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/pg_dependencies.c
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
#include "utils/float.h"
#include "utils/fmgrprotos.h"

typedef enum
{
	DEPS_EXPECT_START = 0,
	DEPS_EXPECT_ITEM,
	DEPS_EXPECT_KEY,
	DEPS_EXPECT_ATTNUM_LIST,
	DEPS_EXPECT_ATTNUM,
	DEPS_EXPECT_DEPENDENCY,
	DEPS_EXPECT_DEGREE,
	DEPS_PARSE_COMPLETE,
} DependenciesSemanticState;

typedef struct
{
	const char *str;
	DependenciesSemanticState state;

	List	   *dependency_list;
	Node	   *escontext;

	bool		found_attributes;	/* Item has an attributes key */
	bool		found_dependency;	/* Item has an dependency key */
	bool		found_degree;	/* Item has degree key */
	List	   *attnum_list;	/* Accumulated attribute numbers */
	AttrNumber	dependency;
	double		degree;
} DependenciesParseState;

/*
 * Invoked at the start of each MVDependency object.
 *
 * The entire JSON document should be one array of MVDependency objects.
 *
 * If we are anywhere else in the document, it's an error.
 */
static JsonParseErrorType
dependencies_object_start(void *state)
{
	DependenciesParseState *parse = state;

	switch (parse->state)
	{
		case DEPS_EXPECT_ITEM:
			/* Now we expect to see attributes/dependency/degree keys */
			parse->state = DEPS_EXPECT_KEY;
			return JSON_SUCCESS;

		case DEPS_EXPECT_START:
			/* pg_dependencies must begin with a '[' */
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_dependencies: \"%s\"", parse->str),
					errdetail("Initial element must be an array."));
			break;

		case DEPS_EXPECT_KEY:
			/* In an object, expecting key */
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_dependencies: \"%s\"", parse->str),
					errdetail("A key was expected."));
			break;

		case DEPS_EXPECT_ATTNUM_LIST:
			/* Just followed an "attributes": key */
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_dependencies: \"%s\"", parse->str),
					errdetail("Value of \"%s\" must be an array of attribute numbers.",
							  PG_DEPENDENCIES_KEY_ATTRIBUTES));
			break;

		case DEPS_EXPECT_ATTNUM:
			/* In an attribute number list, expect only scalar integers */
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_dependencies: \"%s\"", parse->str),
					errdetail("Attribute lists can only contain attribute numbers."));
			break;

		case DEPS_EXPECT_DEPENDENCY:
			/* Just followed a "dependency" key */
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_dependencies: \"%s\"", parse->str),
					errdetail("Value of \"%s\" must be an integer.",
							  PG_DEPENDENCIES_KEY_DEPENDENCY));
			break;

		case DEPS_EXPECT_DEGREE:
			/* Just followed a "degree" key */
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_dependencies: \"%s\"", parse->str),
					errdetail("Value of \"%s\" must be an integer.",
							  PG_DEPENDENCIES_KEY_DEGREE));
			break;

		default:
			elog(ERROR,
				 "object start of \"%s\" found in unexpected parse state: %d.",
				 "pg_dependencies", (int) parse->state);
			break;
	}

	return JSON_SEM_ACTION_FAILED;
}

/*
 * Invoked at the end of an object.
 *
 * Handle the end of an MVDependency object's JSON representation.
 */
static JsonParseErrorType
dependencies_object_end(void *state)
{
	DependenciesParseState *parse = state;

	MVDependency *dep;

	int			natts = 0;

	if (parse->state != DEPS_EXPECT_KEY)
		elog(ERROR,
			 "object end of \"%s\" found in unexpected parse state: %d.",
			 "pg_dependencies", (int) parse->state);

	if (!parse->found_attributes)
	{
		errsave(parse->escontext,
				errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				errmsg("malformed pg_dependencies: \"%s\"", parse->str),
				errdetail("Item must contain \"%s\" key.",
						  PG_DEPENDENCIES_KEY_ATTRIBUTES));
		return JSON_SEM_ACTION_FAILED;
	}

	if (!parse->found_dependency)
	{
		errsave(parse->escontext,
				errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				errmsg("malformed pg_dependencies: \"%s\"", parse->str),
				errdetail("Item must contain \"%s\" key.",
						  PG_DEPENDENCIES_KEY_DEPENDENCY));
		return JSON_SEM_ACTION_FAILED;
	}

	if (!parse->found_degree)
	{
		errsave(parse->escontext,
				errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				errmsg("malformed pg_dependencies: \"%s\"", parse->str),
				errdetail("Item must contain \"%s\" key.",
						  PG_DEPENDENCIES_KEY_DEGREE));
		return JSON_SEM_ACTION_FAILED;
	}

	/*
	 * We need at least one attribute number in a dependencies item, anything
	 * less is malformed.
	 */
	natts = list_length(parse->attnum_list);
	if ((natts < 1) || (natts > (STATS_MAX_DIMENSIONS - 1)))
	{
		errsave(parse->escontext,
				errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				errmsg("malformed pg_dependencies: \"%s\"", parse->str),
				errdetail("The \"%s\" key must contain an array of at least %d and no more than %d elements.",
						  PG_DEPENDENCIES_KEY_ATTRIBUTES, 1,
						  STATS_MAX_DIMENSIONS - 1));
		return JSON_SEM_ACTION_FAILED;
	}

	/*
	 * Allocate enough space for the dependency, the attribute numbers in the
	 * list and the final attribute number for the dependency.
	 */
	dep = palloc0(offsetof(MVDependency, attributes) + ((natts + 1) * sizeof(AttrNumber)));
	dep->nattributes = natts + 1;

	dep->attributes[natts] = parse->dependency;
	dep->degree = parse->degree;

	/*
	 * Assign attribute numbers to the attributes array, comparing each one
	 * against the dependency attribute to ensure that there there are no
	 * matches.
	 */
	for (int i = 0; i < natts; i++)
	{
		dep->attributes[i] = (AttrNumber) list_nth_int(parse->attnum_list, i);
		if (dep->attributes[i] == parse->dependency)
		{
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_dependencies: \"%s\"", parse->str),
					errdetail("Item \"%s\" with value %d has been found in the \"%s\" list.",
							  PG_DEPENDENCIES_KEY_DEPENDENCY, parse->dependency,
							  PG_DEPENDENCIES_KEY_ATTRIBUTES));
			return JSON_SEM_ACTION_FAILED;
		}
	}

	parse->dependency_list = lappend(parse->dependency_list, (void *) dep);

	/*
	 * Reset dependency item state variables to look for the next
	 * MVDependency.
	 */
	list_free(parse->attnum_list);
	parse->attnum_list = NIL;
	parse->dependency = 0;
	parse->degree = 0.0;
	parse->found_attributes = false;
	parse->found_dependency = false;
	parse->found_degree = false;
	parse->state = DEPS_EXPECT_ITEM;

	return JSON_SUCCESS;
}

/*
 * Invoked at the start of an array.
 *
 * Dependency input format does not have arrays, so any array elements
 * encountered are an error.
 */
static JsonParseErrorType
dependencies_array_start(void *state)
{
	DependenciesParseState *parse = state;

	switch (parse->state)
	{
		case DEPS_EXPECT_ATTNUM_LIST:
			parse->state = DEPS_EXPECT_ATTNUM;
			break;
		case DEPS_EXPECT_START:
			parse->state = DEPS_EXPECT_ITEM;
			break;
		default:
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_dependencies: \"%s\"", parse->str),
					errdetail("Array has been found at an unexpected location."));
			return JSON_SEM_ACTION_FAILED;
	}

	return JSON_SUCCESS;
}

/*
 * Invoked at the end of an array.
 *
 * Either the end of an attribute number list or the whole object.
 */
static JsonParseErrorType
dependencies_array_end(void *state)
{
	DependenciesParseState *parse = state;

	switch (parse->state)
	{
		case DEPS_EXPECT_ATTNUM:
			if (list_length(parse->attnum_list) > 0)
			{
				parse->state = DEPS_EXPECT_KEY;
				return JSON_SUCCESS;
			}

			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_dependencies: \"%s\"", parse->str),
					errdetail("The \"%s\" key must be a non-empty array.",
							  PG_DEPENDENCIES_KEY_ATTRIBUTES));
			break;

		case DEPS_EXPECT_ITEM:
			if (list_length(parse->dependency_list) > 0)
			{
				parse->state = DEPS_PARSE_COMPLETE;
				return JSON_SUCCESS;
			}

			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_dependencies: \"%s\"", parse->str),
					errdetail("Item array cannot be empty."));
			break;

		default:

			/*
			 * This can only happen if a case was missed in
			 * dependencies_array_start().
			 */
			elog(ERROR,
				 "array end of \"%s\" found in unexpected parse state: %d.",
				 "pg_dependencies", (int) parse->state);
			break;
	}
	return JSON_SEM_ACTION_FAILED;
}

/*
 * Invoked at the start of a key/value field.
 *
 * The valid keys for the MVDependency object are:
 *   - attributes
 *   - dependency
 *   - degree
 */
static JsonParseErrorType
dependencies_object_field_start(void *state, char *fname, bool isnull)
{
	DependenciesParseState *parse = state;

	if (strcmp(fname, PG_DEPENDENCIES_KEY_ATTRIBUTES) == 0)
	{
		if (parse->found_attributes)
		{
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_dependencies: \"%s\"", parse->str),
					errdetail("Multiple \"%s\" keys are not allowed.",
							  PG_DEPENDENCIES_KEY_ATTRIBUTES));
			return JSON_SEM_ACTION_FAILED;
		}

		parse->found_attributes = true;
		parse->state = DEPS_EXPECT_ATTNUM_LIST;
		return JSON_SUCCESS;
	}

	if (strcmp(fname, PG_DEPENDENCIES_KEY_DEPENDENCY) == 0)
	{
		if (parse->found_dependency)
		{
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_dependencies: \"%s\"", parse->str),
					errdetail("Multiple \"%s\" keys are not allowed.",
							  PG_DEPENDENCIES_KEY_DEPENDENCY));
			return JSON_SEM_ACTION_FAILED;
		}

		parse->found_dependency = true;
		parse->state = DEPS_EXPECT_DEPENDENCY;
		return JSON_SUCCESS;
	}

	if (strcmp(fname, PG_DEPENDENCIES_KEY_DEGREE) == 0)
	{
		if (parse->found_degree)
		{
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_dependencies: \"%s\"", parse->str),
					errdetail("Multiple \"%s\" keys are not allowed.",
							  PG_DEPENDENCIES_KEY_DEGREE));
			return JSON_SEM_ACTION_FAILED;
		}

		parse->found_degree = true;
		parse->state = DEPS_EXPECT_DEGREE;
		return JSON_SUCCESS;
	}

	errsave(parse->escontext,
			errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			errmsg("malformed pg_dependencies: \"%s\"", parse->str),
			errdetail("Only allowed keys are \"%s\", \"%s\" and \"%s\".",
					  PG_DEPENDENCIES_KEY_ATTRIBUTES,
					  PG_DEPENDENCIES_KEY_DEPENDENCY,
					  PG_DEPENDENCIES_KEY_DEGREE));
	return JSON_SEM_ACTION_FAILED;
}

/*
 * Invoked at the start of an array element.
 *
 * pg_dependencies input format does not have arrays, so any array elements
 * encountered are an error.
 */
static JsonParseErrorType
dependencies_array_element_start(void *state, bool isnull)
{
	DependenciesParseState *parse = state;

	switch (parse->state)
	{
		case DEPS_EXPECT_ATTNUM:
			if (!isnull)
				return JSON_SUCCESS;

			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_dependencies: \"%s\"", parse->str),
					errdetail("Attribute number array cannot be null."));
			break;

		case DEPS_EXPECT_ITEM:
			if (!isnull)
				return JSON_SUCCESS;

			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_dependencies: \"%s\"", parse->str),
					errdetail("Item list elements cannot be null."));
			break;

		default:
			elog(ERROR,
				 "array element start of \"%s\" found in unexpected parse state: %d.",
				 "pg_dependencies", (int) parse->state);
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
 * Duplicate values are not allowed; that is already covered by the rules
 * described above.
 */
static bool
valid_subsequent_attnum(const AttrNumber prev, const AttrNumber cur)
{
	Assert(prev != 0);

	if (prev > 0)
		return ((cur > prev) || (cur < 0));

	return (cur < prev);
}

/*
 * Handle scalar events from the dependencies input parser.
 *
 * There is only one case where we will encounter a scalar, and that is the
 * dependency degree for the previous object key.
 */
static JsonParseErrorType
dependencies_scalar(void *state, char *token, JsonTokenType tokentype)
{
	DependenciesParseState *parse = state;
	AttrNumber	attnum;
	ErrorSaveContext escontext = {T_ErrorSaveContext};

	switch (parse->state)
	{
		case DEPS_EXPECT_ATTNUM:
			attnum = pg_strtoint16_safe(token, (Node *) &escontext);

			if (escontext.error_occurred)
			{
				errsave(parse->escontext,
						errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						errmsg("malformed pg_dependencies: \"%s\"", parse->str),
						errdetail("Key \"%s\" has an incorrect value.", PG_DEPENDENCIES_KEY_ATTRIBUTES));
				return JSON_SEM_ACTION_FAILED;
			}

			/*
			 * An attribute number cannot be zero or a negative number beyond
			 * the number of the possible expressions.
			 */
			if (attnum == 0 || attnum < (0 - STATS_MAX_DIMENSIONS))
			{
				errsave(parse->escontext,
						errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						errmsg("malformed pg_dependencies: \"%s\"", parse->str),
						errdetail("Invalid \"%s\" element has been found: %d.",
								  PG_DEPENDENCIES_KEY_ATTRIBUTES, attnum));
				return JSON_SEM_ACTION_FAILED;
			}

			if (parse->attnum_list != NIL)
			{
				const AttrNumber prev = llast_int(parse->attnum_list);

				if (!valid_subsequent_attnum(prev, attnum))
				{
					errsave(parse->escontext,
							errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							errmsg("malformed pg_dependencies: \"%s\"", parse->str),
							errdetail("Invalid \"%s\" element has been found: %d cannot follow %d.",
									  PG_DEPENDENCIES_KEY_ATTRIBUTES, attnum, prev));
					return JSON_SEM_ACTION_FAILED;
				}
			}

			parse->attnum_list = lappend_int(parse->attnum_list, (int) attnum);
			return JSON_SUCCESS;

		case DEPS_EXPECT_DEPENDENCY:
			parse->dependency = (AttrNumber)
				pg_strtoint16_safe(token, (Node *) &escontext);

			if (escontext.error_occurred)
			{
				errsave(parse->escontext,
						errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						errmsg("malformed pg_dependencies: \"%s\"", parse->str),
						errdetail("Key \"%s\" has an incorrect value.", PG_DEPENDENCIES_KEY_DEPENDENCY));
				return JSON_SEM_ACTION_FAILED;
			}

			/*
			 * The dependency attribute number cannot be zero or a negative
			 * number beyond the number of the possible expressions.
			 */
			if (parse->dependency == 0 || parse->dependency < (0 - STATS_MAX_DIMENSIONS))
			{
				errsave(parse->escontext,
						errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						errmsg("malformed pg_dependencies: \"%s\"", parse->str),
						errdetail("Key \"%s\" has an incorrect value: %d.",
								  PG_DEPENDENCIES_KEY_DEPENDENCY, parse->dependency));
				return JSON_SEM_ACTION_FAILED;
			}

			parse->state = DEPS_EXPECT_KEY;
			return JSON_SUCCESS;

		case DEPS_EXPECT_DEGREE:
			parse->degree = float8in_internal(token, NULL, "double",
											  token, (Node *) &escontext);

			if (escontext.error_occurred)
			{
				errsave(parse->escontext,
						errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						errmsg("malformed pg_dependencies: \"%s\"", parse->str),
						errdetail("Key \"%s\" has an incorrect value.", PG_DEPENDENCIES_KEY_DEGREE));
				return JSON_SEM_ACTION_FAILED;
			}

			parse->state = DEPS_EXPECT_KEY;
			return JSON_SUCCESS;

		default:
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_dependencies: \"%s\"", parse->str),
					errdetail("Unexpected scalar has been found."));
			break;
	}

	return JSON_SEM_ACTION_FAILED;
}

/*
 * Compare the attribute arrays of two MVDependency values,
 * looking for duplicated sets.
 */
static bool
dep_attributes_eq(const MVDependency *a, const MVDependency *b)
{
	int			i;

	if (a->nattributes != b->nattributes)
		return false;

	for (i = 0; i < a->nattributes; i++)
	{
		if (a->attributes[i] != b->attributes[i])
			return false;
	}

	return true;
}

/*
 * Generate a string representing an array of attribute numbers.
 * Internally, the dependency attribute is the last element, so we
 * leave that off.
 *
 * Freeing the allocated string is the responsibility of the caller.
 */
static char *
dep_attnum_list(const MVDependency *item)
{
	StringInfoData str;

	initStringInfo(&str);

	appendStringInfo(&str, "%d", item->attributes[0]);

	for (int i = 1; i < item->nattributes - 1; i++)
		appendStringInfo(&str, ", %d", item->attributes[i]);

	return str.data;
}

/*
 * Return the dependency, which is the last attribute element.
 */
static AttrNumber
dep_attnum_dependency(const MVDependency *item)
{
	return item->attributes[item->nattributes - 1];
}

/*
 * Attempt to build and serialize the MVDependencies object.
 *
 * This can only be executed after the completion of the JSON parsing.
 *
 * In the event of an error, set the error context and return NULL.
 */
static bytea *
build_mvdependencies(DependenciesParseState *parse, char *str)
{
	int			ndeps = list_length(parse->dependency_list);

	MVDependencies *mvdeps;
	bytea	   *bytes;

	switch (parse->state)
	{
		case DEPS_PARSE_COMPLETE:

			/*
			 * Parse ended in the expected place.  We should have a list of
			 * items, but if we do not there is an issue with one of the
			 * earlier parse steps.
			 */
			if (ndeps == 0)
				elog(ERROR,
					 "pg_dependencies parsing claims success with an empty item list.");
			break;

		case DEPS_EXPECT_START:
			/* blank */
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_dependencies: \"%s\"", str),
					errdetail("Value cannot be empty."));
			return NULL;

		default:
			/* Unexpected end-state. */
			errsave(parse->escontext,
					errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					errmsg("malformed pg_dependencies: \"%s\"", str),
					errdetail("Unexpected end state has been found: %d.", parse->state));
			return NULL;
	}

	mvdeps = palloc0(offsetof(MVDependencies, deps)
					 + (ndeps * sizeof(MVDependency *)));
	mvdeps->magic = STATS_DEPS_MAGIC;
	mvdeps->type = STATS_DEPS_TYPE_BASIC;
	mvdeps->ndeps = ndeps;

	for (int i = 0; i < ndeps; i++)
	{
		/*
		 * Use the MVDependency objects in the dependency_list.
		 *
		 * Because we free the dependency_list after parsing is done, we
		 * cannot free it here.
		 */
		mvdeps->deps[i] = list_nth(parse->dependency_list, i);

		/*
		 * Ensure that this item does not duplicate the attributes of any
		 * pre-existing item.
		 */
		for (int j = 0; j < i; j++)
		{
			if (dep_attributes_eq(mvdeps->deps[i], mvdeps->deps[j]))
			{
				MVDependency *dep = mvdeps->deps[i];
				char	   *attnum_list = dep_attnum_list(dep);
				AttrNumber	attnum_dep = dep_attnum_dependency(dep);

				errsave(parse->escontext,
						errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						errmsg("malformed pg_dependencies: \"%s\"", str),
						errdetail("Duplicated \"%s\" array has been found: [%s] for key \"%s\" and value %d.",
								  PG_DEPENDENCIES_KEY_ATTRIBUTES, attnum_list,
								  PG_DEPENDENCIES_KEY_DEPENDENCY, attnum_dep));
				pfree(mvdeps);
				return NULL;
			}
		}
	}

	bytes = statext_dependencies_serialize(mvdeps);

	/*
	 * No need to free the individual MVDependency objects, because they are
	 * still in the dependency_list, and will be freed with that.
	 */
	pfree(mvdeps);

	return bytes;
}


/*
 * pg_dependencies_in		- input routine for type pg_dependencies.
 *
 * This format is valid JSON, with the expected format:
 *    [{"attributes": [1,2], "dependency": -1, "degree": 1.0000},
 *     {"attributes": [1,-1], "dependency": 2, "degree": 0.0000},
 *     {"attributes": [2,-1], "dependency": 1, "degree": 1.0000}]
 *
 */
Datum
pg_dependencies_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	bytea	   *bytes = NULL;

	DependenciesParseState parse_state;
	JsonParseErrorType result;
	JsonLexContext *lex;
	JsonSemAction sem_action;

	/* initialize the semantic state */
	parse_state.str = str;
	parse_state.state = DEPS_EXPECT_START;
	parse_state.dependency_list = NIL;
	parse_state.attnum_list = NIL;
	parse_state.dependency = 0;
	parse_state.degree = 0.0;
	parse_state.found_attributes = false;
	parse_state.found_dependency = false;
	parse_state.found_degree = false;
	parse_state.escontext = fcinfo->context;

	/* set callbacks */
	sem_action.semstate = (void *) &parse_state;
	sem_action.object_start = dependencies_object_start;
	sem_action.object_end = dependencies_object_end;
	sem_action.array_start = dependencies_array_start;
	sem_action.array_end = dependencies_array_end;
	sem_action.array_element_start = dependencies_array_element_start;
	sem_action.array_element_end = NULL;
	sem_action.object_field_start = dependencies_object_field_start;
	sem_action.object_field_end = NULL;
	sem_action.scalar = dependencies_scalar;

	lex = makeJsonLexContextCstringLen(NULL, str, strlen(str), PG_UTF8, true);

	result = pg_parse_json(lex, &sem_action);
	freeJsonLexContext(lex);

	if (result == JSON_SUCCESS)
		bytes = build_mvdependencies(&parse_state, str);

	list_free_deep(parse_state.dependency_list);
	list_free(parse_state.attnum_list);

	if (bytes)
		PG_RETURN_BYTEA_P(bytes);

	/*
	 * If escontext already set, just use that. Anything else is a generic
	 * JSON parse error.
	 */
	if (!SOFT_ERROR_OCCURRED(parse_state.escontext))
		errsave(parse_state.escontext,
				errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				errmsg("malformed pg_dependencies: \"%s\"", str),
				errdetail("Input data must be valid JSON."));

	PG_RETURN_NULL();
}


/*
 * pg_dependencies_out		- output routine for type pg_dependencies.
 */
Datum
pg_dependencies_out(PG_FUNCTION_ARGS)
{
	bytea	   *data = PG_GETARG_BYTEA_PP(0);
	MVDependencies *dependencies = statext_dependencies_deserialize(data);
	StringInfoData str;

	initStringInfo(&str);
	appendStringInfoChar(&str, '[');

	for (int i = 0; i < dependencies->ndeps; i++)
	{
		MVDependency *dependency = dependencies->deps[i];

		if (i > 0)
			appendStringInfoString(&str, ", ");

		if (dependency->nattributes <= 1)
			elog(ERROR, "invalid zero-length nattributes array in MVDependencies");

		appendStringInfo(&str, "{\"" PG_DEPENDENCIES_KEY_ATTRIBUTES "\": [%d",
						 dependency->attributes[0]);

		for (int j = 1; j < dependency->nattributes - 1; j++)
			appendStringInfo(&str, ", %d", dependency->attributes[j]);

		appendStringInfo(&str, "], \"" PG_DEPENDENCIES_KEY_DEPENDENCY "\": %d, "
						 "\"" PG_DEPENDENCIES_KEY_DEGREE "\": %f}",
						 dependency->attributes[dependency->nattributes - 1],
						 dependency->degree);
	}

	appendStringInfoChar(&str, ']');

	PG_RETURN_CSTRING(str.data);
}

/*
 * pg_dependencies_recv		- binary input routine for type pg_dependencies.
 */
Datum
pg_dependencies_recv(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "pg_dependencies")));

	PG_RETURN_VOID();			/* keep compiler quiet */
}

/*
 * pg_dependencies_send		- binary output routine for type pg_dependencies.
 *
 * Functional dependencies are serialized in a bytea value (although the type
 * is named differently), so let's just send that.
 */
Datum
pg_dependencies_send(PG_FUNCTION_ARGS)
{
	return byteasend(fcinfo);
}
