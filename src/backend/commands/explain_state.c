/*-------------------------------------------------------------------------
 *
 * explain_state.c
 *	  Code for initializing and accessing ExplainState objects
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * In-core options have hard-coded fields inside ExplainState; e.g. if
 * the user writes EXPLAIN (BUFFERS) then ExplainState's "buffers" member
 * will be set to true. Extensions can also register options using
 * RegisterExtensionExplainOption; so that e.g. EXPLAIN (BICYCLE 'red')
 * will invoke a designated handler that knows what the legal values are
 * for the BICYCLE option. However, it's not enough for an extension to be
 * able to parse new options: it also needs a place to store the results
 * of that parsing, and an ExplainState has no 'bicycle' field.
 *
 * To solve this problem, an ExplainState can contain an array of opaque
 * pointers, one per extension. An extension can use GetExplainExtensionId
 * to acquire an integer ID to acquire an offset into this array that is
 * reserved for its exclusive use, and then use GetExplainExtensionState
 * and SetExplainExtensionState to read and write its own private state
 * within an ExplainState.
 *
 * Note that there is no requirement that the name of the option match
 * the name of the extension; e.g. a pg_explain_conveyance extension could
 * implement options for BICYCLE, MONORAIL, etc.
 *
 * IDENTIFICATION
 *	  src/backend/commands/explain_state.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/explain_state.h"

/* Hook to perform additional EXPLAIN options validation */
explain_validate_options_hook_type explain_validate_options_hook = NULL;

typedef struct
{
	const char *option_name;
	ExplainOptionHandler option_handler;
} ExplainExtensionOption;

static const char **ExplainExtensionNameArray = NULL;
static int	ExplainExtensionNamesAssigned = 0;
static int	ExplainExtensionNamesAllocated = 0;

static ExplainExtensionOption *ExplainExtensionOptionArray = NULL;
static int	ExplainExtensionOptionsAssigned = 0;
static int	ExplainExtensionOptionsAllocated = 0;

/*
 * Create a new ExplainState struct initialized with default options.
 */
ExplainState *
NewExplainState(void)
{
	ExplainState *es = (ExplainState *) palloc0(sizeof(ExplainState));

	/* Set default options (most fields can be left as zeroes). */
	es->costs = true;
	/* Prepare output buffer. */
	es->str = makeStringInfo();

	return es;
}

/*
 * Parse a list of EXPLAIN options and update an ExplainState accordingly.
 */
