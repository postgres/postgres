/*-------------------------------------------------------------------------
 *
 * option.c
 *		  FDW and GUC option handling for postgres_fdw
 *
 * Portions Copyright (c) 2012-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  contrib/postgres_fdw/option.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/reloptions.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "libpq/libpq-be.h"
#include "postgres_fdw.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/varlena.h"

/*
 * Describes the valid options for objects that this wrapper uses.
 */
typedef struct PgFdwOption
{
	const char *keyword;
	Oid			optcontext;		/* OID of catalog in which option may appear */
	bool		is_libpq_opt;	/* true if it's used in libpq */
} PgFdwOption;

/*
 * Valid options for postgres_fdw.
 * Allocated and filled in InitPgFdwOptions.
 */
static PgFdwOption *postgres_fdw_options;

/*
 * Valid options for libpq.
 * Allocated and filled in InitPgFdwOptions.
 */
static PQconninfoOption *libpq_options;

/*
 * GUC parameters
 */
char	   *pgfdw_application_name = NULL;

/*
 * Helper functions
 */
static void InitPgFdwOptions(void);
static bool is_valid_option(const char *keyword, Oid context);
static bool is_libpq_option(const char *keyword);

#include "miscadmin.h"

/*
 * Validate the generic options given to a FOREIGN DATA WRAPPER, SERVER,
 * USER MAPPING or FOREIGN TABLE that uses postgres_fdw.
 *
 * Raise an ERROR if the option or its value is considered invalid.
 */
PG_FUNCTION_INFO_V1(postgres_fdw_validator);

Datum
postgres_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);
	ListCell   *cell;

	/* Build our options lists if we didn't yet. */
	InitPgFdwOptions();

	/*
	 * Check that only options supported by postgres_fdw, and allowed for the
	 * current object type, are given.
	 */
	foreach(cell, options_list)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (!is_valid_option(def->defname, catalog))
		{
			/*
			 * Unknown option specified, complain about it. Provide a hint
			 * with a valid option that looks similar, if there is one.
			 */
			PgFdwOption *opt;
			const char *closest_match;
			ClosestMatchState match_state;
			bool		has_valid_options = false;

			initClosestMatch(&match_state, def->defname, 4);
			for (opt = postgres_fdw_options; opt->keyword; opt++)
			{
				if (catalog == opt->optcontext)
				{
					has_valid_options = true;
					updateClosestMatch(&match_state, opt->keyword);
				}
			}

			closest_match = getClosestMatch(&match_state);
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", def->defname),
					 has_valid_options ? closest_match ?
					 errhint("Perhaps you meant the option \"%s\".",
							 closest_match) : 0 :
					 errhint("There are no valid options in this context.")));
		}

		/*
		 * Validate option value, when we can do so without any context.
		 */
		if (strcmp(def->defname, "use_remote_estimate") == 0 ||
			strcmp(def->defname, "updatable") == 0 ||
			strcmp(def->defname, "truncatable") == 0 ||
			strcmp(def->defname, "async_capable") == 0 ||
			strcmp(def->defname, "parallel_commit") == 0 ||
			strcmp(def->defname, "keep_connections") == 0)
		{
			/* these accept only boolean values */
			(void) defGetBoolean(def);
		}
		else if (strcmp(def->defname, "fdw_startup_cost") == 0 ||
				 strcmp(def->defname, "fdw_tuple_cost") == 0)
		{
			/*
			 * These must have a floating point value greater than or equal to
			 * zero.
			 */
			char	   *value;
			double		real_val;
			bool		is_parsed;

			value = defGetString(def);
			is_parsed = parse_real(value, &real_val, 0, NULL);

			if (!is_parsed)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid value for floating point option \"%s\": %s",
								def->defname, value)));

			if (real_val < 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("\"%s\" must be a floating point value greater than or equal to zero",
								def->defname)));
		}
		else if (strcmp(def->defname, "extensions") == 0)
		{
			/* check list syntax, warn about uninstalled extensions */
			(void) ExtractExtensionList(defGetString(def), true);
		}
		else if (strcmp(def->defname, "fetch_size") == 0 ||
				 strcmp(def->defname, "batch_size") == 0)
		{
			char	   *value;
			int			int_val;
			bool		is_parsed;

			value = defGetString(def);
			is_parsed = parse_int(value, &int_val, 0, NULL);

			if (!is_parsed)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid value for integer option \"%s\": %s",
								def->defname, value)));

			if (int_val <= 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("\"%s\" must be an integer value greater than zero",
								def->defname)));
		}
		else if (strcmp(def->defname, "password_required") == 0)
		{
			bool		pw_required = defGetBoolean(def);

			/*
			 * Only the superuser may set this option on a user mapping, or
			 * alter a user mapping on which this option is set. We allow a
			 * user to clear this option if it's set - in fact, we don't have
			 * a choice since we can't see the old mapping when validating an
			 * alter.
			 */
			if (!superuser() && !pw_required)
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("password_required=false is superuser-only"),
						 errhint("User mappings with the password_required option set to false may only be created or modified by the superuser.")));
		}
		else if (strcmp(def->defname, "sslcert") == 0 ||
				 strcmp(def->defname, "sslkey") == 0)
		{
			/* similarly for sslcert / sslkey on user mapping */
			if (catalog == UserMappingRelationId && !superuser())
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("sslcert and sslkey are superuser-only"),
						 errhint("User mappings with the sslcert or sslkey options set may only be created or modified by the superuser.")));
		}
		else if (strcmp(def->defname, "analyze_sampling") == 0)
		{
			char	   *value;

			value = defGetString(def);

			/* we recognize off/auto/random/system/bernoulli */
			if (strcmp(value, "off") != 0 &&
				strcmp(value, "auto") != 0 &&
				strcmp(value, "random") != 0 &&
				strcmp(value, "system") != 0 &&
				strcmp(value, "bernoulli") != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid value for string option \"%s\": %s",
								def->defname, value)));
		}
	}

	PG_RETURN_VOID();
}

