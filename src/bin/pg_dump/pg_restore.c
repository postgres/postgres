/*-------------------------------------------------------------------------
 *
 * pg_restore.c
 *	pg_restore is an utility extracting postgres database definitions
 *	from a backup archive created by pg_dump using the archiver
 *	interface.
 *
 *	pg_restore will read the backup archive and
 *	dump out a script that reproduces
 *	the schema of the database in terms of
 *		  user-defined types
 *		  user-defined functions
 *		  tables
 *		  indexes
 *		  aggregates
 *		  operators
 *		  ACL - grant/revoke
 *
 * the output script is SQL that is understood by PostgreSQL
 *
 * Basic process in a restore operation is:
 *
 *	Open the Archive and read the TOC.
 *	Set flags in TOC entries, and *maybe* reorder them.
 *	Generate script to stdout
 *	Exit
 *
 * Copyright (c) 2000, Philip Warner
 *		Rights are granted to use this software in any way so long
 *		as this notice is not removed.
 *
 *	The author is not responsible for loss or damages that may
 *	result from its use.
 *
 *
 * IDENTIFICATION
 *		$Header: /cvsroot/pgsql/src/bin/pg_dump/pg_restore.c,v 1.53.2.1 2005/04/30 08:00:55 neilc Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "pg_backup.h"
#include "pg_backup_archiver.h"
#include "dumputils.h"

#include <ctype.h>

#ifndef HAVE_STRDUP
#include "strdup.h"
#endif

#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif

#include <unistd.h>

#include "getopt_long.h"

#ifndef HAVE_OPTRESET
int			optreset;
#endif

#ifdef ENABLE_NLS
#include <locale.h>
#endif

#define _(x) gettext((x))

/* Forward decls */
static void usage(const char *progname);

typedef struct option optType;

