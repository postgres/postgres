/*
 * oid2name, a PostgreSQL app to map OIDs on the filesystem
 * to table and database names.
 *
 * Originally by
 * B. Palmer, bpalmer@crimelabs.net 1-17-2001
 *
 * contrib/oid2name/oid2name.c
 */
#include "postgres_fe.h"

#include "catalog/pg_class_d.h"
#include "common/connect.h"
#include "common/logging.h"
#include "common/string.h"
#include "getopt_long.h"
#include "libpq-fe.h"
#include "pg_getopt.h"

/* an extensible array to keep track of elements to show */
typedef struct
{
	char	  **array;
	int			num;
	int			alloc;
} eary;

/* these are the opts structures for command line params */
struct options
{
	eary	   *tables;
	eary	   *oids;
	eary	   *filenodes;

	bool		quiet;
	bool		systables;
	bool		indexes;
	bool		nodb;
	bool		extended;
	bool		tablespaces;

	char	   *dbname;
	char	   *hostname;
	char	   *port;
	char	   *username;
	const char *progname;
};

/* function prototypes */
static void help(const char *progname);
void		get_opts(int, char **, struct options *);
void		add_one_elt(char *eltname, eary *eary);
char	   *get_comma_elts(eary *eary);
PGconn	   *sql_conn(struct options *);
int			sql_exec(PGconn *, const char *sql, bool quiet);
void		sql_exec_dumpalldbs(PGconn *, struct options *);
void		sql_exec_dumpalltables(PGconn *, struct options *);
void		sql_exec_searchtables(PGconn *, struct options *);
void		sql_exec_dumpalltbspc(PGconn *, struct options *);

