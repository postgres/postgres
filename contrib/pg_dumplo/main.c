/* -------------------------------------------------------------------------
 * pg_dumplo
 *
 * $Header: /cvsroot/pgsql/contrib/pg_dumplo/Attic/main.c,v 1.18 2003/08/07 21:11:57 tgl Exp $
 *
 *					Karel Zak 1999-2000
 * -------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <errno.h>
#include <unistd.h>

#include "libpq-fe.h"
#include "libpq/libpq-fs.h"

#include "pg_dumplo.h"

#ifndef HAVE_STRDUP
#include "strdup.h"
#endif

#include "getopt_long.h"

#ifndef HAVE_OPTRESET
int			optreset;
#endif

char	   *progname = NULL;

int			main(int argc, char **argv);
static void usage(void);
static void parse_lolist(LODumpMaster * pgLO);


/*-----
 *	The mother of all C functions
 *-----
 */
int
main(int argc, char **argv)
{
	LODumpMaster _pgLO,
			   *pgLO = &_pgLO;
	char	   *pwd = NULL;

	pgLO->argv = argv;
	pgLO->argc = argc;
	pgLO->action = 0;
	pgLO->lolist = NULL;
	pgLO->user = NULL;
	pgLO->db = NULL;
	pgLO->host = NULL;
	pgLO->port = NULL;
	pgLO->space = NULL;
	pgLO->index = NULL;
	pgLO->remove = FALSE;
	pgLO->quiet = FALSE;
	pgLO->counter = 0;
	pgLO->lolist_start = 0;

	progname = argv[0];

	/*
	 * Parse ARGV
	 */
	if (argc > 1)
	{
		int			arg;
		extern int	optind;
		int			l_index = 0;
		static struct option l_opt[] = {
			{"help", no_argument, 0, 'h'},
			{"user", required_argument, 0, 'u'},
			{"pwd", required_argument, 0, 'p'},
			{"db", required_argument, 0, 'd'},
			{"host", required_argument, 0, 'h'},
			{"port", required_argument, 0, 'o'},
			{"space", required_argument, 0, 's'},
			{"import", no_argument, 0, 'i'},
			{"export", no_argument, 0, 'e'},
			{"remove", no_argument, 0, 'r'},
			{"quiet", no_argument, 0, 'q'},
			{"all", no_argument, 0, 'a'},
			{"show", no_argument, 0, 'w'},
			{NULL, 0, 0, 0}
		};

		while ((arg = getopt_long(argc, argv, "?aeho:u:p:qd:l:t:irs:w", l_opt, &l_index)) != -1)
		{
			switch (arg)
			{
				case '?':
				case 'h':
					usage();
					exit(RE_OK);
				case 'u':
					pgLO->user = strdup(optarg);
					break;
				case 't':
					pgLO->host = strdup(optarg);
					break;
				case 'o':
					pgLO->port = strdup(optarg);
					break;
				case 'p':
					pwd = strdup(optarg);
					break;
				case 'd':
					pgLO->db = strdup(optarg);
					break;
				case 's':
					pgLO->space = strdup(optarg);
					break;
				case 'i':
					pgLO->action = ACTION_IMPORT;
					break;
				case 'l':
					pgLO->action = ACTION_EXPORT_ATTR;
					pgLO->lolist_start = optind - 1;
					parse_lolist(pgLO);
					break;
				case 'e':
				case 'a':
					pgLO->action = ACTION_EXPORT_ALL;
					break;
				case 'w':
					pgLO->action = ACTION_SHOW;
					break;
				case 'r':
					pgLO->remove = TRUE;
					break;
				case 'q':
					pgLO->quiet = TRUE;
					break;
				default:
					fprintf(stderr, "%s: bad arg -%c\n", progname, arg);
					usage();
					exit(RE_ERROR);
			}
		}
	}
	else
	{
		usage();
		exit(RE_ERROR);
	}

	/*
	 * Check space
	 */
	if (!pgLO->space && !pgLO->action == ACTION_SHOW)
	{
		if (!(pgLO->space = getenv("PWD")))
		{
			fprintf(stderr, "%s: not set space for dump-tree (option '-s' or $PWD).\n", progname);
			exit(RE_ERROR);
		}
	}

	if (!pgLO->action)
	{
		fprintf(stderr, "%s: What do you want - export or import?\n", progname);
		exit(RE_ERROR);
	}

	/*
	 * Make connection
	 */
	pgLO->conn = PQsetdbLogin(pgLO->host, pgLO->port, NULL, NULL, pgLO->db,
							  pgLO->user, pwd);

	if (PQstatus(pgLO->conn) == CONNECTION_BAD)
	{
		fprintf(stderr, "%s (connection): %s\n", progname, PQerrorMessage(pgLO->conn));
		exit(RE_ERROR);
	}
	pgLO->host = PQhost(pgLO->conn) ? PQhost(pgLO->conn) : "localhost";
	pgLO->db = PQdb(pgLO->conn);
	pgLO->user = PQuser(pgLO->conn);


	/*
	 * Init index file
	 */
	if (pgLO->action != ACTION_SHOW)
		index_file(pgLO);

	PQexec(pgLO->conn, "SET search_path = public");

	PQexec(pgLO->conn, "BEGIN");

	switch (pgLO->action)
	{

		case ACTION_SHOW:
		case ACTION_EXPORT_ALL:
			load_lolist(pgLO);
			/* FALL THROUGH */

		case ACTION_EXPORT_ATTR:
			pglo_export(pgLO);
			if (!pgLO->quiet)
			{
				if (pgLO->action == ACTION_SHOW)
					printf("\nDatabase '%s' contains %d large objects.\n\n", pgLO->db, pgLO->counter);
				else
					printf("\nExported %d large objects.\n\n", pgLO->counter);
			}
			break;

		case ACTION_IMPORT:
			pglo_import(pgLO);
			if (!pgLO->quiet)
				printf("\nImported %d large objects.\n\n", pgLO->counter);
			break;
	}

	PQexec(pgLO->conn, "COMMIT");
	PQfinish(pgLO->conn);

	if (pgLO->action != ACTION_SHOW)
		fclose(pgLO->index);

	exit(RE_OK);
}