void
ParseExplainOptionList(ExplainState *es, List *options, ParseState *pstate)
{
	ListCell   *lc;
	bool		timing_set = false;
	bool		buffers_set = false;
	bool		summary_set = false;

	/* Parse options list. */
	foreach(lc, options)
	{
		DefElem    *opt = (DefElem *) lfirst(lc);

		if (strcmp(opt->defname, "analyze") == 0)
			es->analyze = defGetBoolean(opt);
		else if (strcmp(opt->defname, "verbose") == 0)
			es->verbose = defGetBoolean(opt);
		else if (strcmp(opt->defname, "costs") == 0)
			es->costs = defGetBoolean(opt);
		else if (strcmp(opt->defname, "buffers") == 0)
		{
			buffers_set = true;
			es->buffers = defGetBoolean(opt);
		}
		else if (strcmp(opt->defname, "wal") == 0)
			es->wal = defGetBoolean(opt);
		else if (strcmp(opt->defname, "settings") == 0)
			es->settings = defGetBoolean(opt);
		else if (strcmp(opt->defname, "generic_plan") == 0)
			es->generic = defGetBoolean(opt);
		else if (strcmp(opt->defname, "timing") == 0)
		{
			timing_set = true;
			es->timing = defGetBoolean(opt);
		}
		else if (strcmp(opt->defname, "summary") == 0)
		{
			summary_set = true;
			es->summary = defGetBoolean(opt);
		}
		else if (strcmp(opt->defname, "memory") == 0)
			es->memory = defGetBoolean(opt);
		else if (strcmp(opt->defname, "serialize") == 0)
		{
			if (opt->arg)
			{
				char	   *p = defGetString(opt);

				if (strcmp(p, "off") == 0 || strcmp(p, "none") == 0)
					es->serialize = EXPLAIN_SERIALIZE_NONE;
				else if (strcmp(p, "text") == 0)
					es->serialize = EXPLAIN_SERIALIZE_TEXT;
				else if (strcmp(p, "binary") == 0)
					es->serialize = EXPLAIN_SERIALIZE_BINARY;
				else
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("unrecognized value for EXPLAIN option \"%s\": \"%s\"",
									opt->defname, p),
							 parser_errposition(pstate, opt->location)));
			}
			else
			{
				/* SERIALIZE without an argument is taken as 'text' */
				es->serialize = EXPLAIN_SERIALIZE_TEXT;
			}
		}
		else if (strcmp(opt->defname, "format") == 0)
		{
			char	   *p = defGetString(opt);

			if (strcmp(p, "text") == 0)
				es->format = EXPLAIN_FORMAT_TEXT;
			else if (strcmp(p, "xml") == 0)
				es->format = EXPLAIN_FORMAT_XML;
			else if (strcmp(p, "json") == 0)
				es->format = EXPLAIN_FORMAT_JSON;
			else if (strcmp(p, "yaml") == 0)
				es->format = EXPLAIN_FORMAT_YAML;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("unrecognized value for EXPLAIN option \"%s\": \"%s\"",
								opt->defname, p),
						 parser_errposition(pstate, opt->location)));
		}
		else if (!ApplyExtensionExplainOption(es, opt, pstate))
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized EXPLAIN option \"%s\"",
							opt->defname),
					 parser_errposition(pstate, opt->location)));
	}

	/* check that WAL is used with EXPLAIN ANALYZE */
	if (es->wal && !es->analyze)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("EXPLAIN option %s requires ANALYZE", "WAL")));

	/* if the timing was not set explicitly, set default value */
	es->timing = (timing_set) ? es->timing : es->analyze;

	/* if the buffers was not set explicitly, set default value */
	es->buffers = (buffers_set) ? es->buffers : es->analyze;

	/* check that timing is used with EXPLAIN ANALYZE */
	if (es->timing && !es->analyze)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("EXPLAIN option %s requires ANALYZE", "TIMING")));

	/* check that serialize is used with EXPLAIN ANALYZE */
	if (es->serialize != EXPLAIN_SERIALIZE_NONE && !es->analyze)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("EXPLAIN option %s requires ANALYZE", "SERIALIZE")));

	/* check that GENERIC_PLAN is not used with EXPLAIN ANALYZE */
	if (es->generic && es->analyze)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("EXPLAIN options ANALYZE and GENERIC_PLAN cannot be used together")));

	/* if the summary was not set explicitly, set default value */
	es->summary = (summary_set) ? es->summary : es->analyze;

	/* plugin specific option validation */
	if (explain_validate_options_hook)
		(*explain_validate_options_hook) (es, options, pstate);
}

/*
 * Map the name of an EXPLAIN extension to an integer ID.
 *
 * Within the lifetime of a particular backend, the same name will be mapped
 * to the same ID every time. IDs are not stable across backends. Use the ID
 * that you get from this function to call GetExplainExtensionState and
 * SetExplainExtensionState.
 *
 * extension_name is assumed to be a constant string or allocated in storage
 * that will never be freed.
 */
int
GetExplainExtensionId(const char *extension_name)
{
	/* Search for an existing extension by this name; if found, return ID. */
	for (int i = 0; i < ExplainExtensionNamesAssigned; ++i)
		if (strcmp(ExplainExtensionNameArray[i], extension_name) == 0)
			return i;

	/* If there is no array yet, create one. */
	if (ExplainExtensionNameArray == NULL)
	{
		ExplainExtensionNamesAllocated = 16;
		ExplainExtensionNameArray = (const char **)
			MemoryContextAlloc(TopMemoryContext,
							   ExplainExtensionNamesAllocated
							   * sizeof(char *));
	}

	/* If there's an array but it's currently full, expand it. */
	if (ExplainExtensionNamesAssigned >= ExplainExtensionNamesAllocated)
	{
		int			i = pg_nextpower2_32(ExplainExtensionNamesAssigned + 1);

		ExplainExtensionNameArray = (const char **)
			repalloc(ExplainExtensionNameArray, i * sizeof(char *));
		ExplainExtensionNamesAllocated = i;
	}

	/* Assign and return new ID. */
	ExplainExtensionNameArray[ExplainExtensionNamesAssigned] = extension_name;
	return ExplainExtensionNamesAssigned++;
}