/* function to parse command line options and check for some usage errors. */
void
get_opts(int argc, char **argv, struct options *my_opts)
{
	static struct option long_options[] = {
		{"dbname", required_argument, NULL, 'd'},
		{"host", required_argument, NULL, 'h'},
		{"host", required_argument, NULL, 'H'}, /* deprecated */
		{"filenode", required_argument, NULL, 'f'},
		{"indexes", no_argument, NULL, 'i'},
		{"oid", required_argument, NULL, 'o'},
		{"port", required_argument, NULL, 'p'},
		{"quiet", no_argument, NULL, 'q'},
		{"tablespaces", no_argument, NULL, 's'},
		{"system-objects", no_argument, NULL, 'S'},
		{"table", required_argument, NULL, 't'},
		{"username", required_argument, NULL, 'U'},
		{"version", no_argument, NULL, 'V'},
		{"extended", no_argument, NULL, 'x'},
		{"help", no_argument, NULL, '?'},
		{NULL, 0, NULL, 0}
	};

	int			c;
	const char *progname;
	int			optindex;

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);

	/* set the defaults */
	my_opts->quiet = false;
	my_opts->systables = false;
	my_opts->indexes = false;
	my_opts->nodb = false;
	my_opts->extended = false;
	my_opts->tablespaces = false;
	my_opts->dbname = NULL;
	my_opts->hostname = NULL;
	my_opts->port = NULL;
	my_opts->username = NULL;
	my_opts->progname = progname;

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			help(progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("oid2name (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	/* get opts */
	while ((c = getopt_long(argc, argv, "d:f:h:H:io:p:qsSt:U:x", long_options, &optindex)) != -1)
	{
		switch (c)
		{
				/* specify the database */
			case 'd':
				my_opts->dbname = pg_strdup(optarg);
				break;

				/* specify one filenode to show */
			case 'f':
				add_one_elt(optarg, my_opts->filenodes);
				break;

				/* host to connect to */
			case 'H':			/* deprecated */
			case 'h':
				my_opts->hostname = pg_strdup(optarg);
				break;

				/* also display indexes */
			case 'i':
				my_opts->indexes = true;
				break;

				/* specify one Oid to show */
			case 'o':
				add_one_elt(optarg, my_opts->oids);
				break;

				/* port to connect to on remote host */
			case 'p':
				my_opts->port = pg_strdup(optarg);
				break;

				/* don't show headers */
			case 'q':
				my_opts->quiet = true;
				break;

				/* dump tablespaces only */
			case 's':
				my_opts->tablespaces = true;
				break;

				/* display system tables */
			case 'S':
				my_opts->systables = true;
				break;

				/* specify one tablename to show */
			case 't':
				add_one_elt(optarg, my_opts->tables);
				break;

				/* username */
			case 'U':
				my_opts->username = pg_strdup(optarg);
				break;

				/* display extra columns */
			case 'x':
				my_opts->extended = true;
				break;

			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
				exit(1);
		}
	}

	if (optind < argc)
	{
		fprintf(stderr, _("%s: too many command-line arguments (first is \"%s\")\n"),
				progname, argv[optind]);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
		exit(1);
	}
}

static void
help(const char *progname)
{
	printf("%s helps examining the file structure used by PostgreSQL.\n\n"
		   "Usage:\n"
		   "  %s [OPTION]...\n"
		   "\nOptions:\n"
		   "  -f, --filenode=FILENODE    show info for table with given file node\n"
		   "  -i, --indexes              show indexes and sequences too\n"
		   "  -o, --oid=OID              show info for table with given OID\n"
		   "  -q, --quiet                quiet (don't show headers)\n"
		   "  -s, --tablespaces          show all tablespaces\n"
		   "  -S, --system-objects       show system objects too\n"
		   "  -t, --table=TABLE          show info for named table\n"
		   "  -V, --version              output version information, then exit\n"
		   "  -x, --extended             extended (show additional columns)\n"
		   "  -?, --help                 show this help, then exit\n"
		   "\nConnection options:\n"
		   "  -d, --dbname=DBNAME        database to connect to\n"
		   "  -h, --host=HOSTNAME        database server host or socket directory\n"
		   "  -H                         same as -h, deprecated option\n"
		   "  -p, --port=PORT            database server port number\n"
		   "  -U, --username=USERNAME    connect as specified database user\n"
		   "\nThe default action is to show all database OIDs.\n\n"
		   "Report bugs to <%s>.\n"
		   "%s home page: <%s>\n",
		   progname, progname, PACKAGE_BUGREPORT, PACKAGE_NAME, PACKAGE_URL);
}

/*
 * add_one_elt
 *
 * Add one element to a (possibly empty) eary struct.
 */
void
add_one_elt(char *eltname, eary *eary)
{
	if (eary->alloc == 0)
	{
		eary	  ->alloc = 8;
		eary	  ->array = (char **) pg_malloc(8 * sizeof(char *));
	}
	else if (eary->num >= eary->alloc)
	{
		eary	  ->alloc *= 2;
		eary	  ->array = (char **) pg_realloc(eary->array,
												 eary->alloc * sizeof(char *));
	}

	eary	  ->array[eary->num] = pg_strdup(eltname);
	eary	  ->num++;
}

/*
 * get_comma_elts
 *
 * Return the elements of an eary as a (freshly allocated) single string, in
 * single quotes, separated by commas and properly escaped for insertion in an
 * SQL statement.
 */
char *
get_comma_elts(eary *eary)
{
	char	   *ret,
			   *ptr;
	int			i,
				length = 0;

	if (eary->num == 0)
		return pg_strdup("");

	/*
	 * PQescapeString wants 2 * length + 1 bytes of breath space.  Add two
	 * chars per element for the single quotes and one for the comma.
	 */
	for (i = 0; i < eary->num; i++)
		length += strlen(eary->array[i]);

	ret = (char *) pg_malloc(length * 2 + 4 * eary->num);
	ptr = ret;

	for (i = 0; i < eary->num; i++)
	{
		if (i != 0)
			sprintf(ptr++, ",");
		sprintf(ptr++, "'");
		ptr += PQescapeString(ptr, eary->array[i], strlen(eary->array[i]));
		sprintf(ptr++, "'");
	}

	return ret;
}

/* establish connection with database. */
PGconn *
sql_conn(struct options *my_opts)
{
	PGconn	   *conn;
	char	   *password = NULL;
	bool		new_pass;
	PGresult   *res;

	/*
	 * Start the connection.  Loop until we have a password if requested by
	 * backend.
	 */
	do
	{
#define PARAMS_ARRAY_SIZE	7

		const char *keywords[PARAMS_ARRAY_SIZE];
		const char *values[PARAMS_ARRAY_SIZE];

		keywords[0] = "host";
		values[0] = my_opts->hostname;
		keywords[1] = "port";
		values[1] = my_opts->port;
		keywords[2] = "user";
		values[2] = my_opts->username;
		keywords[3] = "password";
		values[3] = password;
		keywords[4] = "dbname";
		values[4] = my_opts->dbname;
		keywords[5] = "fallback_application_name";
		values[5] = my_opts->progname;
		keywords[6] = NULL;
		values[6] = NULL;

		new_pass = false;
		conn = PQconnectdbParams(keywords, values, true);

		if (!conn)
		{
			pg_log_error("could not connect to database %s",
						 my_opts->dbname);
			exit(1);
		}

		if (PQstatus(conn) == CONNECTION_BAD &&
			PQconnectionNeedsPassword(conn) &&
			!password)
		{
			PQfinish(conn);
			password = simple_prompt("Password: ", false);
			new_pass = true;
		}
	} while (new_pass);

	/* check to see that the backend connection was successfully made */
	if (PQstatus(conn) == CONNECTION_BAD)
	{
		pg_log_error("%s", PQerrorMessage(conn));
		PQfinish(conn);
		exit(1);
	}

	res = PQexec(conn, ALWAYS_SECURE_SEARCH_PATH_SQL);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("could not clear search_path: %s",
					 PQerrorMessage(conn));
		PQclear(res);
		PQfinish(conn);
		exit(-1);
	}
	PQclear(res);

	/* return the conn if good */
	return conn;
}