static void
parse_lolist(LODumpMaster * pgLO)
{
	LOlist	   *ll;
	char	  **d,
			   *loc,
				buff[MAX_TABLE_NAME + MAX_ATTR_NAME + 1];

	pgLO->lolist = (LOlist *) malloc(pgLO->argc * sizeof(LOlist));

	if (!pgLO->lolist)
	{
		fprintf(stderr, "%s: can't allocate memory\n", progname);
		exit(RE_ERROR);
	}

	for (d = pgLO->argv + pgLO->lolist_start, ll = pgLO->lolist;
		 *d != NULL;
		 d++, ll++)
	{

		strncpy(buff, *d, MAX_TABLE_NAME + MAX_ATTR_NAME);

		if ((loc = strchr(buff, '.')) == NULL)
		{
			fprintf(stderr, "%s: '%s' is bad 'table.attr'\n", progname, buff);
			exit(RE_ERROR);
		}
		*loc = '\0';
		ll->lo_table = strdup(buff);
		ll->lo_attr = strdup(++loc);
	}
	ll++;
	ll->lo_table = ll->lo_attr = (char *) NULL;
}


static void
usage()
{
	printf("\npg_dumplo %s - PostgreSQL large objects dump\n", PG_VERSION);
	puts("pg_dumplo [option]\n\n"
		 "-h --help                    this help\n"
	   "-u --user=<username>         username for connection to server\n"
	   "-p --password=<password>     password for connection to server\n"
		 "-d --db=<database>           database name\n"
		 "-t --host=<hostname>         server hostname\n"
	"-o --port=<port>             database server port (default: 5432)\n"
		 "-s --space=<dir>             directory with dump tree (for export/import)\n"
		 "-i --import                  import large obj dump tree to DB\n"
	"-e --export                  export (dump) large obj to dump tree\n"
		 "-l <table.attr ...>          dump attribute (columns) with LO to dump tree\n"
		 "-a --all                     dump all LO in DB (default)\n"
		 "-r --remove                  if is set '-i' try remove old LO\n"
		 "-q --quiet                   run quietly\n"
		 "-w --show                    not dump, but show all LO in DB\n"
		 "\n"
		 "Example (dump):   pg_dumplo -d my_db -s /my_dump/dir -l t1.a t1.b t2.a\n"
		 "                  pg_dumplo -a -d my_db -s /my_dump/dir\n"
		 "Example (import): pg_dumplo -i -d my_db -s /my_dump/dir\n"
		 "Example (show):   pg_dumplo -w -d my_db\n\n"
		 "Note:  * option '-l' must be last option!\n"
	"       * option '-i' without option '-r' make new large obj in DB\n"
		 "         not rewrite old, the '-i' UPDATE oid numbers in table.attr only!\n"
		 "       * if option -s is not set, pg_dumplo uses $PWD\n"
		);						/* puts() */
}