int
main(int argc, char **argv)
{
	RestoreOptions *opts;
	int			c;
	Archive    *AH;
	char	   *inputFileSpec;
	extern int	optind;
	extern char *optarg;
	static int	use_setsessauth = 0;
	static int	disable_triggers = 0;

	struct option cmdopts[] = {
		{"clean", 0, NULL, 'c'},
		{"create", 0, NULL, 'C'},
		{"data-only", 0, NULL, 'a'},
		{"dbname", 1, NULL, 'd'},
		{"file", 1, NULL, 'f'},
		{"format", 1, NULL, 'F'},
		{"function", 1, NULL, 'P'},
		{"host", 1, NULL, 'h'},
		{"ignore-version", 0, NULL, 'i'},
		{"index", 1, NULL, 'I'},
		{"list", 0, NULL, 'l'},
		{"no-privileges", 0, NULL, 'x'},
		{"no-acl", 0, NULL, 'x'},
		{"no-owner", 0, NULL, 'O'},
		{"no-reconnect", 0, NULL, 'R'},
		{"port", 1, NULL, 'p'},
		{"oid-order", 0, NULL, 'o'},
		{"orig-order", 0, NULL, 'N'},
		{"password", 0, NULL, 'W'},
		{"rearrange", 0, NULL, 'r'},
		{"schema-only", 0, NULL, 's'},
		{"superuser", 1, NULL, 'S'},
		{"table", 1, NULL, 't'},
		{"trigger", 1, NULL, 'T'},
		{"use-list", 1, NULL, 'L'},
		{"username", 1, NULL, 'U'},
		{"verbose", 0, NULL, 'v'},

		/*
		 * the following options don't have an equivalent short option
		 * letter, but are available as '-X long-name'
		 */
		{"use-set-session-authorization", no_argument, &use_setsessauth, 1},
		{"disable-triggers", no_argument, &disable_triggers, 1},

		{NULL, 0, NULL, 0}
	};

#ifdef ENABLE_NLS
	setlocale(LC_ALL, "");
	bindtextdomain("pg_dump", LOCALEDIR);
	textdomain("pg_dump");
#endif

	opts = NewRestoreOptions();

	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage(progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_restore (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((c = getopt_long(argc, argv, "acCd:f:F:h:iI:lL:NoOp:P:rRsS:t:T:uU:vWxX:",
							cmdopts, NULL)) != -1)
	{
		switch (c)
		{
			case 'a':			/* Dump data only */
				opts->dataOnly = 1;
				break;
			case 'c':			/* clean (i.e., drop) schema prior to
								 * create */
				opts->dropSchema = 1;
				break;
			case 'C':
				opts->create = 1;
				break;
			case 'd':
				opts->dbname = strdup(optarg);
				break;
			case 'f':			/* output file name */
				opts->filename = strdup(optarg);
				break;
			case 'F':
				if (strlen(optarg) != 0)
					opts->formatName = strdup(optarg);
				break;
			case 'h':
				if (strlen(optarg) != 0)
					opts->pghost = strdup(optarg);
				break;
			case 'i':
				opts->ignoreVersion = 1;
				break;

			case 'l':			/* Dump the TOC summary */
				opts->tocSummary = 1;
				break;

			case 'L':			/* input TOC summary file name */
				opts->tocFile = strdup(optarg);
				break;

			case 'N':
				opts->origOrder = 1;
				break;
			case 'o':
				opts->oidOrder = 1;
				break;
			case 'O':
				opts->noOwner = 1;
				break;
			case 'p':
				if (strlen(optarg) != 0)
					opts->pgport = strdup(optarg);
				break;
			case 'r':
				opts->rearrange = 1;
				break;
			case 'R':
				/* no-op, still accepted for backwards compatibility */
				break;
			case 'P':			/* Function */
				opts->selTypes = 1;
				opts->selFunction = 1;
				opts->functionNames = strdup(optarg);
				break;
			case 'I':			/* Index */
				opts->selTypes = 1;
				opts->selIndex = 1;
				opts->indexNames = strdup(optarg);
				break;
			case 'T':			/* Trigger */
				opts->selTypes = 1;
				opts->selTrigger = 1;
				opts->triggerNames = strdup(optarg);
				break;
			case 's':			/* dump schema only */
				opts->schemaOnly = 1;
				break;
			case 'S':			/* Superuser username */
				if (strlen(optarg) != 0)
					opts->superuser = strdup(optarg);
				break;
			case 't':			/* Dump data for this table only */
				opts->selTypes = 1;
				opts->selTable = 1;
				opts->tableNames = strdup(optarg);
				break;

			case 'u':
				opts->requirePassword = true;
				opts->username = simple_prompt("User name: ", 100, true);
				break;

			case 'U':
				opts->username = optarg;
				break;

			case 'v':			/* verbose */
				opts->verbose = 1;
				break;

			case 'W':
				opts->requirePassword = true;
				break;

			case 'x':			/* skip ACL dump */
				opts->aclsSkip = 1;
				break;

			case 'X':
				if (strcmp(optarg, "use-set-session-authorization") == 0)
					/* no-op, still allowed for compatibility */ ;
				else if (strcmp(optarg, "disable-triggers") == 0)
					disable_triggers = 1;
				else
				{
					fprintf(stderr,
							_("%s: invalid -X option -- %s\n"),
							progname, optarg);
					fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
					exit(1);
				}
				break;

				/* This covers the long options equivalent to -X xxx. */
			case 0:
				break;

			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
				exit(1);
		}
	}

	if (optind < argc)
		inputFileSpec = argv[optind];
	else
		inputFileSpec = NULL;

	/* Should get at most one of -d and -f, else user is confused */
	if (opts->dbname)
	{
		if (opts->filename)
		{
			fprintf(stderr, _("%s: cannot specify both -d and -f output\n"),
					progname);
			fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
					progname);
			exit(1);
		}
		opts->useDB = 1;
	}

	opts->disable_triggers = disable_triggers;

	if (opts->formatName)
	{

		switch (opts->formatName[0])
		{

			case 'c':
			case 'C':
				opts->format = archCustom;
				break;

			case 'f':
			case 'F':
				opts->format = archFiles;
				break;

			case 't':
			case 'T':
				opts->format = archTar;
				break;

			default:
				write_msg(NULL, "unrecognized archive format '%s'; please specify 't' or 'c'\n",
						  opts->formatName);
				exit(1);
		}
	}

	AH = OpenArchive(inputFileSpec, opts->format);

	/* Let the archiver know how noisy to be */
	AH->verbose = opts->verbose;

	if (opts->tocFile)
		SortTocFromFile(AH, opts);

	if (opts->oidOrder)
		SortTocByOID(AH);
	else if (opts->origOrder)
		SortTocByID(AH);

	if (opts->rearrange)
		SortTocByObjectType(AH);
	else
	{
		/* Database MUST be at start (see also SortTocByObjectType) */
		MoveToStart(AH, "DATABASE");
	}

	if (opts->tocSummary)
		PrintTOCSummary(AH, opts);
	else
		RestoreArchive(AH, opts);

	CloseArchive(AH);

	return 0;
}

static void
usage(const char *progname)
{
	printf(_("%s restores a PostgreSQL database from an archive created by pg_dump.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [FILE]\n"), progname);

	printf(_("\nGeneral options:\n"));
	printf(_("  -d, --dbname=NAME        output database name\n"));
	printf(_("  -f, --file=FILENAME      output file name\n"));
	printf(_("  -F, --format=c|t         specify backup file format\n"));
	printf(_("  -i, --ignore-version     proceed even when server version mismatches\n"));
	printf(_("  -l, --list               print summarized TOC of the archive\n"));
	printf(_("  -v, --verbose            verbose mode\n"));
	printf(_("  --help                   show this help, then exit\n"));
	printf(_("  --version                output version information, then exit\n"));

	printf(_("\nOptions controlling the output content:\n"));
	printf(_("  -a, --data-only          restore only the data, no schema\n"));
	printf(_("  -c, --clean              clean (drop) schema prior to create\n"));
	printf(_("  -C, --create             issue commands to create the database\n"));
	printf(_("  -I, --index=NAME         restore named index\n"));
	printf(_("  -L, --use-list=FILENAME  use specified table of contents for ordering\n"
			 "                           output from this file\n"));
	printf(_("  -N, --orig-order         restore in original dump order\n"));
	printf(_("  -o, --oid-order          restore in OID order\n"));
	printf(_("  -O, --no-owner           do not output commands to set object ownership\n"));
	printf(_("  -P, --function=NAME(args)\n"
			 "                           restore named function\n"));
	printf(_("  -r, --rearrange          rearrange output to put indexes etc. at end\n"));
	printf(_("  -s, --schema-only        restore only the schema, no data\n"));
	printf(_("  -S, --superuser=NAME     specify the superuser user name to use for\n"
			 "                           disabling triggers\n"));
	printf(_("  -t, --table=NAME         restore named table\n"));
	printf(_("  -T, --trigger=NAME       restore named trigger\n"));
	printf(_("  -x, --no-privileges      skip restoration of access privileges (grant/revoke)\n"));
	printf(_("  -X disable-triggers, --disable-triggers\n"
			 "                           disable triggers during data-only restore\n"));

	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME      database server host or socket directory\n"));
	printf(_("  -p, --port=PORT          database server port number\n"));
	printf(_("  -U, --username=NAME      connect as specified database user\n"));
	printf(_("  -W, --password           force password prompt (should happen automatically)\n"));

	printf(_("\nIf no input file name is supplied, then standard input is used.\n\n"));
	printf(_("Report bugs to <pgsql-bugs@postgresql.org>.\n"));
}
