/*-------------------------------------------------------------------------
 *
 * reloptions.c
 *	  Core support for relation options (pg_class.reloptions)
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/common/reloptions.c,v 1.15 2009/01/06 03:15:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gist_private.h"
#include "access/hash.h"
#include "access/nbtree.h"
#include "access/reloptions.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * Contents of pg_class.reloptions
 *
 * To add an option:
 *
 * (i) decide on a class (integer, real, bool, string), name, default value,
 * upper and lower bounds (if applicable).
 * (ii) add a record below.
 * (iii) add it to StdRdOptions if appropriate
 * (iv) add a block to the appropriate handling routine (probably
 * default_reloptions)
 * (v) don't forget to document the option
 *
 * Note that we don't handle "oids" in relOpts because it is handled by
 * interpretOidsOption().
 */

static relopt_bool boolRelOpts[] =
{
	/* list terminator */
	{ { NULL } }
};

static relopt_int intRelOpts[] =
{
	{
		{
			"fillfactor",
			"Packs table pages only to this percentage",
			RELOPT_KIND_HEAP
		},
		HEAP_DEFAULT_FILLFACTOR, HEAP_MIN_FILLFACTOR, 100
	},
	{
		{
			"fillfactor",
			"Packs btree index pages only to this percentage",
			RELOPT_KIND_BTREE
		},
		BTREE_DEFAULT_FILLFACTOR, BTREE_MIN_FILLFACTOR, 100
	},
	{
		{
			"fillfactor",
			"Packs hash index pages only to this percentage",
			RELOPT_KIND_HASH
		},
		HASH_DEFAULT_FILLFACTOR, HASH_MIN_FILLFACTOR, 100
	},
	{
		{
			"fillfactor",
			"Packs gist index pages only to this percentage",
			RELOPT_KIND_GIST
		},
		GIST_DEFAULT_FILLFACTOR, GIST_MIN_FILLFACTOR, 100
	},
	/* list terminator */
	{ { NULL } }
};

static relopt_real realRelOpts[] =
{
	/* list terminator */
	{ { NULL } }
};

static relopt_string stringRelOpts[] = 
{
	/* list terminator */
	{ { NULL } }
};

static relopt_gen **relOpts = NULL;
static int last_assigned_kind = RELOPT_KIND_LAST_DEFAULT + 1;

static int		num_custom_options = 0;
static relopt_gen **custom_options = NULL;
static bool		need_initialization = true;

static void initialize_reloptions(void);
static void parse_one_reloption(relopt_value *option, char *text_str,
					int text_len, bool validate);

/*
 * initialize_reloptions
 * 		initialization routine, must be called before parsing
 *
 * Initialize the relOpts array and fill each variable's type and name length.
 */
static void
initialize_reloptions(void)
{
	int		i;
	int		j = 0;

	for (i = 0; boolRelOpts[i].gen.name; i++)
		j++;
	for (i = 0; intRelOpts[i].gen.name; i++)
		j++;
	for (i = 0; realRelOpts[i].gen.name; i++)
		j++;
	for (i = 0; stringRelOpts[i].gen.name; i++)
		j++;
	j += num_custom_options;

	if (relOpts)
		pfree(relOpts);
	relOpts = MemoryContextAlloc(TopMemoryContext,
								 (j + 1) * sizeof(relopt_gen *));

	j = 0;
	for (i = 0; boolRelOpts[i].gen.name; i++)
	{
		relOpts[j] = &boolRelOpts[i].gen;
		relOpts[j]->type = RELOPT_TYPE_BOOL;
		relOpts[j]->namelen = strlen(relOpts[j]->name);
		j++;
	}

	for (i = 0; intRelOpts[i].gen.name; i++)
	{
		relOpts[j] = &intRelOpts[i].gen;
		relOpts[j]->type = RELOPT_TYPE_INT;
		relOpts[j]->namelen = strlen(relOpts[j]->name);
		j++;
	}

	for (i = 0; realRelOpts[i].gen.name; i++)
	{
		relOpts[j] = &realRelOpts[i].gen;
		relOpts[j]->type = RELOPT_TYPE_REAL;
		relOpts[j]->namelen = strlen(relOpts[j]->name);
		j++;
	}

	for (i = 0; stringRelOpts[i].gen.name; i++)
	{
		relOpts[j] = &stringRelOpts[i].gen;
		relOpts[j]->type = RELOPT_TYPE_STRING;
		relOpts[j]->namelen = strlen(relOpts[j]->name);
		j++;
	}

	for (i = 0; i < num_custom_options; i++)
	{
		relOpts[j] = custom_options[i];
		j++;
	}

	/* add a list terminator */
	relOpts[j] = NULL;
}

