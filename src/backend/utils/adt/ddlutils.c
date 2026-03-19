/*-------------------------------------------------------------------------
 *
 * ddlutils.c
 *		Utility functions for generating DDL statements
 *
 * This file contains the pg_get_*_ddl family of functions that generate
 * DDL statements to recreate database objects such as roles, tablespaces,
 * and databases, along with common infrastructure for option parsing and
 * pretty-printing.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/ddlutils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/varlena.h"

/* Option value types for DDL option parsing */
typedef enum
{
	DDL_OPT_BOOL,
	DDL_OPT_TEXT,
	DDL_OPT_INT,
} DdlOptType;

/*
 * A single DDL option descriptor: caller fills in name and type,
 * parse_ddl_options fills in isset + the appropriate value field.
 */
typedef struct DdlOption
{
	const char *name;			/* option name (case-insensitive match) */
	DdlOptType	type;			/* expected value type */
	bool		isset;			/* true if caller supplied this option */
	/* fields for specific option types */
	union
	{
		bool		boolval;	/* filled in for DDL_OPT_BOOL */
		char	   *textval;	/* filled in for DDL_OPT_TEXT (palloc'd) */
		int			intval;		/* filled in for DDL_OPT_INT */
	};
} DdlOption;


static void parse_ddl_options(FunctionCallInfo fcinfo, int variadic_start,
							  DdlOption *opts, int nopts);
static void append_ddl_option(StringInfo buf, bool pretty, int indent,
							  const char *fmt,...)
			pg_attribute_printf(4, 5);
static void append_guc_value(StringInfo buf, const char *name,
							 const char *value);


/*
 * parse_ddl_options
 * 		Parse variadic name/value option pairs
 *
 * Options are passed as alternating key/value text pairs.  The caller
 * provides an array of DdlOption descriptors specifying the accepted
 * option names and their types; this function matches each supplied
 * pair against the array, validates the value, and fills in the
 * result fields.
 */
static void
parse_ddl_options(FunctionCallInfo fcinfo, int variadic_start,
				  DdlOption *opts, int nopts)
{
	Datum	   *args;
	bool	   *nulls;
	Oid		   *types;
	int			nargs;

	/* Clear all output fields */
	for (int i = 0; i < nopts; i++)
	{
		opts[i].isset = false;
		switch (opts[i].type)
		{
			case DDL_OPT_BOOL:
				opts[i].boolval = false;
				break;
			case DDL_OPT_TEXT:
				opts[i].textval = NULL;
				break;
			case DDL_OPT_INT:
				opts[i].intval = 0;
				break;
		}
	}

	nargs = extract_variadic_args(fcinfo, variadic_start, true,
								  &args, &types, &nulls);

	if (nargs <= 0)
		return;

	/* Handle DEFAULT NULL case */
	if (nargs == 1 && nulls[0])
		return;

	if (nargs % 2 != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("variadic arguments must be name/value pairs"),
				 errhint("Provide an even number of variadic arguments that can be divided into pairs.")));

	/*
	 * For each option name/value pair, find corresponding positional option
	 * for the option name, and assign the option value.
	 */
	for (int i = 0; i < nargs; i += 2)
	{
		char	   *name;
		char	   *valstr;
		DdlOption  *opt = NULL;

		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("option name at variadic position %d is null", i + 1)));

		name = TextDatumGetCString(args[i]);

		if (nulls[i + 1])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("value for option \"%s\" must not be null", name)));

		/* Find matching option descriptor */
		for (int j = 0; j < nopts; j++)
		{
			if (pg_strcasecmp(name, opts[j].name) == 0)
			{
				opt = &opts[j];
				break;
			}
		}

		if (opt == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized option: \"%s\"", name)));

		if (opt->isset)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("option \"%s\" is specified more than once",
							name)));

		valstr = TextDatumGetCString(args[i + 1]);

		switch (opt->type)
		{
			case DDL_OPT_BOOL:
				if (!parse_bool(valstr, &opt->boolval))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for boolean option \"%s\": %s",
									name, valstr)));
				break;

			case DDL_OPT_TEXT:
				opt->textval = valstr;
				valstr = NULL;	/* don't pfree below */
				break;

			case DDL_OPT_INT:
				{
					char	   *endp;
					long		val;

					errno = 0;
					val = strtol(valstr, &endp, 10);
					if (*endp != '\0' || errno == ERANGE ||
						val < PG_INT32_MIN || val > PG_INT32_MAX)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("invalid value for integer option \"%s\": %s",
										name, valstr)));
					opt->intval = (int) val;
				}
				break;
		}

		opt->isset = true;

		if (valstr)
			pfree(valstr);
		pfree(name);
	}
}

/*
 * Helper to append a formatted string with optional pretty-printing.
 */
static void
append_ddl_option(StringInfo buf, bool pretty, int indent,
				  const char *fmt,...)
{
	if (pretty)
	{
		appendStringInfoChar(buf, '\n');
		appendStringInfoSpaces(buf, indent);
	}
	else
		appendStringInfoChar(buf, ' ');

	for (;;)
	{
		va_list		args;
		int			needed;

		va_start(args, fmt);
		needed = appendStringInfoVA(buf, fmt, args);
		va_end(args);
		if (needed == 0)
			break;
		enlargeStringInfo(buf, needed);
	}
}

/*
 * append_guc_value
 *		Append a GUC setting value to buf, handling GUC_LIST_QUOTE properly.
 *
 * Variables marked GUC_LIST_QUOTE were already fully quoted before they
 * were stored in the setconfig array.  We break the list value apart
 * and re-quote the elements as string literals.  For all other variables
 * we simply quote the value as a single string literal.
 *
 * The caller has already appended "SET <name> TO " to buf.
 */
static void
append_guc_value(StringInfo buf, const char *name, const char *value)
{
	char	   *rawval;

	rawval = pstrdup(value);

	if (GetConfigOptionFlags(name, true) & GUC_LIST_QUOTE)
	{
		List	   *namelist;
		bool		first = true;

		/* Parse string into list of identifiers */
		if (!SplitGUCList(rawval, ',', &namelist))
		{
			/* this shouldn't fail really */
			elog(ERROR, "invalid list syntax in setconfig item");
		}
		/* Special case: represent an empty list as NULL */
		if (namelist == NIL)
			appendStringInfoString(buf, "NULL");
		foreach_ptr(char, curname, namelist)
		{
			if (first)
				first = false;
			else
				appendStringInfoString(buf, ", ");
			appendStringInfoString(buf, quote_literal_cstr(curname));
		}
		list_free(namelist);
	}
	else
		appendStringInfoString(buf, quote_literal_cstr(rawval));

	pfree(rawval);
}
