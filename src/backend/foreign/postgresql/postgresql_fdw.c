/*-------------------------------------------------------------------------
 *
 * postgresql_fdw.c
 *        foreign-data wrapper for postgresql (libpq) connections.
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *        $PostgreSQL: pgsql/src/backend/foreign/postgresql/postgresql_fdw.c,v 1.2 2009/01/01 17:23:42 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "nodes/value.h"
#include "nodes/parsenodes.h"
#include "nodes/makefuncs.h"
#include "foreign/foreign.h"

PG_MODULE_MAGIC;


/*
 * Describes the valid options for postgresql FDW, server and user mapping.
 */
typedef struct ConnectionOptions {
	const char		   *optname;		/* Option name */
	GenericOptionFlags	optflags;		/* Option usage bitmap */
} ConnectionOptions;

/*
 * Copied from fe-connect.c PQconninfoOptions.
 *
 * The list is small - don't bother with bsearch if it stays so.
 */
static ConnectionOptions libpq_conninfo_options[] = {
	{ "authtype",			ServerOpt		},
	{ "service",			ServerOpt		},
	{ "user",				UserMappingOpt	},
	{ "password",			UserMappingOpt	},
	{ "connect_timeout", 	ServerOpt		},
	{ "dbname",				ServerOpt		},
	{ "host",				ServerOpt		},
	{ "hostaddr",			ServerOpt		},
	{ "port",				ServerOpt		},
	{ "tty",				ServerOpt		},
	{ "options",			ServerOpt		},
	{ "requiressl",			ServerOpt		},
	{ "sslmode",			ServerOpt		},
	{ "gsslib",				ServerOpt		},
	{ NULL,					InvalidOpt		}
};

void _PG_fini(void);


/*
 * Check if the provided option is one of libpq conninfo options.
 * We look at only options with matching flags.
 */
static bool
is_conninfo_option(const char *option, GenericOptionFlags flags)
{
	ConnectionOptions *opt;

	for (opt = libpq_conninfo_options; opt->optname != NULL; opt++)
		if (flags & opt->optflags && strcmp(opt->optname, option) == 0)
			return true;
	return false;
}

/*
 * Validate the generic option given to SERVER or USER MAPPING.
 * Raise an ERROR if the option or its value is considered
 * invalid.
 *
 * Valid server options are all libpq conninfo options except
 * user and password -- these may only appear in USER MAPPING options.
 */
void
_pg_validateOptionList(ForeignDataWrapper *fdw, GenericOptionFlags flags,
				   List *options)
{
	ListCell *cell;

	foreach (cell, options)
	{
		DefElem    *def = lfirst(cell);

		if (!is_conninfo_option(def->defname, flags))
		{
			ConnectionOptions  *opt;
			StringInfoData		buf;
			const char		   *objtype;

			/*
			 * Unknown option specified, complain about it. Provide a hint
			 * with list of valid options for the object.
			 */
			initStringInfo(&buf);
			for (opt = libpq_conninfo_options; opt->optname != NULL; opt++)
				if (flags & opt->optflags)
					appendStringInfo(&buf, "%s%s", (buf.len > 0) ? ", " : "",
									 opt->optname);

			if (flags & ServerOpt)
				objtype = "server";
			else if (flags & UserMappingOpt)
				objtype = "user mapping";
			else if (flags & FdwOpt)
				objtype = "foreign-data wrapper";
			else
				objtype = "???";

			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid option \"%s\" to %s", def->defname, objtype),
					 errhint("valid %s options are: %s", objtype, buf.data)));
		}
	}
}