/*
 * add_reloption_kind
 * 		Create a new relopt_kind value, to be used in custom reloptions by
 * 		user-defined AMs.
 */
int
add_reloption_kind(void)
{
	if (last_assigned_kind >= RELOPT_KIND_MAX)
		ereport(ERROR,
				(errmsg("user-defined relation parameter types limit exceeded")));

	return last_assigned_kind++;
}

/*
 * add_reloption
 * 		Add an already-created custom reloption to the list, and recompute the
 * 		main parser table.
 */
static void
add_reloption(relopt_gen *newoption)
{
	static int		max_custom_options = 0;

	if (num_custom_options >= max_custom_options)
	{
		MemoryContext	oldcxt;

		oldcxt = MemoryContextSwitchTo(TopMemoryContext);

		if (max_custom_options == 0)
		{
			max_custom_options = 8;
			custom_options = palloc(max_custom_options * sizeof(relopt_gen *));
		}
		else
		{
			max_custom_options *= 2;
			custom_options = repalloc(custom_options,
									  max_custom_options * sizeof(relopt_gen *));
		}
		MemoryContextSwitchTo(oldcxt);
	}
	custom_options[num_custom_options++] = newoption;

	need_initialization = true;
}

/*
 * allocate_reloption
 * 		Allocate a new reloption and initialize the type-agnostic fields
 * 		(for types other than string)
 */
static relopt_gen *
allocate_reloption(int kind, int type, char *name, char *desc)
{
	MemoryContext	oldcxt;
	size_t			size;
	relopt_gen	   *newoption;

	Assert(type != RELOPT_TYPE_STRING);

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);

	switch (type)
	{
		case RELOPT_TYPE_BOOL:
			size = sizeof(relopt_bool);
			break;
		case RELOPT_TYPE_INT:
			size = sizeof(relopt_int);
			break;
		case RELOPT_TYPE_REAL:
			size = sizeof(relopt_real);
			break;
		default:
			elog(ERROR, "unsupported option type");
			return NULL;	/* keep compiler quiet */
	}

	newoption = palloc(size);

	newoption->name = pstrdup(name);
	if (desc)
		newoption->desc = pstrdup(desc);
	else
		newoption->desc = NULL;
	newoption->kind = kind;
	newoption->namelen = strlen(name);
	newoption->type = type;

	MemoryContextSwitchTo(oldcxt);

	return newoption;
}

/*
 * add_bool_reloption
 * 		Add a new boolean reloption
 */
void
add_bool_reloption(int kind, char *name, char *desc, bool default_val)
{
	relopt_bool	   *newoption;

	newoption = (relopt_bool *) allocate_reloption(kind, RELOPT_TYPE_BOOL,
												   name, desc);
	newoption->default_val = default_val;

	add_reloption((relopt_gen *) newoption);
}

/*
 * add_int_reloption
 * 		Add a new integer reloption
 */
void
add_int_reloption(int kind, char *name, char *desc, int default_val,
				  int min_val, int max_val)
{
	relopt_int	   *newoption;

	newoption = (relopt_int *) allocate_reloption(kind, RELOPT_TYPE_INT,
												  name, desc);
	newoption->default_val = default_val;
	newoption->min = min_val;
	newoption->max = max_val;

	add_reloption((relopt_gen *) newoption);
}

/*
 * add_real_reloption
 * 		Add a new float reloption
 */
void
add_real_reloption(int kind, char *name, char *desc, double default_val,
				  double min_val, double max_val)
{
	relopt_real	   *newoption;

	newoption = (relopt_real *) allocate_reloption(kind, RELOPT_TYPE_REAL,
												   name, desc);
	newoption->default_val = default_val;
	newoption->min = min_val;
	newoption->max = max_val;

	add_reloption((relopt_gen *) newoption);
}

/*
 * add_string_reloption
 *		Add a new string reloption
 */