/*
 * Actual code to make call to the database and print the output data.
 */
int
sql_exec(PGconn *conn, const char *todo, bool quiet)
{
	PGresult   *res;

	int			nfields;
	int			nrows;
	int			i,
				j,
				l;
	int		   *length;
	char	   *pad;

	/* make the call */
	res = PQexec(conn, todo);

	/* check and deal with errors */
	if (!res || PQresultStatus(res) > 2)
	{
		pg_log_error("query failed: %s", PQerrorMessage(conn));
		pg_log_error("query was: %s", todo);

		PQclear(res);
		PQfinish(conn);
		exit(-1);
	}

	/* get the number of fields */
	nrows = PQntuples(res);
	nfields = PQnfields(res);

	/* for each field, get the needed width */
	length = (int *) pg_malloc(sizeof(int) * nfields);
	for (j = 0; j < nfields; j++)
		length[j] = strlen(PQfname(res, j));

	for (i = 0; i < nrows; i++)
	{
		for (j = 0; j < nfields; j++)
		{
			l = strlen(PQgetvalue(res, i, j));
			if (l > length[j])
				length[j] = strlen(PQgetvalue(res, i, j));
		}
	}

	/* print a header */
	if (!quiet)
	{
		for (j = 0, l = 0; j < nfields; j++)
		{
			fprintf(stdout, "%*s", length[j] + 2, PQfname(res, j));
			l += length[j] + 2;
		}
		fprintf(stdout, "\n");
		pad = (char *) pg_malloc(l + 1);
		MemSet(pad, '-', l);
		pad[l] = '\0';
		fprintf(stdout, "%s\n", pad);
		free(pad);
	}

	/* for each row, dump the information */
	for (i = 0; i < nrows; i++)
	{
		for (j = 0; j < nfields; j++)
			fprintf(stdout, "%*s", length[j] + 2, PQgetvalue(res, i, j));
		fprintf(stdout, "\n");
	}

	/* cleanup */
	PQclear(res);
	free(length);

	return 0;
}