/*
 * Initialize option lists.
 */
static void
InitPgFdwOptions(void)
{
	int			num_libpq_opts;
	PQconninfoOption *lopt;
	PgFdwOption *popt;

	/* non-libpq FDW-specific FDW options */
	static const PgFdwOption non_libpq_options[] = {
		{"schema_name", ForeignTableRelationId, false},
		{"table_name", ForeignTableRelationId, false},
		{"column_name", AttributeRelationId, false},
		/* use_remote_estimate is available on both server and table */
		{"use_remote_estimate", ForeignServerRelationId, false},
		{"use_remote_estimate", ForeignTableRelationId, false},
		/* cost factors */
		{"fdw_startup_cost", ForeignServerRelationId, false},
		{"fdw_tuple_cost", ForeignServerRelationId, false},
		/* shippable extensions */
		{"extensions", ForeignServerRelationId, false},
		/* updatable is available on both server and table */
		{"updatable", ForeignServerRelationId, false},
		{"updatable", ForeignTableRelationId, false},
		/* truncatable is available on both server and table */
		{"truncatable", ForeignServerRelationId, false},
		{"truncatable", ForeignTableRelationId, false},
		/* fetch_size is available on both server and table */
		{"fetch_size", ForeignServerRelationId, false},
		{"fetch_size", ForeignTableRelationId, false},
		/* batch_size is available on both server and table */
		{"batch_size", ForeignServerRelationId, false},
		{"batch_size", ForeignTableRelationId, false},
		/* async_capable is available on both server and table */
		{"async_capable", ForeignServerRelationId, false},
		{"async_capable", ForeignTableRelationId, false},
		{"parallel_commit", ForeignServerRelationId, false},
		{"keep_connections", ForeignServerRelationId, false},
		{"password_required", UserMappingRelationId, false},

		/* sampling is available on both server and table */
		{"analyze_sampling", ForeignServerRelationId, false},
		{"analyze_sampling", ForeignTableRelationId, false},

		/*
		 * sslcert and sslkey are in fact libpq options, but we repeat them
		 * here to allow them to appear in both foreign server context (when
		 * we generate libpq options) and user mapping context (from here).
		 */
		{"sslcert", UserMappingRelationId, true},
		{"sslkey", UserMappingRelationId, true},

		{NULL, InvalidOid, false}
	};

	/* Prevent redundant initialization. */
	if (postgres_fdw_options)
		return;

	/*
	 * Get list of valid libpq options.
	 *
	 * To avoid unnecessary work, we get the list once and use it throughout
	 * the lifetime of this backend process.  We don't need to care about
	 * memory context issues, because PQconndefaults allocates with malloc.
	 */
	libpq_options = PQconndefaults();
	if (!libpq_options)			/* assume reason for failure is OOM */
		ereport(ERROR,
				(errcode(ERRCODE_FDW_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Could not get libpq's default connection options.")));

	/* Count how many libpq options are available. */
	num_libpq_opts = 0;
	for (lopt = libpq_options; lopt->keyword; lopt++)
		num_libpq_opts++;

	/*
	 * Construct an array which consists of all valid options for
	 * postgres_fdw, by appending FDW-specific options to libpq options.
	 *
	 * We use plain malloc here to allocate postgres_fdw_options because it
	 * lives as long as the backend process does.  Besides, keeping
	 * libpq_options in memory allows us to avoid copying every keyword
	 * string.
	 */
	postgres_fdw_options = (PgFdwOption *)
		malloc(sizeof(PgFdwOption) * num_libpq_opts +
			   sizeof(non_libpq_options));
	if (postgres_fdw_options == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	popt = postgres_fdw_options;
	for (lopt = libpq_options; lopt->keyword; lopt++)
	{
		/* Hide debug options, as well as settings we override internally. */
		if (strchr(lopt->dispchar, 'D') ||
			strcmp(lopt->keyword, "fallback_application_name") == 0 ||
			strcmp(lopt->keyword, "client_encoding") == 0)
			continue;

		/* We don't have to copy keyword string, as described above. */
		popt->keyword = lopt->keyword;

		/*
		 * "user" and any secret options are allowed only on user mappings.
		 * Everything else is a server option.
		 */
		if (strcmp(lopt->keyword, "user") == 0 || strchr(lopt->dispchar, '*'))
			popt->optcontext = UserMappingRelationId;
		else
			popt->optcontext = ForeignServerRelationId;
		popt->is_libpq_opt = true;

		popt++;
	}

	/* Append FDW-specific options and dummy terminator. */
	memcpy(popt, non_libpq_options, sizeof(non_libpq_options));
}

/*
 * Check whether the given option is one of the valid postgres_fdw options.
 * context is the Oid of the catalog holding the object the option is for.
 */
static bool
is_valid_option(const char *keyword, Oid context)
{
	PgFdwOption *opt;

	Assert(postgres_fdw_options);	/* must be initialized already */

	for (opt = postgres_fdw_options; opt->keyword; opt++)
	{
		if (context == opt->optcontext && strcmp(opt->keyword, keyword) == 0)
			return true;
	}

	return false;
}

/*
 * Check whether the given option is one of the valid libpq options.
 */
static bool
is_libpq_option(const char *keyword)
{
	PgFdwOption *opt;

	Assert(postgres_fdw_options);	/* must be initialized already */

	for (opt = postgres_fdw_options; opt->keyword; opt++)
	{
		if (opt->is_libpq_opt && strcmp(opt->keyword, keyword) == 0)
			return true;
	}

	return false;
}

/*
 * Generate key-value arrays which include only libpq options from the
 * given list (which can contain any kind of options).  Caller must have
 * allocated large-enough arrays.  Returns number of options found.
 */
int
ExtractConnectionOptions(List *defelems, const char **keywords,
						 const char **values)
{
	ListCell   *lc;
	int			i;

	/* Build our options lists if we didn't yet. */
	InitPgFdwOptions();

	i = 0;
	foreach(lc, defelems)
	{
		DefElem    *d = (DefElem *) lfirst(lc);

		if (is_libpq_option(d->defname))
		{
			keywords[i] = d->defname;
			values[i] = defGetString(d);
			i++;
		}
	}
	return i;
}

/*
 * Parse a comma-separated string and return a List of the OIDs of the
 * extensions named in the string.  If any names in the list cannot be
 * found, report a warning if warnOnMissing is true, else just silently
 * ignore them.
 */
List *
ExtractExtensionList(const char *extensionsString, bool warnOnMissing)
{
	List	   *extensionOids = NIL;
	List	   *extlist;
	ListCell   *lc;

	/* SplitIdentifierString scribbles on its input, so pstrdup first */
	if (!SplitIdentifierString(pstrdup(extensionsString), ',', &extlist))
	{
		/* syntax error in name list */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("parameter \"%s\" must be a list of extension names",
						"extensions")));
	}

	foreach(lc, extlist)
	{
		const char *extension_name = (const char *) lfirst(lc);
		Oid			extension_oid = get_extension_oid(extension_name, true);

		if (OidIsValid(extension_oid))
		{
			extensionOids = lappend_oid(extensionOids, extension_oid);
		}
		else if (warnOnMissing)
		{
			ereport(WARNING,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("extension \"%s\" is not installed",
							extension_name)));
		}
	}

	list_free(extlist);
	return extensionOids;
}

/*
 * Replace escape sequences beginning with % character in the given
 * application_name with status information, and return it.
 *
 * This function always returns a palloc'd string, so the caller is
 * responsible for pfreeing it.
 */
char *
process_pgfdw_appname(const char *appname)
{
	const char *p;
	StringInfoData buf;

	Assert(MyProcPort != NULL);

	initStringInfo(&buf);

	for (p = appname; *p != '\0'; p++)
	{
		if (*p != '%')
		{
			/* literal char, just copy */
			appendStringInfoChar(&buf, *p);
			continue;
		}

		/* must be a '%', so skip to the next char */
		p++;
		if (*p == '\0')
			break;				/* format error - ignore it */
		else if (*p == '%')
		{
			/* string contains %% */
			appendStringInfoChar(&buf, '%');
			continue;
		}

		/* process the option */
		switch (*p)
		{
			case 'a':
				appendStringInfoString(&buf, application_name);
				break;
			case 'c':
				appendStringInfo(&buf, "%lx.%x", (long) (MyStartTime), MyProcPid);
				break;
			case 'C':
				appendStringInfoString(&buf, cluster_name);
				break;
			case 'd':
				appendStringInfoString(&buf, MyProcPort->database_name);
				break;
			case 'p':
				appendStringInfo(&buf, "%d", MyProcPid);
				break;
			case 'u':
				appendStringInfoString(&buf, MyProcPort->user_name);
				break;
			default:
				/* format error - ignore it */
				break;
		}
	}

	return buf.data;
}

/*
 * Module load callback
 */
void
_PG_init(void)
{
	/*
	 * Unlike application_name GUC, don't set GUC_IS_NAME flag nor check_hook
	 * to allow postgres_fdw.application_name to be any string more than
	 * NAMEDATALEN characters and to include non-ASCII characters. Instead,
	 * remote server truncates application_name of remote connection to less
	 * than NAMEDATALEN and replaces any non-ASCII characters in it with a '?'
	 * character.
	 */
	DefineCustomStringVariable("postgres_fdw.application_name",
							   "Sets the application name to be used on the remote server.",
							   NULL,
							   &pgfdw_application_name,
							   NULL,
							   PGC_USERSET,
							   0,
							   NULL,
							   NULL,
							   NULL);

	MarkGUCPrefixReserved("postgres_fdw");
}