void
add_string_reloption(int kind, char *name, char *desc, char *default_val)
{
	MemoryContext	oldcxt;
	relopt_string  *newoption;
	int				default_len = 0;

	oldcxt = MemoryContextSwitchTo(TopMemoryContext);

	if (default_val)
		default_len = strlen(default_val);

	newoption = palloc0(sizeof(relopt_string) + default_len);

	newoption->gen.name = pstrdup(name);
	if (desc)
		newoption->gen.desc = pstrdup(desc);
	else
		newoption->gen.desc = NULL;
	newoption->gen.kind = kind;
	newoption->gen.namelen = strlen(name);
	newoption->gen.type = RELOPT_TYPE_STRING;
	if (default_val)
	{
		strcpy(newoption->default_val, default_val);
		newoption->default_len = default_len;
		newoption->default_isnull = false;
	}
	else
	{
		newoption->default_val[0] = '\0';
		newoption->default_len = 0;
		newoption->default_isnull = true;
	}

	MemoryContextSwitchTo(oldcxt);

	add_reloption((relopt_gen *) newoption);
}

/*
 * Transform a relation options list (list of DefElem) into the text array
 * format that is kept in pg_class.reloptions.
 *
 * This is used for three cases: CREATE TABLE/INDEX, ALTER TABLE SET, and
 * ALTER TABLE RESET.  In the ALTER cases, oldOptions is the existing
 * reloptions value (possibly NULL), and we replace or remove entries
 * as needed.
 *
 * If ignoreOids is true, then we should ignore any occurrence of "oids"
 * in the list (it will be or has been handled by interpretOidsOption()).
 *
 * Note that this is not responsible for determining whether the options
 * are valid.
 *
 * Both oldOptions and the result are text arrays (or NULL for "default"),
 * but we declare them as Datums to avoid including array.h in reloptions.h.
 */
Datum
transformRelOptions(Datum oldOptions, List *defList,
					bool ignoreOids, bool isReset)
{
	Datum		result;
	ArrayBuildState *astate;
	ListCell   *cell;

	/* no change if empty list */
	if (defList == NIL)
		return oldOptions;

	/* We build new array using accumArrayResult */
	astate = NULL;

	/* Copy any oldOptions that aren't to be replaced */
	if (PointerIsValid(DatumGetPointer(oldOptions)))
	{
		ArrayType  *array = DatumGetArrayTypeP(oldOptions);
		Datum	   *oldoptions;
		int			noldoptions;
		int			i;

		Assert(ARR_ELEMTYPE(array) == TEXTOID);

		deconstruct_array(array, TEXTOID, -1, false, 'i',
						  &oldoptions, NULL, &noldoptions);

		for (i = 0; i < noldoptions; i++)
		{
			text	   *oldoption = DatumGetTextP(oldoptions[i]);
			char	   *text_str = VARDATA(oldoption);
			int			text_len = VARSIZE(oldoption) - VARHDRSZ;

			/* Search for a match in defList */
			foreach(cell, defList)
			{
				DefElem    *def = lfirst(cell);
				int			kw_len = strlen(def->defname);

				if (text_len > kw_len && text_str[kw_len] == '=' &&
					pg_strncasecmp(text_str, def->defname, kw_len) == 0)
					break;
			}
			if (!cell)
			{
				/* No match, so keep old option */
				astate = accumArrayResult(astate, oldoptions[i],
										  false, TEXTOID,
										  CurrentMemoryContext);
			}
		}
	}

	/*
	 * If CREATE/SET, add new options to array; if RESET, just check that the
	 * user didn't say RESET (option=val).  (Must do this because the grammar
	 * doesn't enforce it.)
	 */
	foreach(cell, defList)
	{
		DefElem    *def = lfirst(cell);

		if (isReset)
		{
			if (def->arg != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("RESET must not include values for parameters")));
		}
		else
		{
			text	   *t;
			const char *value;
			Size		len;

			if (ignoreOids && pg_strcasecmp(def->defname, "oids") == 0)
				continue;

			/*
			 * Flatten the DefElem into a text string like "name=arg". If we
			 * have just "name", assume "name=true" is meant.
			 */
			if (def->arg != NULL)
				value = defGetString(def);
			else
				value = "true";
			len = VARHDRSZ + strlen(def->defname) + 1 + strlen(value);
			/* +1 leaves room for sprintf's trailing null */
			t = (text *) palloc(len + 1);
			SET_VARSIZE(t, len);
			sprintf(VARDATA(t), "%s=%s", def->defname, value);

			astate = accumArrayResult(astate, PointerGetDatum(t),
									  false, TEXTOID,
									  CurrentMemoryContext);
		}
	}

	if (astate)
		result = makeArrayResult(astate, CurrentMemoryContext);
	else
		result = (Datum) 0;

	return result;
}