/*
 * Get extension-specific state from an ExplainState.
 *
 * See comments for SetExplainExtensionState, below.
 */
void *
GetExplainExtensionState(ExplainState *es, int extension_id)
{
	Assert(extension_id >= 0);

	if (extension_id >= es->extension_state_allocated)
		return NULL;

	return es->extension_state[extension_id];
}

/*
 * Store extension-specific state into an ExplainState.
 *
 * To use this function, first obtain an integer extension_id using
 * GetExplainExtensionId. Then use this function to store an opaque pointer
 * in the ExplainState. Later, you can retrieve the opaque pointer using
 * GetExplainExtensionState.
 */
void
SetExplainExtensionState(ExplainState *es, int extension_id, void *opaque)
{
	Assert(extension_id >= 0);

	/* If there is no array yet, create one. */
	if (es->extension_state == NULL)
	{
		es->extension_state_allocated = 16;
		es->extension_state =
			palloc0(es->extension_state_allocated * sizeof(void *));
	}

	/* If there's an array but it's currently full, expand it. */
	if (extension_id >= es->extension_state_allocated)
	{
		int			i;

		i = pg_nextpower2_32(es->extension_state_allocated + 1);
		es->extension_state = (void **)
			repalloc0(es->extension_state,
					  es->extension_state_allocated * sizeof(void *),
					  i * sizeof(void *));
		es->extension_state_allocated = i;
	}

	es->extension_state[extension_id] = opaque;
}

/*
 * Register a new EXPLAIN option.
 *
 * When option_name is used as an EXPLAIN option, handler will be called and
 * should update the ExplainState passed to it. See comments at top of file
 * for a more detailed explanation.
 *
 * option_name is assumed to be a constant string or allocated in storage
 * that will never be freed.
 */
void
RegisterExtensionExplainOption(const char *option_name,
							   ExplainOptionHandler handler)
{
	ExplainExtensionOption *exopt;

	/* Search for an existing option by this name; if found, update handler. */
	for (int i = 0; i < ExplainExtensionOptionsAssigned; ++i)
	{
		if (strcmp(ExplainExtensionOptionArray[i].option_name,
				   option_name) == 0)
		{
			ExplainExtensionOptionArray[i].option_handler = handler;
			return;
		}
	}

	/* If there is no array yet, create one. */
	if (ExplainExtensionOptionArray == NULL)
	{
		ExplainExtensionOptionsAllocated = 16;
		ExplainExtensionOptionArray = (ExplainExtensionOption *)
			MemoryContextAlloc(TopMemoryContext,
							   ExplainExtensionOptionsAllocated
							   * sizeof(char *));
	}

	/* If there's an array but it's currently full, expand it. */
	if (ExplainExtensionOptionsAssigned >= ExplainExtensionOptionsAllocated)
	{
		int			i = pg_nextpower2_32(ExplainExtensionOptionsAssigned + 1);

		ExplainExtensionOptionArray = (ExplainExtensionOption *)
			repalloc(ExplainExtensionOptionArray, i * sizeof(char *));
		ExplainExtensionOptionsAllocated = i;
	}

	/* Assign and return new ID. */
	exopt = &ExplainExtensionOptionArray[ExplainExtensionOptionsAssigned++];
	exopt->option_name = option_name;
	exopt->option_handler = handler;
}

/*
 * Apply an EXPLAIN option registered by an extension.
 *
 * If no extension has registered the named option, returns false. Otherwise,
 * calls the appropriate handler function and then returns true.
 */
bool
ApplyExtensionExplainOption(ExplainState *es, DefElem *opt, ParseState *pstate)
{
	for (int i = 0; i < ExplainExtensionOptionsAssigned; ++i)
	{
		if (strcmp(ExplainExtensionOptionArray[i].option_name,
				   opt->defname) == 0)
		{
			ExplainExtensionOptionArray[i].option_handler(es, opt, pstate);
			return true;
		}
	}

	return false;
}