/*
 * Dump all databases.  There are no system objects to worry about.
 */
void
sql_exec_dumpalldbs(PGconn *conn, struct options *opts)
{
	char		todo[1024];

	/* get the oid and database name from the system pg_database table */
	snprintf(todo, sizeof(todo),
			 "SELECT d.oid AS \"Oid\", datname AS \"Database Name\", "
			 "spcname AS \"Tablespace\" FROM pg_catalog.pg_database d JOIN pg_catalog.pg_tablespace t ON "
			 "(dattablespace = t.oid) ORDER BY 2");

	sql_exec(conn, todo, opts->quiet);
}

/*
 * Dump all tables, indexes and sequences in the current database.
 */
void
sql_exec_dumpalltables(PGconn *conn, struct options *opts)
{
	char		todo[1024];
	char	   *addfields = ",c.oid AS \"Oid\", nspname AS \"Schema\", spcname as \"Tablespace\" ";

	snprintf(todo, sizeof(todo),
			 "SELECT pg_catalog.pg_relation_filenode(c.oid) as \"Filenode\", relname as \"Table Name\" %s "
			 "FROM pg_catalog.pg_class c "
			 "	LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
			 "	LEFT JOIN pg_catalog.pg_database d ON d.datname = pg_catalog.current_database(),"
			 "	pg_catalog.pg_tablespace t "
			 "WHERE relkind IN (" CppAsString2(RELKIND_RELATION) ","
			 CppAsString2(RELKIND_MATVIEW) "%s%s) AND "
			 "	%s"
			 "		t.oid = CASE"
			 "			WHEN reltablespace <> 0 THEN reltablespace"
			 "			ELSE dattablespace"
			 "		END "
			 "ORDER BY relname",
			 opts->extended ? addfields : "",
			 opts->indexes ? "," CppAsString2(RELKIND_INDEX) "," CppAsString2(RELKIND_SEQUENCE) : "",
			 opts->systables ? "," CppAsString2(RELKIND_TOASTVALUE) : "",
			 opts->systables ? "" : "n.nspname NOT IN ('pg_catalog', 'information_schema') AND n.nspname !~ '^pg_toast' AND");

	sql_exec(conn, todo, opts->quiet);
}

/*
 * Show oid, filenode, name, schema and tablespace for each of the
 * given objects in the current database.
 */