/*
 * Convert the text-array format of reloptions into a List of DefElem.
 * This is the inverse of transformRelOptions().
 */
List *
untransformRelOptions(Datum options)
{
	List	   *result = NIL;
	ArrayType  *array;
	Datum	   *optiondatums;
	int			noptions;
	int			i;

	/* Nothing to do if no options */
	if (!PointerIsValid(DatumGetPointer(options)))
		return result;

	array = DatumGetArrayTypeP(options);

	Assert(ARR_ELEMTYPE(array) == TEXTOID);

	deconstruct_array(array, TEXTOID, -1, false, 'i',
					  &optiondatums, NULL, &noptions);

	for (i = 0; i < noptions; i++)
	{
		char	   *s;
		char	   *p;
		Node	   *val = NULL;

		s = TextDatumGetCString(optiondatums[i]);
		p = strchr(s, '=');
		if (p)
		{
			*p++ = '\0';
			val = (Node *) makeString(pstrdup(p));
		}
		result = lappend(result, makeDefElem(pstrdup(s), val));
	}

	return result;
}


/*
 * Interpret reloptions that are given in text-array format.
 *
 * options is a reloption text array as constructed by transformRelOptions.
 * kind specifies the family of options to be processed.
 *
 * The return value is a relopt_value * array on which the options actually
 * set in the options array are marked with isset=true.  The length of this
 * array is returned in *numrelopts.  Options not set are also present in the
 * array; this is so that the caller can easily locate the default values.
 *
 * If there are no options of the given kind, numrelopts is set to 0 and NULL
 * is returned.
 *
 * Note: values of type int, bool and real are allocated as part of the
 * returned array.  Values of type string are allocated separately and must
 * be freed by the caller.
 */
relopt_value *
parseRelOptions(Datum options, bool validate, relopt_kind kind,
				int *numrelopts)
{
	relopt_value *reloptions;
	int			numoptions = 0;
	int			i;
	int			j;

	if (need_initialization)
		initialize_reloptions();

	/* Build a list of expected options, based on kind */

	for (i = 0; relOpts[i]; i++)
		if (relOpts[i]->kind == kind)
			numoptions++;

	if (numoptions == 0)
	{
		*numrelopts = 0;
		return NULL;
	}

	reloptions = palloc(numoptions * sizeof(relopt_value));

	for (i = 0, j = 0; relOpts[i]; i++)
	{
		if (relOpts[i]->kind == kind)
		{
			reloptions[j].gen = relOpts[i];
			reloptions[j].isset = false;
			j++;
		}
	}

	/* Done if no options */
	if (PointerIsValid(DatumGetPointer(options)))
	{
		ArrayType  *array;
		Datum	   *optiondatums;
		int			noptions;

		array = DatumGetArrayTypeP(options);

		Assert(ARR_ELEMTYPE(array) == TEXTOID);

		deconstruct_array(array, TEXTOID, -1, false, 'i',
						  &optiondatums, NULL, &noptions);

		for (i = 0; i < noptions; i++)
		{
			text	   *optiontext = DatumGetTextP(optiondatums[i]);
			char	   *text_str = VARDATA(optiontext);
			int			text_len = VARSIZE(optiontext) - VARHDRSZ;
			int			j;

			/* Search for a match in reloptions */
			for (j = 0; j < numoptions; j++)
			{
				int			kw_len = reloptions[j].gen->namelen;

				if (text_len > kw_len && text_str[kw_len] == '=' &&
					pg_strncasecmp(text_str, reloptions[j].gen->name,
								   kw_len) == 0)
				{
					parse_one_reloption(&reloptions[j], text_str, text_len,
										validate);
					break;
				}
			}

			if (j >= numoptions && validate)
			{
				char	   *s;
				char	   *p;

				s = TextDatumGetCString(optiondatums[i]);
				p = strchr(s, '=');
				if (p)
					*p = '\0';
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("unrecognized parameter \"%s\"", s)));
			}
		}
	}

	*numrelopts = numoptions;
	return reloptions;
}

/*
 * Subroutine for parseRelOptions, to parse and validate a single option's
 * value
 */
static void
parse_one_reloption(relopt_value *option, char *text_str, int text_len,
					bool validate)
{
	char	   *value;
	int			value_len;
	bool		parsed;
	bool		nofree = false;

	if (option->isset && validate)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("parameter \"%s\" specified more than once",
						option->gen->name)));

	value_len = text_len - option->gen->namelen - 1;
	value = (char *) palloc(value_len + 1);
	memcpy(value, text_str + option->gen->namelen + 1, value_len);
	value[value_len] = '\0';

	switch (option->gen->type)
	{
		case RELOPT_TYPE_BOOL:
			{
				parsed = parse_bool(value, &option->values.bool_val);
				if (validate && !parsed)
					ereport(ERROR,
							(errmsg("invalid value for boolean option \"%s\": %s",
									option->gen->name, value)));
			}
			break;
		case RELOPT_TYPE_INT:
			{
				relopt_int	*optint = (relopt_int *) option->gen;

				parsed = parse_int(value, &option->values.int_val, 0, NULL);
				if (validate && !parsed)
					ereport(ERROR,
							(errmsg("invalid value for integer option \"%s\": %s",
									option->gen->name, value)));
				if (validate && (option->values.int_val < optint->min ||
								 option->values.int_val > optint->max))
					ereport(ERROR,
							(errmsg("value %s out of bounds for option \"%s\"",
									value, option->gen->name),
							 errdetail("Valid values are between \"%d\" and \"%d\".",
									   optint->min, optint->max)));
			}
			break;
		case RELOPT_TYPE_REAL:
			{
				relopt_real	*optreal = (relopt_real *) option->gen;

				parsed = parse_real(value, &option->values.real_val);
				if (validate && !parsed)
					ereport(ERROR,
							(errmsg("invalid value for floating point option \"%s\": %s",
									option->gen->name, value)));
				if (validate && (option->values.real_val < optreal->min ||
								 option->values.real_val > optreal->max))
					ereport(ERROR,
							(errmsg("value %s out of bounds for option \"%s\"",
									value, option->gen->name),
							 errdetail("Valid values are between \"%f\" and \"%f\".",
									   optreal->min, optreal->max)));
			}
			break;
		case RELOPT_TYPE_STRING:
			option->values.string_val = value;
			nofree = true;
			parsed = true;
			/* no validation possible */
			break;
		default:
			elog(ERROR, "unsupported reloption type %d", option->gen->type);
			parsed = true; /* quiet compiler */
			break;
	}

	if (parsed)
		option->isset = true;
	if (!nofree)
		pfree(value);
}

/*
 * Option parser for anything that uses StdRdOptions (i.e. fillfactor only)
 */
bytea *
default_reloptions(Datum reloptions, bool validate, relopt_kind kind)
{
	relopt_value   *options;
	StdRdOptions   *rdopts;
	StdRdOptions	lopts;
	int				numoptions;
	int				len;
	int				i;

	options = parseRelOptions(reloptions, validate, kind, &numoptions);

	/* if none set, we're done */
	if (numoptions == 0)
		return NULL;

	MemSet(&lopts, 0, sizeof(StdRdOptions));

	for (i = 0; i < numoptions; i++)
	{
		HANDLE_INT_RELOPTION("fillfactor", lopts.fillfactor, options[i]);
	}

	pfree(options);

	len = sizeof(StdRdOptions);
	rdopts = palloc(len);
	memcpy(rdopts, &lopts, len);
	SET_VARSIZE(rdopts, len);

	return (bytea *) rdopts;
}

/*
 * Parse options for heaps (and perhaps someday toast tables).
 */
bytea *
heap_reloptions(char relkind, Datum reloptions, bool validate)
{
	return default_reloptions(reloptions, validate, RELOPT_KIND_HEAP);
}


/*
 * Parse options for indexes.
 *
 *	amoptions	Oid of option parser
 *	reloptions	options as text[] datum
 *	validate	error flag
 */
bytea *
index_reloptions(RegProcedure amoptions, Datum reloptions, bool validate)
{
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	Datum		result;

	Assert(RegProcedureIsValid(amoptions));

	/* Assume function is strict */
	if (!PointerIsValid(DatumGetPointer(reloptions)))
		return NULL;

	/* Can't use OidFunctionCallN because we might get a NULL result */
	fmgr_info(amoptions, &flinfo);

	InitFunctionCallInfoData(fcinfo, &flinfo, 2, NULL, NULL);

	fcinfo.arg[0] = reloptions;
	fcinfo.arg[1] = BoolGetDatum(validate);
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;

	result = FunctionCallInvoke(&fcinfo);

	if (fcinfo.isnull || DatumGetPointer(result) == NULL)
		return NULL;

	return DatumGetByteaP(result);
}