void
sql_exec_searchtables(PGconn *conn, struct options *opts)
{
	char	   *todo;
	char	   *qualifiers,
			   *ptr;
	char	   *comma_oids,
			   *comma_filenodes,
			   *comma_tables;
	bool		written = false;
	char	   *addfields = ",c.oid AS \"Oid\", nspname AS \"Schema\", spcname as \"Tablespace\" ";

	/* get tables qualifiers, whether names, filenodes, or OIDs */
	comma_oids = get_comma_elts(opts->oids);
	comma_tables = get_comma_elts(opts->tables);
	comma_filenodes = get_comma_elts(opts->filenodes);

	/* 80 extra chars for SQL expression */
	qualifiers = (char *) pg_malloc(strlen(comma_oids) + strlen(comma_tables) +
									strlen(comma_filenodes) + 80);
	ptr = qualifiers;

	if (opts->oids->num > 0)
	{
		ptr += sprintf(ptr, "c.oid IN (%s)", comma_oids);
		written = true;
	}
	if (opts->filenodes->num > 0)
	{
		if (written)
			ptr += sprintf(ptr, " OR ");
		ptr += sprintf(ptr, "pg_catalog.pg_relation_filenode(c.oid) IN (%s)", comma_filenodes);
		written = true;
	}
	if (opts->tables->num > 0)
	{
		if (written)
			ptr += sprintf(ptr, " OR ");
		sprintf(ptr, "c.relname ~~ ANY (ARRAY[%s])", comma_tables);
	}
	free(comma_oids);
	free(comma_tables);
	free(comma_filenodes);

	/* now build the query */
	todo = psprintf("SELECT pg_catalog.pg_relation_filenode(c.oid) as \"Filenode\", relname as \"Table Name\" %s\n"
					"FROM pg_catalog.pg_class c\n"
					"	LEFT JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace\n"
					"	LEFT JOIN pg_catalog.pg_database d ON d.datname = pg_catalog.current_database(),\n"
					"	pg_catalog.pg_tablespace t\n"
					"WHERE relkind IN (" CppAsString2(RELKIND_RELATION) ","
					CppAsString2(RELKIND_MATVIEW) ","
					CppAsString2(RELKIND_INDEX) ","
					CppAsString2(RELKIND_SEQUENCE) ","
					CppAsString2(RELKIND_TOASTVALUE) ") AND\n"
					"		t.oid = CASE\n"
					"			WHEN reltablespace <> 0 THEN reltablespace\n"
					"			ELSE dattablespace\n"
					"		END AND\n"
					"  (%s)\n"
					"ORDER BY relname\n",
					opts->extended ? addfields : "",
					qualifiers);

	free(qualifiers);

	sql_exec(conn, todo, opts->quiet);
}

void
sql_exec_dumpalltbspc(PGconn *conn, struct options *opts)
{
	char		todo[1024];

	snprintf(todo, sizeof(todo),
			 "SELECT oid AS \"Oid\", spcname as \"Tablespace Name\"\n"
			 "FROM pg_catalog.pg_tablespace");

	sql_exec(conn, todo, opts->quiet);
}

int
main(int argc, char **argv)
{
	struct options *my_opts;
	PGconn	   *pgconn;

	my_opts = (struct options *) pg_malloc(sizeof(struct options));

	my_opts->oids = (eary *) pg_malloc(sizeof(eary));
	my_opts->tables = (eary *) pg_malloc(sizeof(eary));
	my_opts->filenodes = (eary *) pg_malloc(sizeof(eary));

	my_opts->oids->num = my_opts->oids->alloc = 0;
	my_opts->tables->num = my_opts->tables->alloc = 0;
	my_opts->filenodes->num = my_opts->filenodes->alloc = 0;

	/* parse the opts */
	get_opts(argc, argv, my_opts);

	if (my_opts->dbname == NULL)
	{
		my_opts->dbname = "postgres";
		my_opts->nodb = true;
	}
	pgconn = sql_conn(my_opts);

	/* display only tablespaces */
	if (my_opts->tablespaces)
	{
		if (!my_opts->quiet)
			printf("All tablespaces:\n");
		sql_exec_dumpalltbspc(pgconn, my_opts);

		PQfinish(pgconn);
		exit(0);
	}

	/* display the given elements in the database */
	if (my_opts->oids->num > 0 ||
		my_opts->tables->num > 0 ||
		my_opts->filenodes->num > 0)
	{
		if (!my_opts->quiet)
			printf("From database \"%s\":\n", my_opts->dbname);
		sql_exec_searchtables(pgconn, my_opts);

		PQfinish(pgconn);
		exit(0);
	}

	/* no elements given; dump the given database */
	if (my_opts->dbname && !my_opts->nodb)
	{
		if (!my_opts->quiet)
			printf("From database \"%s\":\n", my_opts->dbname);
		sql_exec_dumpalltables(pgconn, my_opts);

		PQfinish(pgconn);
		exit(0);
	}

	/* no database either; dump all databases */
	if (!my_opts->quiet)
		printf("All databases:\n");
	sql_exec_dumpalldbs(pgconn, my_opts);

	PQfinish(pgconn);
	return 0;
}
