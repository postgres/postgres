/*-------------------------------------------------------------------------
 *
 * pg_dump.c
 *	  pg_dump is a utility for dumping out a postgres database
 *	  into a script file.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	pg_dump will read the system catalogs in a database and
 *	dump out a script that reproduces
 *	the schema of the database in terms of
 *		  user-defined types
 *		  user-defined functions
 *		  tables
 *		  indexes
 *		  aggregates
 *		  operators
 *		  privileges
 *
 * the output script is SQL that is understood by PostgreSQL
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/bin/pg_dump/pg_dump.c,v 1.241.2.3 2004/03/20 18:12:32 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

/*
 * Although this is not a backend module, we must include postgres.h anyway
 * so that we can include a bunch of backend include files.  pg_dump has
 * never pretended to be very independent of the backend anyhow ...
 */
#include "postgres.h"

#include <unistd.h>				/* for getopt() */
#include <ctype.h>
#ifdef ENABLE_NLS
#include <locale.h>
#endif
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif
#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif

#ifndef HAVE_STRDUP
#include "strdup.h"
#endif

#include "access/attnum.h"
#include "access/htup.h"
#include "catalog/pg_class.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"

#include "libpq-fe.h"
#include "libpq/libpq-fs.h"

#include "pg_dump.h"
#include "pg_backup.h"
#include "pg_backup_archiver.h"

typedef enum _formatLiteralOptions
{
	CONV_ALL = 0,
	PASS_LFTAB = 3				/* NOTE: 1 and 2 are reserved in case we
								 * want to make a mask. */
	/* We could make this a bit mask for control chars, but I don't */
	/* see any value in making it more complex...the current code */
	/* only checks for 'opts == CONV_ALL' anyway. */
} formatLiteralOptions;

static void dumpComment(Archive *fout, const char *target, const char *oid,
			const char *classname, int subid,
			const char *((*deps)[]));
static void dumpSequence(Archive *fout, TableInfo tbinfo, const bool schemaOnly, const bool dataOnly);
static void dumpACL(Archive *fout, TableInfo tbinfo);
static void dumpTriggers(Archive *fout, const char *tablename,
			 TableInfo *tblinfo, int numTables);
static void dumpRules(Archive *fout, const char *tablename,
		  TableInfo *tblinfo, int numTables);
static void formatStringLiteral(PQExpBuffer buf, const char *str, const formatLiteralOptions opts);
static void clearTableInfo(TableInfo *, int);
static void dumpOneFunc(Archive *fout, FuncInfo *finfo, int i,
			TypeInfo *tinfo, int numTypes);
static Oid	findLastBuiltinOid_V71(const char *);
static Oid	findLastBuiltinOid_V70(void);
static void setMaxOid(Archive *fout);

static void AddAcl(char *aclbuf, const char *keyword);
static char *GetPrivileges(Archive *AH, const char *s);

static int	dumpBlobs(Archive *AH, char *, void *);
static int	dumpDatabase(Archive *AH);
static PQExpBuffer getPKconstraint(TableInfo *tblInfo, IndInfo *indInfo);
static const char *getAttrName(int attrnum, TableInfo *tblInfo);

extern char *optarg;
extern int	optind,
			opterr;

/* global decls */
bool		g_verbose;			/* User wants verbose narration of our
								 * activities. */
Oid			g_last_builtin_oid; /* value of the last builtin oid */
Archive    *g_fout;				/* the script file */
PGconn	   *g_conn;				/* the database connection */

bool		force_quotes;		/* User wants to suppress double-quotes */
bool		dumpData;			/* dump data using proper insert strings */
bool		attrNames;			/* put attr names into insert strings */
bool		schemaOnly;
bool		dataOnly;
bool		aclsSkip;

char		g_opaque_type[10];	/* name for the opaque type */

/* placeholders for the delimiters for comments */
char		g_comment_start[10];
char		g_comment_end[10];


typedef struct _dumpContext
{
	TableInfo  *tblinfo;
	int			tblidx;
	bool		oids;
} DumpContext;

static void
help(const char *progname)
{
	printf(gettext("%s dumps a database as a text file or to other formats.\n\n"), progname);
	puts(gettext("Usage:"));
	printf(gettext("  %s [options] dbname\n\n"), progname);
	puts(gettext("Options:"));

#ifdef HAVE_GETOPT_LONG
	puts(gettext(
		"  -a, --data-only          dump only the data, not the schema\n"
			 "  -b, --blobs              include large objects in dump\n"
	   "  -c, --clean              clean (drop) schema prior to create\n"
				 "  -C, --create             include commands to create database in dump\n"
				 "  -d, --inserts            dump data as INSERT, rather than COPY, commands\n"
				 "  -D, --column-inserts     dump data as INSERT commands with column names\n"
				 "  -f, --file=FILENAME      output file name\n"
				 "  -F, --format {c|t|p}     output file format (custom, tar, plain text)\n"
				 "  -h, --host=HOSTNAME      database server host name\n"
				 "  -i, --ignore-version     proceed even when server version mismatches\n"
				 "                           pg_dump version\n"
	"  -n, --no-quotes          suppress most quotes around identifiers\n"
	 "  -N, --quotes             enable most quotes around identifiers\n"
				 "  -o, --oids               include oids in dump\n"
				 "  -O, --no-owner           do not output \\connect commands in plain\n"
				 "                           text format\n"
			   "  -p, --port=PORT          database server port number\n"
				 "  -R, --no-reconnect       disable ALL reconnections to the database in\n"
				 "                           plain text format\n"
			 "  -s, --schema-only        dump only the schema, no data\n"
				 "  -S, --superuser=NAME     specify the superuser user name to use in\n"
				 "                           plain text format\n"
		  "  -t, --table=TABLE        dump this table only (* for all)\n"
		"  -U, --username=NAME      connect as specified database user\n"
				 "  -v, --verbose            verbose mode\n"
				 "  -W, --password           force password prompt (should happen automatically)\n"
	 "  -x, --no-privileges      do not dump privileges (grant/revoke)\n"
				 "  -X use-set-session-authorization, --use-set-session-authorization\n"
				 "                           output SET SESSION AUTHORIZATION commands rather\n"
				 "                           than \\connect commands\n"
				 "  -Z, --compress {0-9}     compression level for compressed formats\n"
				 ));
#else
	puts(gettext(
		"  -a                       dump only the data, not the schema\n"
			 "  -b                       include large objects in dump\n"
	   "  -c                       clean (drop) schema prior to create\n"
				 "  -C                       include commands to create database in dump\n"
				 "  -d                       dump data as INSERT, rather than COPY, commands\n"
				 "  -D                       dump data as INSERT commands with column names\n"
				 "  -f FILENAME              output file name\n"
				 "  -F {c|t|p}               output file format (custom, tar, plain text)\n"
				 "  -h HOSTNAME              database server host name\n"
				 "  -i                       proceed even when server version mismatches\n"
				 "                           pg_dump version\n"
	"  -n                       suppress most quotes around identifiers\n"
	 "  -N                       enable most quotes around identifiers\n"
				 "  -o                       include oids in dump\n"
				 "  -O                       do not output \\connect commands in plain\n"
				 "                           text format\n"
			   "  -p PORT                  database server port number\n"
				 "  -R                       disable ALL reconnections to the database in\n"
				 "                           plain text format\n"
			 "  -s                       dump only the schema, no data\n"
				 "  -S NAME                  specify the superuser user name to use in\n"
				 "                           plain text format\n"
		  "  -t TABLE                 dump this table only (* for all)\n"
		"  -U NAME                  connect as specified database user\n"
				 "  -v                       verbose mode\n"
				 "  -W                       force password prompt (should happen automatically)\n"
	 "  -x                       do not dump privileges (grant/revoke)\n"
				 "  -X use-set-session-authorization\n"
				 "                           output SET SESSION AUTHORIZATION commands rather\n"
				 "                           than \\connect commands\n"
				 "  -Z {0-9}                 compression level for compressed formats\n"
				 ));
#endif
	puts(gettext("If no database name is not supplied, then the PGDATABASE environment\n"
				 "variable value is used.\n\n"
				 "Report bugs to <pgsql-bugs@postgresql.org>."));
}


void
exit_nicely(void)
{
	PQfinish(g_conn);
	if (g_verbose)
		write_msg(NULL, "*** aborted because of error\n");
	exit(1);
}


#define COPYBUFSIZ		8192

/*
 *	Dump a table's contents for loading using the COPY command
 *	- this routine is called by the Archiver when it wants the table
 *	  to be dumped.
 */

static int
dumpClasses_nodumpData(Archive *fout, char *oid, void *dctxv)
{
	const DumpContext *dctx = (DumpContext *) dctxv;
	const char *classname = dctx->tblinfo[dctx->tblidx].relname;
	const bool	hasoids = dctx->tblinfo[dctx->tblidx].hasoids;
	const bool	oids = dctx->oids;

	PGresult   *res;
	char		query[255];
	int			ret;
	bool		copydone;
	char		copybuf[COPYBUFSIZ];

	if (g_verbose)
		write_msg(NULL, "dumping out the contents of table %s\n", classname);

	if (oids && hasoids)
	{
		/*
		 * archprintf(fout, "COPY %s WITH OIDS FROM stdin;\n",
		 * fmtId(classname, force_quotes));
		 *
		 * - Not used as of V1.3 (needs to be in ArchiveEntry call)
		 *
		 */

		sprintf(query, "COPY %s WITH OIDS TO stdout;",
				fmtId(classname, force_quotes));
	}
	else
	{
		/*
		 * archprintf(fout, "COPY %s FROM stdin;\n", fmtId(classname,
		 * force_quotes));
		 *
		 * - Not used as of V1.3 (needs to be in ArchiveEntry call)
		 *
		 */

		sprintf(query, "COPY %s TO stdout;", fmtId(classname, force_quotes));
	}
	res = PQexec(g_conn, query);
	if (!res ||
		PQresultStatus(res) == PGRES_FATAL_ERROR)
	{
		write_msg(NULL, "SQL command to dump the contents of table \"%s\" failed\n",
				  classname);
		write_msg(NULL, "Error message from server: %s", PQerrorMessage(g_conn));
		write_msg(NULL, "The command was: %s\n", query);
		exit_nicely();
	}
	else
	{
		if (PQresultStatus(res) != PGRES_COPY_OUT)
		{
			write_msg(NULL, "SQL command to dump the contents of table \"%s\" executed abnormally.\n",
					  classname);
			write_msg(NULL, "The server returned status %d when %d was expected.\n",
					  PQresultStatus(res), PGRES_COPY_OUT);
			write_msg(NULL, "The command was: %s\n", query);
			exit_nicely();
		}
		else
		{
			copydone = false;

			while (!copydone)
			{
				ret = PQgetline(g_conn, copybuf, COPYBUFSIZ);

				if (copybuf[0] == '\\' &&
					copybuf[1] == '.' &&
					copybuf[2] == '\0')
				{
					copydone = true;	/* don't print this... */
				}
				else
				{
					archputs(copybuf, fout);
					switch (ret)
					{
						case EOF:
							copydone = true;
							/* FALLTHROUGH */
						case 0:
							archputc('\n', fout);
							break;
						case 1:
							break;
					}
				}

				/*
				 * THROTTLE:
				 *
				 * There was considerable discussion in late July, 2000
				 * regarding slowing down pg_dump when backing up large
				 * tables. Users with both slow & fast (muti-processor)
				 * machines experienced performance degradation when doing
				 * a backup.
				 *
				 * Initial attempts based on sleeping for a number of ms for
				 * each ms of work were deemed too complex, then a simple
				 * 'sleep in each loop' implementation was suggested. The
				 * latter failed because the loop was too tight. Finally,
				 * the following was implemented:
				 *
				 * If throttle is non-zero, then See how long since the last
				 * sleep. Work out how long to sleep (based on ratio). If
				 * sleep is more than 100ms, then sleep reset timer EndIf
				 * EndIf
				 *
				 * where the throttle value was the number of ms to sleep per
				 * ms of work. The calculation was done in each loop.
				 *
				 * Most of the hard work is done in the backend, and this
				 * solution still did not work particularly well: on slow
				 * machines, the ratio was 50:1, and on medium paced
				 * machines, 1:1, and on fast multi-processor machines, it
				 * had little or no effect, for reasons that were unclear.
				 *
				 * Further discussion ensued, and the proposal was dropped.
				 *
				 * For those people who want this feature, it can be
				 * implemented using gettimeofday in each loop,
				 * calculating the time since last sleep, multiplying that
				 * by the sleep ratio, then if the result is more than a
				 * preset 'minimum sleep time' (say 100ms), call the
				 * 'select' function to sleep for a subsecond period ie.
				 *
				 * select(0, NULL, NULL, NULL, &tvi);
				 *
				 * This will return after the interval specified in the
				 * structure tvi. Fianally, call gettimeofday again to
				 * save the 'last sleep time'.
				 */
			}
			archprintf(fout, "\\.\n");
		}
		ret = PQendcopy(g_conn);
		if (ret != 0)
		{
			write_msg(NULL, "SQL command to dump the contents of table \"%s\" failed: PQendcopy() failed.\n", classname);
			write_msg(NULL, "Error message from server: %s", PQerrorMessage(g_conn));
			write_msg(NULL, "The command was: %s\n", query);
			PQclear(res);
			exit_nicely();
		}
	}

	return 1;
}

static int
dumpClasses_dumpData(Archive *fout, char *oid, void *dctxv)
{
	const DumpContext *dctx = (DumpContext *) dctxv;
	const char *classname = dctx->tblinfo[dctx->tblidx].relname;

	PGresult   *res;
	PQExpBuffer q = createPQExpBuffer();
	int			tuple;
	int			field;

	if (fout->remoteVersion >= 70100)
		appendPQExpBuffer(q, "DECLARE _pg_dump_cursor CURSOR FOR SELECT * FROM ONLY %s", fmtId(classname, force_quotes));
	else
		appendPQExpBuffer(q, "DECLARE _pg_dump_cursor CURSOR FOR SELECT * FROM %s", fmtId(classname, force_quotes));

	res = PQexec(g_conn, q->data);
	if (!res ||
		PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		write_msg(NULL, "dumpClasses(): SQL command failed\n");
		write_msg(NULL, "Error message from server: %s", PQerrorMessage(g_conn));
		write_msg(NULL, "The command was: %s\n", q->data);
		exit_nicely();
	}

	do
	{
		PQclear(res);

		res = PQexec(g_conn, "FETCH 100 FROM _pg_dump_cursor");
		if (!res ||
			PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			write_msg(NULL, "dumpClasses(): SQL command failed\n");
			write_msg(NULL, "Error message from server: %s", PQerrorMessage(g_conn));
			write_msg(NULL, "The command was: FETCH 100 FROM _pg_dump_cursor\n");
			exit_nicely();
		}

		for (tuple = 0; tuple < PQntuples(res); tuple++)
		{
			archprintf(fout, "INSERT INTO %s ", fmtId(classname, force_quotes));
			if (attrNames == true)
			{
				resetPQExpBuffer(q);
				appendPQExpBuffer(q, "(");
				for (field = 0; field < PQnfields(res); field++)
				{
					if (field > 0)
						appendPQExpBuffer(q, ",");
					appendPQExpBuffer(q, fmtId(PQfname(res, field), force_quotes));
				}
				appendPQExpBuffer(q, ") ");
				archprintf(fout, "%s", q->data);
			}
			archprintf(fout, "VALUES (");
			for (field = 0; field < PQnfields(res); field++)
			{
				if (field > 0)
					archprintf(fout, ",");
				if (PQgetisnull(res, tuple, field))
				{
					archprintf(fout, "NULL");
					continue;
				}
				switch (PQftype(res, field))
				{
					case INT2OID:
					case INT4OID:
					case OIDOID:		/* int types */
					case FLOAT4OID:
					case FLOAT8OID:		/* float types */
						/* These types are printed without quotes */
						archprintf(fout, "%s",
								   PQgetvalue(res, tuple, field));
						break;
					case BITOID:
					case VARBITOID:
						archprintf(fout, "B'%s'",
								   PQgetvalue(res, tuple, field));
						break;
					default:

						/*
						 * All other types are printed as string literals,
						 * with appropriate escaping of special
						 * characters.
						 */
						resetPQExpBuffer(q);
						formatStringLiteral(q, PQgetvalue(res, tuple, field), CONV_ALL);
						archprintf(fout, "%s", q->data);
						break;
				}
			}
			archprintf(fout, ");\n");
		}

	} while (PQntuples(res) > 0);
	PQclear(res);

	res = PQexec(g_conn, "CLOSE _pg_dump_cursor");
	if (!res ||
		PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		write_msg(NULL, "dumpClasses(): SQL command failed\n");
		write_msg(NULL, "Error message from server: %s", PQerrorMessage(g_conn));
		write_msg(NULL, "The command was: CLOSE _pg_dump_cursor\n");
		exit_nicely();
	}
	PQclear(res);

	destroyPQExpBuffer(q);
	return 1;
}

/*
 * Convert a string value to an SQL string literal,
 * with appropriate escaping of special characters.
 * Quote mark ' goes to '' per SQL standard, other
 * stuff goes to \ sequences.
 * The literal is appended to the given PQExpBuffer.
 */
static void
formatStringLiteral(PQExpBuffer buf, const char *str, const formatLiteralOptions opts)
{
	appendPQExpBufferChar(buf, '\'');
	while (*str)
	{
		char		ch = *str++;

		if (ch == '\\' || ch == '\'')
		{
			appendPQExpBufferChar(buf, ch);		/* double these */
			appendPQExpBufferChar(buf, ch);
		}
		else if ((unsigned char) ch < (unsigned char) ' ' &&
				 (opts == CONV_ALL
				  || (ch != '\n' && ch != '\t')
				  ))
		{
			/*
			 * generate octal escape for control chars other than
			 * whitespace
			 */
			appendPQExpBufferChar(buf, '\\');
			appendPQExpBufferChar(buf, ((ch >> 6) & 3) + '0');
			appendPQExpBufferChar(buf, ((ch >> 3) & 7) + '0');
			appendPQExpBufferChar(buf, (ch & 7) + '0');
		}
		else
			appendPQExpBufferChar(buf, ch);
	}
	appendPQExpBufferChar(buf, '\'');
}

/*
 * DumpClasses -
 *	  dump the contents of all the classes.
 */
static void
dumpClasses(const TableInfo *tblinfo, const int numTables, Archive *fout,
		 const char *onlytable, const bool oids, const bool force_quotes)
{
	int			i;
	DataDumperPtr dumpFn;
	DumpContext *dumpCtx;
	char		copyBuf[512];
	char	   *copyStmt;

	if (g_verbose)
	{
		if (onlytable == NULL || (strlen(onlytable) == 0))
			write_msg(NULL, "preparing to dump the contents of all %d tables/sequences\n", numTables);
		else
			write_msg(NULL, "preparing to dump the contents of only one table/sequence\n");
	}

	for (i = 0; i < numTables; i++)
	{
		const char *classname = tblinfo[i].relname;

		/* Skip VIEW relations */
		if (tblinfo[i].viewdef != NULL)
			continue;

		if (tblinfo[i].sequence)	/* already dumped */
			continue;

		if (!onlytable || (strcmp(classname, onlytable) == 0) || (strlen(onlytable) == 0))
		{
			if (g_verbose)
				write_msg(NULL, "preparing to dump the contents of table %s\n", classname);

#if 0
			becomeUser(fout, tblinfo[i].usename);
#endif

			dumpCtx = (DumpContext *) malloc(sizeof(DumpContext));
			dumpCtx->tblinfo = (TableInfo *) tblinfo;
			dumpCtx->tblidx = i;
			dumpCtx->oids = oids;

			if (!dumpData)
			{
				/* Dump/restore using COPY */
				dumpFn = dumpClasses_nodumpData;
				sprintf(copyBuf, "COPY %s %sFROM stdin;\n",
						fmtId(tblinfo[i].relname, force_quotes),
						(oids && tblinfo[i].hasoids) ? "WITH OIDS " : "");
				copyStmt = copyBuf;
			}
			else
			{
				/* Restore using INSERT */
				dumpFn = dumpClasses_dumpData;
				copyStmt = NULL;
			}

			ArchiveEntry(fout, tblinfo[i].oid, tblinfo[i].relname,
						 "TABLE DATA", NULL, "", "",
						 copyStmt, tblinfo[i].usename,
						 dumpFn, dumpCtx);
		}
	}
}



static int
parse_version(const char *versionString)
{
	int			cnt;
	int			vmaj,
				vmin,
				vrev;

	cnt = sscanf(versionString, "%d.%d.%d", &vmaj, &vmin, &vrev);

	if (cnt < 2)
	{
		write_msg(NULL, "unable to parse version string \"%s\"\n", versionString);
		exit(1);
	}

	if (cnt == 2)
		vrev = 0;

	return (100 * vmaj + vmin) * 100 + vrev;
}



int
main(int argc, char **argv)
{
	int			c;
	const char *filename = NULL;
	const char *format = "p";
	const char *dbname = NULL;
	const char *pghost = NULL;
	const char *pgport = NULL;
	const char *username = NULL;
	char	   *tablename = NULL;
	bool		oids = false;
	TableInfo  *tblinfo;
	int			numTables;
	bool		force_password = false;
	int			compressLevel = -1;
	bool		ignore_version = false;
	int			plainText = 0;
	int			outputClean = 0;
	int			outputCreate = 0;
	int			outputBlobs = 0;
	int			outputNoOwner = 0;
	int			outputNoReconnect = 0;
	static int	use_setsessauth = 0;
	char	   *outputSuperuser = NULL;

	RestoreOptions *ropt;

#ifdef HAVE_GETOPT_LONG
	static struct option long_options[] = {
		{"data-only", no_argument, NULL, 'a'},
		{"blobs", no_argument, NULL, 'b'},
		{"clean", no_argument, NULL, 'c'},
		{"create", no_argument, NULL, 'C'},
		{"file", required_argument, NULL, 'f'},
		{"format", required_argument, NULL, 'F'},
		{"inserts", no_argument, NULL, 'd'},
		{"attribute-inserts", no_argument, NULL, 'D'},
		{"column-inserts", no_argument, NULL, 'D'},
		{"host", required_argument, NULL, 'h'},
		{"ignore-version", no_argument, NULL, 'i'},
		{"no-reconnect", no_argument, NULL, 'R'},
		{"no-quotes", no_argument, NULL, 'n'},
		{"quotes", no_argument, NULL, 'N'},
		{"oids", no_argument, NULL, 'o'},
		{"no-owner", no_argument, NULL, 'O'},
		{"port", required_argument, NULL, 'p'},
		{"schema-only", no_argument, NULL, 's'},
		{"superuser", required_argument, NULL, 'S'},
		{"table", required_argument, NULL, 't'},
		{"password", no_argument, NULL, 'W'},
		{"username", required_argument, NULL, 'U'},
		{"verbose", no_argument, NULL, 'v'},
		{"no-privileges", no_argument, NULL, 'x'},
		{"no-acl", no_argument, NULL, 'x'},
		{"compress", required_argument, NULL, 'Z'},
		{"help", no_argument, NULL, '?'},
		{"version", no_argument, NULL, 'V'},

		/*
		 * the following options don't have an equivalent short option
		 * letter, but are available as '-X long-name'
		 */
		{"use-set-session-authorization", no_argument, &use_setsessauth, 1},
		{NULL, 0, NULL, 0}
	};
	int			optindex;
#endif

#ifdef ENABLE_NLS
	setlocale(LC_ALL, "");
	bindtextdomain("pg_dump", LOCALEDIR);
	textdomain("pg_dump");
#endif

	g_verbose = false;
	force_quotes = true;

	strcpy(g_comment_start, "-- ");
	g_comment_end[0] = '\0';
	strcpy(g_opaque_type, "opaque");

	dataOnly = schemaOnly = dumpData = attrNames = false;

	if (!strrchr(argv[0], '/'))
		progname = argv[0];
	else
		progname = strrchr(argv[0], '/') + 1;

	/* Set default options based on progname */
	if (strcmp(progname, "pg_backup") == 0)
	{
		format = "c";
		outputBlobs = true;
	}

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			help(progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_dump (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

#ifdef HAVE_GETOPT_LONG
	while ((c = getopt_long(argc, argv, "abcCdDf:F:h:inNoOp:RsS:t:uU:vWxX:zZ:V?", long_options, &optindex)) != -1)
#else
	while ((c = getopt(argc, argv, "abcCdDf:F:h:inNoOp:RsS:t:uU:vWxX:zZ:V?-")) != -1)
#endif

	{
		switch (c)
		{
			case 'a':			/* Dump data only */
				dataOnly = true;
				break;

			case 'b':			/* Dump blobs */
				outputBlobs = true;
				break;

			case 'c':			/* clean (i.e., drop) schema prior to
								 * create */
				outputClean = 1;
				break;

			case 'C':			/* Create DB */

				outputCreate = 1;
				break;

			case 'd':			/* dump data as proper insert strings */
				dumpData = true;
				break;

			case 'D':			/* dump data as proper insert strings with
								 * attr names */
				dumpData = true;
				attrNames = true;
				break;

			case 'f':
				filename = optarg;
				break;

			case 'F':
				format = optarg;
				break;

			case 'h':			/* server host */
				pghost = optarg;
				break;

			case 'i':			/* ignore database version mismatch */
				ignore_version = true;
				break;

			case 'n':			/* Do not force double-quotes on
								 * identifiers */
				force_quotes = false;
				break;

			case 'N':			/* Force double-quotes on identifiers */
				force_quotes = true;
				break;

			case 'o':			/* Dump oids */
				oids = true;
				break;


			case 'O':			/* Don't reconnect to match owner */
				outputNoOwner = 1;
				break;

			case 'p':			/* server port */
				pgport = optarg;
				break;

			case 'R':			/* No reconnect */
				outputNoReconnect = 1;
				break;

			case 's':			/* dump schema only */
				schemaOnly = true;
				break;

			case 'S':			/* Username for superuser in plain text
								 * output */
				outputSuperuser = strdup(optarg);
				break;

			case 't':			/* Dump data for this table only */
				{
					int			i;

					tablename = strdup(optarg);

					/*
					 * quoted string? Then strip quotes and preserve
					 * case...
					 */
					if (tablename[0] == '"')
					{
						strcpy(tablename, &tablename[1]);
						if (*(tablename + strlen(tablename) - 1) == '"')
							*(tablename + strlen(tablename) - 1) = '\0';
					}
					/* otherwise, convert table name to lowercase... */
					else
					{
						for (i = 0; tablename[i]; i++)
							if (isupper((unsigned char) tablename[i]))
								tablename[i] = tolower((unsigned char) tablename[i]);

						/*
						 * '*' is a special case meaning ALL tables, but
						 * only if unquoted
						 */
						if (strcmp(tablename, "*") == 0)
							tablename[0] = '\0';

					}
				}
				break;

			case 'u':
				force_password = true;
				username = simple_prompt("User name: ", 100, true);
				break;

			case 'U':
				username = optarg;
				break;

			case 'v':			/* verbose */
				g_verbose = true;
				break;

			case 'W':
				force_password = true;
				break;

			case 'x':			/* skip ACL dump */
				aclsSkip = true;
				break;

				/*
				 * Option letters were getting scarce, so I invented this
				 * new scheme: '-X feature' turns on some feature. Compare
				 * to the -f option in GCC.  You should also add an
				 * equivalent GNU-style option --feature.  Features that
				 * require arguments should use '-X feature=foo'.
				 */
			case 'X':
				if (strcmp(optarg, "use-set-session-authorization") == 0)
					use_setsessauth = 1;
				else
				{
					fprintf(stderr,
							gettext("%s: invalid -X option -- %s\n"),
							progname, optarg);
					fprintf(stderr, gettext("Try '%s --help' for more information.\n"), progname);
					exit(1);
				}
				break;
			case 'Z':			/* Compression Level */
				compressLevel = atoi(optarg);
				break;

#ifndef HAVE_GETOPT_LONG
			case '-':
				fprintf(stderr,
						gettext("%s was compiled without support for long options.\n"
						 "Use --help for help on invocation options.\n"),
						progname);
				exit(1);
				break;
#else
				/* This covers the long options equivalent to -X xxx. */
			case 0:
				break;
#endif
			default:
				fprintf(stderr, gettext("Try '%s --help' for more information.\n"), progname);
				exit(1);
		}
	}

	if (optind < (argc - 1))
	{
		fprintf(stderr,
			gettext("%s: too many command line options (first is '%s')\n"
					"Try '%s --help' for more information.\n"),
				progname, argv[optind + 1], progname);
		exit(1);
	}

	/* Get the target database name */
	if (optind < argc)
		dbname = argv[optind];
	else
		dbname = getenv("PGDATABASE");
	if (!dbname)
	{
		write_msg(NULL, "no database name specified\n");
		exit(1);
	}

	if (dataOnly && schemaOnly)
	{
		write_msg(NULL, "The options \"schema only\" (-s) and \"data only\" (-a) cannot be used together.\n");
		exit(1);
	}

	if (outputBlobs && tablename != NULL && strlen(tablename) > 0)
	{
		write_msg(NULL, "Large object output is not supported for a single table.\n");
		write_msg(NULL, "Use all tables or a full dump instead.\n");
		exit(1);
	}

	if (dumpData == true && oids == true)
	{
		write_msg(NULL, "INSERT (-d, -D) and OID (-o) options cannot be used together.\n");
		write_msg(NULL, "(The INSERT command cannot set oids.)\n");
		exit(1);
	}

	if (outputBlobs == true && (format[0] == 'p' || format[0] == 'P'))
	{
		write_msg(NULL, "large object output is not supported for plain text dump files.\n");
		write_msg(NULL, "(Use a different output format.)\n");
		exit(1);
	}

	/* open the output file */
	switch (format[0])
	{

		case 'c':
		case 'C':
			g_fout = CreateArchive(filename, archCustom, compressLevel);
			break;

		case 'f':
		case 'F':
			g_fout = CreateArchive(filename, archFiles, compressLevel);
			break;

		case 'p':
		case 'P':
			plainText = 1;
			g_fout = CreateArchive(filename, archNull, 0);
			break;

		case 't':
		case 'T':
			g_fout = CreateArchive(filename, archTar, compressLevel);
			break;

		default:
			write_msg(NULL, "invalid output format '%s' specified\n", format);
			exit(1);
	}

	if (g_fout == NULL)
	{
		write_msg(NULL, "could not open output file %s for writing\n", filename);
		exit(1);
	}

	/* Let the archiver know how noisy to be */
	g_fout->verbose = g_verbose;

	/*
	 * Open the database using the Archiver, so it knows about it. Errors
	 * mean death.
	 */
	g_fout->minRemoteVersion = 70000;	/* we can handle back to 7.0 */
	g_fout->maxRemoteVersion = parse_version(PG_VERSION);
	g_conn = ConnectDatabase(g_fout, dbname, pghost, pgport, username, force_password, ignore_version);

	/*
	 * Start serializable transaction to dump consistent data
	 */
	{
		PGresult   *res;

		res = PQexec(g_conn, "begin");
		if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
			exit_horribly(g_fout, NULL, "BEGIN command failed: %s",
						  PQerrorMessage(g_conn));

		PQclear(res);
		res = PQexec(g_conn, "set transaction isolation level serializable");
		if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
			exit_horribly(g_fout, NULL, "could not set transaction isolation level to serializable: %s",
						  PQerrorMessage(g_conn));

		PQclear(res);
	}

	if (g_fout->remoteVersion >= 70100)
		g_last_builtin_oid = findLastBuiltinOid_V71(dbname);
	else
		g_last_builtin_oid = findLastBuiltinOid_V70();

	/* Dump the database definition */
	if (!dataOnly)
		dumpDatabase(g_fout);

	if (oids == true)
		setMaxOid(g_fout);

	if (g_verbose)
		write_msg(NULL, "last built-in oid is %u\n", g_last_builtin_oid);
	tblinfo = dumpSchema(g_fout, &numTables, tablename, aclsSkip, oids, schemaOnly, dataOnly);

	if (!schemaOnly)
		dumpClasses(tblinfo, numTables, g_fout, tablename, oids, force_quotes);

	if (outputBlobs)
		ArchiveEntry(g_fout, "0", "BLOBS", "BLOBS", NULL, "", "", "", "", dumpBlobs, 0);

	if (!dataOnly)				/* dump indexes and triggers at the end
								 * for performance */
	{
		dumpTriggers(g_fout, tablename, tblinfo, numTables);
		dumpRules(g_fout, tablename, tblinfo, numTables);
	}

	/* Now sort the output nicely */
	SortTocByOID(g_fout);
	MoveToStart(g_fout, "DATABASE");
	MoveToEnd(g_fout, "TABLE DATA");
	MoveToEnd(g_fout, "BLOBS");
	MoveToEnd(g_fout, "INDEX");
	MoveToEnd(g_fout, "CONSTRAINT");
	MoveToEnd(g_fout, "TRIGGER");
	MoveToEnd(g_fout, "RULE");
	MoveToEnd(g_fout, "SEQUENCE SET");

	/*
	 * Moving all comments to end is annoying, but must do it for comments
	 * on stuff we just moved, and we don't seem to have quite enough
	 * dependency structure to get it really right...
	 */
	MoveToEnd(g_fout, "COMMENT");

	if (plainText)
	{
		ropt = NewRestoreOptions();
		ropt->filename = (char *) filename;
		ropt->dropSchema = outputClean;
		ropt->aclsSkip = aclsSkip;
		ropt->superuser = outputSuperuser;
		ropt->create = outputCreate;
		ropt->noOwner = outputNoOwner;
		ropt->noReconnect = outputNoReconnect;
		ropt->use_setsessauth = use_setsessauth;

		if (outputSuperuser)
			ropt->superuser = outputSuperuser;
		else
			ropt->superuser = PQuser(g_conn);

		if (compressLevel == -1)
			ropt->compression = 0;
		else
			ropt->compression = compressLevel;

		ropt->suppressDumpWarnings = true;		/* We've already shown
												 * them */

		RestoreArchive(g_fout, ropt);
	}

	CloseArchive(g_fout);

	clearTableInfo(tblinfo, numTables);
	PQfinish(g_conn);
	exit(0);
}

/*
 * dumpDatabase:
 *	dump the database definition
 *
 */
static int
dumpDatabase(Archive *AH)
{
	PQExpBuffer dbQry = createPQExpBuffer();
	PQExpBuffer delQry = createPQExpBuffer();
	PQExpBuffer creaQry = createPQExpBuffer();
	PGresult   *res;
	int			ntups;
	int			i_dba,
				i_encoding,
				i_datpath;
	const char *datname,
			   *dba,
			   *encoding,
			   *datpath;

	datname = PQdb(g_conn);

	if (g_verbose)
		write_msg(NULL, "saving database definition\n");

	/* Get the database owner and parameters from pg_database */
	appendPQExpBuffer(dbQry, "select (select usename from pg_user where usesysid = datdba) as dba,"
					  " encoding, datpath from pg_database"
					  " where datname = ");
	formatStringLiteral(dbQry, datname, CONV_ALL);

	res = PQexec(g_conn, dbQry->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "SQL command failed\n");
		write_msg(NULL, "Error message from server: %s", PQerrorMessage(g_conn));
		write_msg(NULL, "The command was: %s\n", dbQry->data);
		exit_nicely();
	}

	ntups = PQntuples(res);

	if (ntups <= 0)
	{
		write_msg(NULL, "missing pg_database entry for database \"%s\"\n",
				  datname);
		exit_nicely();
	}

	if (ntups != 1)
	{
		write_msg(NULL, "query returned more than one (%d) pg_database entry for database \"%s\"\n",
				  ntups, datname);
		exit_nicely();
	}

	i_dba = PQfnumber(res, "dba");
	i_encoding = PQfnumber(res, "encoding");
	i_datpath = PQfnumber(res, "datpath");
	dba = PQgetvalue(res, 0, i_dba);
	encoding = PQgetvalue(res, 0, i_encoding);
	datpath = PQgetvalue(res, 0, i_datpath);

	appendPQExpBuffer(creaQry, "CREATE DATABASE %s WITH TEMPLATE = template0",
					  fmtId(datname, force_quotes));
	if (strlen(encoding) > 0)
		appendPQExpBuffer(creaQry, " ENCODING = %s", encoding);
	if (strlen(datpath) > 0)
		appendPQExpBuffer(creaQry, " LOCATION = '%s'", datpath);
	appendPQExpBuffer(creaQry, ";\n");

	appendPQExpBuffer(delQry, "DROP DATABASE %s;\n",
					  fmtId(datname, force_quotes));

	ArchiveEntry(AH, "0" /* OID */ , datname /* Name */ , "DATABASE", NULL,
				 creaQry->data /* Create */ , delQry->data /* Del */ ,
				 "" /* Copy */ , dba /* Owner */ ,
				 NULL /* Dumper */ , NULL /* Dumper Arg */ );

	PQclear(res);

	destroyPQExpBuffer(dbQry);
	destroyPQExpBuffer(delQry);
	destroyPQExpBuffer(creaQry);

	return 1;
}


/*
 * dumpBlobs:
 *	dump all blobs
 *
 */

#define loBufSize 16384
#define loFetchSize 1000

static int
dumpBlobs(Archive *AH, char *junkOid, void *junkVal)
{
	PQExpBuffer oidQry = createPQExpBuffer();
	PQExpBuffer oidFetchQry = createPQExpBuffer();
	PGresult   *res;
	int			i;
	int			loFd;
	char		buf[loBufSize];
	int			cnt;
	Oid			blobOid;

	if (g_verbose)
		write_msg(NULL, "saving large objects\n");

	/* Cursor to get all BLOB tables */
	if (AH->remoteVersion >= 70100)
		appendPQExpBuffer(oidQry, "Declare blobOid Cursor for SELECT DISTINCT loid FROM pg_largeobject");
	else
		appendPQExpBuffer(oidQry, "Declare blobOid Cursor for SELECT oid from pg_class where relkind = 'l'");

	res = PQexec(g_conn, oidQry->data);
	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		write_msg(NULL, "dumpBlobs(): cursor declaration failed: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}

	/* Fetch for cursor */
	appendPQExpBuffer(oidFetchQry, "Fetch %d in blobOid", loFetchSize);

	do
	{
		/* Do a fetch */
		PQclear(res);
		res = PQexec(g_conn, oidFetchQry->data);

		if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			write_msg(NULL, "dumpBlobs(): fetch from cursor failed: %s",
					  PQerrorMessage(g_conn));
			exit_nicely();
		}

		/* Process the tuples, if any */
		for (i = 0; i < PQntuples(res); i++)
		{
			blobOid = atooid(PQgetvalue(res, i, 0));
			/* Open the BLOB */
			loFd = lo_open(g_conn, blobOid, INV_READ);
			if (loFd == -1)
			{
				write_msg(NULL, "dumpBlobs(): could not open large object: %s",
						  PQerrorMessage(g_conn));
				exit_nicely();
			}

			StartBlob(AH, blobOid);

			/* Now read it in chunks, sending data to archive */
			do
			{
				cnt = lo_read(g_conn, loFd, buf, loBufSize);
				if (cnt < 0)
				{
					write_msg(NULL, "dumpBlobs(): error reading large object: %s",
							  PQerrorMessage(g_conn));
					exit_nicely();
				}

				WriteData(AH, buf, cnt);

			} while (cnt > 0);

			lo_close(g_conn, loFd);

			EndBlob(AH, blobOid);

		}
	} while (PQntuples(res) > 0);

	destroyPQExpBuffer(oidQry);
	destroyPQExpBuffer(oidFetchQry);

	return 1;
}

/*
 * getTypes:
 *	  read all base types in the system catalogs and return them in the
 * TypeInfo* structure
 *
 *	numTypes is set to the number of types read in
 *
 */
TypeInfo *
getTypes(int *numTypes)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	TypeInfo   *tinfo;

	int			i_oid;
	int			i_typowner;
	int			i_typname;
	int			i_typlen;
	int			i_typprtlen;
	int			i_typinput;
	int			i_typoutput;
	int			i_typreceive;
	int			i_typsend;
	int			i_typelem;
	int			i_typdelim;
	int			i_typdefault;
	int			i_typrelid;
	int			i_typalign;
	int			i_typstorage;
	int			i_typbyval;
	int			i_typisdefined;
	int			i_usename;
	int			i_typedefn;

	/* find all base types */

	/*
	 * we include even the built-in types because those may be used as
	 * array elements by user-defined types
	 *
	 * we filter out the built-in types when we dump out the types
	 */

	if (g_fout->remoteVersion < 70100)
	{
		appendPQExpBuffer(query, "SELECT pg_type.oid, typowner, typname, typlen, typprtlen, "
		  "typinput, typoutput, typreceive, typsend, typelem, typdelim, "
						  "typdefault, typrelid, typalign, 'p'::char as typstorage, typbyval, typisdefined, "
						  "(select usename from pg_user where typowner = usesysid) as usename, "
						  "typname as typedefn "
						  "from pg_type");
	}
	else
	{
		appendPQExpBuffer(query, "SELECT pg_type.oid, typowner, typname, typlen, typprtlen, "
		  "typinput, typoutput, typreceive, typsend, typelem, typdelim, "
						  "typdefault, typrelid, typalign, typstorage, typbyval, typisdefined, "
						  "(select usename from pg_user where typowner = usesysid) as usename, "
						  "format_type(pg_type.oid, NULL) as typedefn "
						  "from pg_type");
	}

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain list of data types failed: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}

	ntups = PQntuples(res);

	tinfo = (TypeInfo *) malloc(ntups * sizeof(TypeInfo));

	i_oid = PQfnumber(res, "oid");
	i_typowner = PQfnumber(res, "typowner");
	i_typname = PQfnumber(res, "typname");
	i_typlen = PQfnumber(res, "typlen");
	i_typprtlen = PQfnumber(res, "typprtlen");
	i_typinput = PQfnumber(res, "typinput");
	i_typoutput = PQfnumber(res, "typoutput");
	i_typreceive = PQfnumber(res, "typreceive");
	i_typsend = PQfnumber(res, "typsend");
	i_typelem = PQfnumber(res, "typelem");
	i_typdelim = PQfnumber(res, "typdelim");
	i_typdefault = PQfnumber(res, "typdefault");
	i_typrelid = PQfnumber(res, "typrelid");
	i_typalign = PQfnumber(res, "typalign");
	i_typstorage = PQfnumber(res, "typstorage");
	i_typbyval = PQfnumber(res, "typbyval");
	i_typisdefined = PQfnumber(res, "typisdefined");
	i_usename = PQfnumber(res, "usename");
	i_typedefn = PQfnumber(res, "typedefn");

	for (i = 0; i < ntups; i++)
	{
		tinfo[i].oid = strdup(PQgetvalue(res, i, i_oid));
		tinfo[i].typowner = strdup(PQgetvalue(res, i, i_typowner));
		tinfo[i].typname = strdup(PQgetvalue(res, i, i_typname));
		tinfo[i].typlen = strdup(PQgetvalue(res, i, i_typlen));
		tinfo[i].typprtlen = strdup(PQgetvalue(res, i, i_typprtlen));
		tinfo[i].typinput = strdup(PQgetvalue(res, i, i_typinput));
		tinfo[i].typoutput = strdup(PQgetvalue(res, i, i_typoutput));
		tinfo[i].typreceive = strdup(PQgetvalue(res, i, i_typreceive));
		tinfo[i].typsend = strdup(PQgetvalue(res, i, i_typsend));
		tinfo[i].typelem = strdup(PQgetvalue(res, i, i_typelem));
		tinfo[i].typdelim = strdup(PQgetvalue(res, i, i_typdelim));
		if (PQgetisnull(res, i, i_typdefault))
			tinfo[i].typdefault = NULL;
		else
			tinfo[i].typdefault = strdup(PQgetvalue(res, i, i_typdefault));
		tinfo[i].typrelid = strdup(PQgetvalue(res, i, i_typrelid));
		tinfo[i].typalign = strdup(PQgetvalue(res, i, i_typalign));
		tinfo[i].typstorage = strdup(PQgetvalue(res, i, i_typstorage));
		tinfo[i].usename = strdup(PQgetvalue(res, i, i_usename));
		tinfo[i].typedefn = strdup(PQgetvalue(res, i, i_typedefn));

		if (strlen(tinfo[i].usename) == 0)
			write_msg(NULL, "WARNING: owner of data type %s appears to be invalid\n",
					  tinfo[i].typname);

		if (strcmp(PQgetvalue(res, i, i_typbyval), "f") == 0)
			tinfo[i].passedbyvalue = 0;
		else
			tinfo[i].passedbyvalue = 1;

		/*
		 * check for user-defined array types, omit system generated ones
		 */
		if ((strcmp(tinfo[i].typelem, "0") != 0) &&
			tinfo[i].typname[0] != '_')
			tinfo[i].isArray = 1;
		else
			tinfo[i].isArray = 0;

		if (strcmp(PQgetvalue(res, i, i_typisdefined), "f") == 0)
			tinfo[i].isDefined = 0;
		else
			tinfo[i].isDefined = 1;
	}

	*numTypes = ntups;

	PQclear(res);

	destroyPQExpBuffer(query);

	return tinfo;
}

/*
 * getOperators:
 *	  read all operators in the system catalogs and return them in the
 * OprInfo* structure
 *
 *	numOprs is set to the number of operators read in
 */
OprInfo *
getOperators(int *numOprs)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();

	OprInfo    *oprinfo;

	int			i_oid;
	int			i_oprname;
	int			i_oprkind;
	int			i_oprcode;
	int			i_oprleft;
	int			i_oprright;
	int			i_oprcom;
	int			i_oprnegate;
	int			i_oprrest;
	int			i_oprjoin;
	int			i_oprcanhash;
	int			i_oprlsortop;
	int			i_oprrsortop;
	int			i_usename;

	/*
	 * find all operators, including builtin operators, filter out
	 * system-defined operators at dump-out time
	 */

	appendPQExpBuffer(query, "SELECT pg_operator.oid, oprname, oprkind, oprcode, "
			   "oprleft, oprright, oprcom, oprnegate, oprrest, oprjoin, "
					  "oprcanhash, oprlsortop, oprrsortop, "
	"(select usename from pg_user where oprowner = usesysid) as usename "
					  "from pg_operator");

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain list of operators failed: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}

	ntups = PQntuples(res);
	*numOprs = ntups;

	oprinfo = (OprInfo *) malloc(ntups * sizeof(OprInfo));

	i_oid = PQfnumber(res, "oid");
	i_oprname = PQfnumber(res, "oprname");
	i_oprkind = PQfnumber(res, "oprkind");
	i_oprcode = PQfnumber(res, "oprcode");
	i_oprleft = PQfnumber(res, "oprleft");
	i_oprright = PQfnumber(res, "oprright");
	i_oprcom = PQfnumber(res, "oprcom");
	i_oprnegate = PQfnumber(res, "oprnegate");
	i_oprrest = PQfnumber(res, "oprrest");
	i_oprjoin = PQfnumber(res, "oprjoin");
	i_oprcanhash = PQfnumber(res, "oprcanhash");
	i_oprlsortop = PQfnumber(res, "oprlsortop");
	i_oprrsortop = PQfnumber(res, "oprrsortop");
	i_usename = PQfnumber(res, "usename");

	for (i = 0; i < ntups; i++)
	{
		oprinfo[i].oid = strdup(PQgetvalue(res, i, i_oid));
		oprinfo[i].oprname = strdup(PQgetvalue(res, i, i_oprname));
		oprinfo[i].oprkind = strdup(PQgetvalue(res, i, i_oprkind));
		oprinfo[i].oprcode = strdup(PQgetvalue(res, i, i_oprcode));
		oprinfo[i].oprleft = strdup(PQgetvalue(res, i, i_oprleft));
		oprinfo[i].oprright = strdup(PQgetvalue(res, i, i_oprright));
		oprinfo[i].oprcom = strdup(PQgetvalue(res, i, i_oprcom));
		oprinfo[i].oprnegate = strdup(PQgetvalue(res, i, i_oprnegate));
		oprinfo[i].oprrest = strdup(PQgetvalue(res, i, i_oprrest));
		oprinfo[i].oprjoin = strdup(PQgetvalue(res, i, i_oprjoin));
		oprinfo[i].oprcanhash = strdup(PQgetvalue(res, i, i_oprcanhash));
		oprinfo[i].oprlsortop = strdup(PQgetvalue(res, i, i_oprlsortop));
		oprinfo[i].oprrsortop = strdup(PQgetvalue(res, i, i_oprrsortop));
		oprinfo[i].usename = strdup(PQgetvalue(res, i, i_usename));

		if (strlen(oprinfo[i].usename) == 0)
			write_msg(NULL, "WARNING: owner of operator \"%s\" appears to be invalid\n",
					  oprinfo[i].oprname);

	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return oprinfo;
}

void
clearTypeInfo(TypeInfo *tp, int numTypes)
{
	int			i;

	for (i = 0; i < numTypes; ++i)
	{
		if (tp[i].oid)
			free(tp[i].oid);
		if (tp[i].typowner)
			free(tp[i].typowner);
		if (tp[i].typname)
			free(tp[i].typname);
		if (tp[i].typlen)
			free(tp[i].typlen);
		if (tp[i].typprtlen)
			free(tp[i].typprtlen);
		if (tp[i].typinput)
			free(tp[i].typinput);
		if (tp[i].typoutput)
			free(tp[i].typoutput);
		if (tp[i].typreceive)
			free(tp[i].typreceive);
		if (tp[i].typsend)
			free(tp[i].typsend);
		if (tp[i].typelem)
			free(tp[i].typelem);
		if (tp[i].typdelim)
			free(tp[i].typdelim);
		if (tp[i].typdefault)
			free(tp[i].typdefault);
		if (tp[i].typrelid)
			free(tp[i].typrelid);
		if (tp[i].typalign)
			free(tp[i].typalign);
		if (tp[i].typstorage)
			free(tp[i].typstorage);
		if (tp[i].usename)
			free(tp[i].usename);
		if (tp[i].typedefn)
			free(tp[i].typedefn);
	}
	free(tp);
}

void
clearFuncInfo(FuncInfo *fun, int numFuncs)
{
	int			i,
				a;

	if (!fun)
		return;
	for (i = 0; i < numFuncs; ++i)
	{
		if (fun[i].oid)
			free(fun[i].oid);
		if (fun[i].proname)
			free(fun[i].proname);
		if (fun[i].usename)
			free(fun[i].usename);
		for (a = 0; a < FUNC_MAX_ARGS; ++a)
			if (fun[i].argtypes[a])
				free(fun[i].argtypes[a]);
		if (fun[i].prorettype)
			free(fun[i].prorettype);
		if (fun[i].prosrc)
			free(fun[i].prosrc);
		if (fun[i].probin)
			free(fun[i].probin);
	}
	free(fun);
}

static void
clearTableInfo(TableInfo *tblinfo, int numTables)
{
	int			i,
				j;

	for (i = 0; i < numTables; ++i)
	{

		if (tblinfo[i].oid)
			free(tblinfo[i].oid);
		if (tblinfo[i].relacl)
			free(tblinfo[i].relacl);
		if (tblinfo[i].usename)
			free(tblinfo[i].usename);

		if (tblinfo[i].relname)
			free(tblinfo[i].relname);

		if (tblinfo[i].sequence)
			continue;

		/* Process Attributes */
		for (j = 0; j < tblinfo[i].numatts; j++)
		{
			if (tblinfo[i].attnames[j])
				free(tblinfo[i].attnames[j]);
			if (tblinfo[i].typnames[j])
				free(tblinfo[i].typnames[j]);
		}

		if (tblinfo[i].triggers)
		{
			for (j = 0; j < tblinfo[i].ntrig; j++)
			{
				if (tblinfo[i].triggers[j].tgsrc)
					free(tblinfo[i].triggers[j].tgsrc);
				if (tblinfo[i].triggers[j].oid)
					free(tblinfo[i].triggers[j].oid);
				if (tblinfo[i].triggers[j].tgname)
					free(tblinfo[i].triggers[j].tgname);
				if (tblinfo[i].triggers[j].tgdel)
					free(tblinfo[i].triggers[j].tgdel);
			}
			free(tblinfo[i].triggers);
		}

		if (tblinfo[i].atttypmod)
			free((int *) tblinfo[i].atttypmod);
		if (tblinfo[i].inhAttrs)
			free((int *) tblinfo[i].inhAttrs);
		if (tblinfo[i].inhAttrDef)
			free((int *) tblinfo[i].inhAttrDef);
		if (tblinfo[i].inhNotNull)
			free((int *) tblinfo[i].inhNotNull);
		if (tblinfo[i].attnames)
			free(tblinfo[i].attnames);
		if (tblinfo[i].atttypedefns)
			free(tblinfo[i].atttypedefns);
		if (tblinfo[i].typnames)
			free(tblinfo[i].typnames);
		if (tblinfo[i].notnull)
			free(tblinfo[i].notnull);
		if (tblinfo[i].primary_key_name)
			free(tblinfo[i].primary_key_name);
	}
	free(tblinfo);
}

void
clearInhInfo(InhInfo *inh, int numInherits)
{
	int			i;

	if (!inh)
		return;
	for (i = 0; i < numInherits; ++i)
	{
		if (inh[i].inhrelid)
			free(inh[i].inhrelid);
		if (inh[i].inhparent)
			free(inh[i].inhparent);
	}
	free(inh);
}

void
clearOprInfo(OprInfo *opr, int numOprs)
{
	int			i;

	if (!opr)
		return;
	for (i = 0; i < numOprs; ++i)
	{
		if (opr[i].oid)
			free(opr[i].oid);
		if (opr[i].oprname)
			free(opr[i].oprname);
		if (opr[i].oprkind)
			free(opr[i].oprkind);
		if (opr[i].oprcode)
			free(opr[i].oprcode);
		if (opr[i].oprleft)
			free(opr[i].oprleft);
		if (opr[i].oprright)
			free(opr[i].oprright);
		if (opr[i].oprcom)
			free(opr[i].oprcom);
		if (opr[i].oprnegate)
			free(opr[i].oprnegate);
		if (opr[i].oprrest)
			free(opr[i].oprrest);
		if (opr[i].oprjoin)
			free(opr[i].oprjoin);
		if (opr[i].oprcanhash)
			free(opr[i].oprcanhash);
		if (opr[i].oprlsortop)
			free(opr[i].oprlsortop);
		if (opr[i].oprrsortop)
			free(opr[i].oprrsortop);
		if (opr[i].usename)
			free(opr[i].usename);
	}
	free(opr);
}

void
clearIndInfo(IndInfo *ind, int numIndexes)
{
	int			i,
				a;

	if (!ind)
		return;
	for (i = 0; i < numIndexes; ++i)
	{
		if (ind[i].indexreloid)
			free(ind[i].indexreloid);
		if (ind[i].indreloid)
			free(ind[i].indreloid);
		if (ind[i].indexrelname)
			free(ind[i].indexrelname);
		if (ind[i].indrelname)
			free(ind[i].indrelname);
		if (ind[i].indexdef)
			free(ind[i].indexdef);
		if (ind[i].indisprimary)
			free(ind[i].indisprimary);
		for (a = 0; a < INDEX_MAX_KEYS; ++a)
		{
			if (ind[i].indkey[a])
				free(ind[i].indkey[a]);
		}
	}
	free(ind);
}

void
clearAggInfo(AggInfo *agginfo, int numArgs)
{
	int			i;

	if (!agginfo)
		return;
	for (i = 0; i < numArgs; ++i)
	{
		if (agginfo[i].oid)
			free(agginfo[i].oid);
		if (agginfo[i].aggname)
			free(agginfo[i].aggname);
		if (agginfo[i].aggtransfn)
			free(agginfo[i].aggtransfn);
		if (agginfo[i].aggfinalfn)
			free(agginfo[i].aggfinalfn);
		if (agginfo[i].aggtranstype)
			free(agginfo[i].aggtranstype);
		if (agginfo[i].aggbasetype)
			free(agginfo[i].aggbasetype);
		if (agginfo[i].agginitval)
			free(agginfo[i].agginitval);
		if (agginfo[i].usename)
			free(agginfo[i].usename);
	}
	free(agginfo);
}

/*
 * getAggregates:
 *	  read all the user-defined aggregates in the system catalogs and
 * return them in the AggInfo* structure
 *
 * numAggs is set to the number of aggregates read in
 */
AggInfo *
getAggregates(int *numAggs)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	AggInfo    *agginfo;

	int			i_oid;
	int			i_aggname;
	int			i_aggtransfn;
	int			i_aggfinalfn;
	int			i_aggtranstype;
	int			i_aggbasetype;
	int			i_agginitval;
	int			i_usename;
	int			i_convertok;

	/* find all user-defined aggregates */

	if (g_fout->remoteVersion < 70100)
	{
		appendPQExpBuffer(query, "SELECT pg_aggregate.oid, aggname, aggtransfn1 as aggtransfn, "
			   "aggfinalfn, aggtranstype1 as aggtranstype, aggbasetype, "
						  "agginitval1 as agginitval, "
						  "(aggtransfn2 = 0 and aggtranstype2 = 0 and agginitval2 is null) as convertok, "
						  "(select usename from pg_user where aggowner = usesysid) as usename "
						  "from pg_aggregate");
	}
	else
	{
		appendPQExpBuffer(query, "SELECT pg_aggregate.oid, aggname, aggtransfn, "
						  "aggfinalfn, aggtranstype, aggbasetype, "
						  "agginitval, "
						  "'t'::boolean as convertok, "
						  "(select usename from pg_user where aggowner = usesysid) as usename "
						  "from pg_aggregate");
	}

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain list of aggregate functions failed: %s",
				  PQerrorMessage(g_conn));
		exit_nicely();
	}

	ntups = PQntuples(res);
	*numAggs = ntups;

	agginfo = (AggInfo *) malloc(ntups * sizeof(AggInfo));

	i_oid = PQfnumber(res, "oid");
	i_aggname = PQfnumber(res, "aggname");
	i_aggtransfn = PQfnumber(res, "aggtransfn");
	i_aggfinalfn = PQfnumber(res, "aggfinalfn");
	i_aggtranstype = PQfnumber(res, "aggtranstype");
	i_aggbasetype = PQfnumber(res, "aggbasetype");
	i_agginitval = PQfnumber(res, "agginitval");
	i_usename = PQfnumber(res, "usename");
	i_convertok = PQfnumber(res, "convertok");

	for (i = 0; i < ntups; i++)
	{
		agginfo[i].oid = strdup(PQgetvalue(res, i, i_oid));
		agginfo[i].aggname = strdup(PQgetvalue(res, i, i_aggname));
		agginfo[i].aggtransfn = strdup(PQgetvalue(res, i, i_aggtransfn));
		agginfo[i].aggfinalfn = strdup(PQgetvalue(res, i, i_aggfinalfn));
		agginfo[i].aggtranstype = strdup(PQgetvalue(res, i, i_aggtranstype));
		agginfo[i].aggbasetype = strdup(PQgetvalue(res, i, i_aggbasetype));
		if (PQgetisnull(res, i, i_agginitval))
			agginfo[i].agginitval = NULL;
		else
			agginfo[i].agginitval = strdup(PQgetvalue(res, i, i_agginitval));
		agginfo[i].usename = strdup(PQgetvalue(res, i, i_usename));
		if (strlen(agginfo[i].usename) == 0)
			write_msg(NULL, "WARNING: owner of aggregate function \"%s\" appears to be invalid\n",
					  agginfo[i].aggname);

		agginfo[i].convertok = (PQgetvalue(res, i, i_convertok)[0] == 't');

	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return agginfo;
}

/*
 * getFuncs:
 *	  read all the user-defined functions in the system catalogs and
 * return them in the FuncInfo* structure
 *
 * numFuncs is set to the number of functions read in
 *
 *
 */
FuncInfo *
getFuncs(int *numFuncs)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	FuncInfo   *finfo;

	int			i_oid;
	int			i_proname;
	int			i_prolang;
	int			i_pronargs;
	int			i_proargtypes;
	int			i_prorettype;
	int			i_proretset;
	int			i_prosrc;
	int			i_probin;
	int			i_iscachable;
	int			i_isstrict;
	int			i_usename;

	/* find all user-defined funcs */

	if (g_fout->remoteVersion < 70100)
	{
		appendPQExpBuffer(query,
		   "SELECT pg_proc.oid, proname, prolang, pronargs, prorettype, "
						  "proretset, proargtypes, prosrc, probin, "
						  "(select usename from pg_user where proowner = usesysid) as usename, "
						  "proiscachable, 'f'::boolean as proisstrict "
						  "from pg_proc "
						  "where pg_proc.oid > '%u'::oid",
						  g_last_builtin_oid);
	}
	else
	{
		appendPQExpBuffer(query,
		   "SELECT pg_proc.oid, proname, prolang, pronargs, prorettype, "
						  "proretset, proargtypes, prosrc, probin, "
						  "(select usename from pg_user where proowner = usesysid) as usename, "
						  "proiscachable, proisstrict "
						  "from pg_proc "
						  "where pg_proc.oid > '%u'::oid",
						  g_last_builtin_oid);
	}

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain list of functions failed: %s",
				  PQerrorMessage(g_conn));
		exit_nicely();
	}

	ntups = PQntuples(res);

	*numFuncs = ntups;

	finfo = (FuncInfo *) malloc(ntups * sizeof(FuncInfo));

	memset((char *) finfo, 0, ntups * sizeof(FuncInfo));

	i_oid = PQfnumber(res, "oid");
	i_proname = PQfnumber(res, "proname");
	i_prolang = PQfnumber(res, "prolang");
	i_pronargs = PQfnumber(res, "pronargs");
	i_proargtypes = PQfnumber(res, "proargtypes");
	i_prorettype = PQfnumber(res, "prorettype");
	i_proretset = PQfnumber(res, "proretset");
	i_prosrc = PQfnumber(res, "prosrc");
	i_probin = PQfnumber(res, "probin");
	i_iscachable = PQfnumber(res, "proiscachable");
	i_isstrict = PQfnumber(res, "proisstrict");
	i_usename = PQfnumber(res, "usename");

	for (i = 0; i < ntups; i++)
	{
		finfo[i].oid = strdup(PQgetvalue(res, i, i_oid));
		finfo[i].proname = strdup(PQgetvalue(res, i, i_proname));

		finfo[i].prosrc = strdup(PQgetvalue(res, i, i_prosrc));
		finfo[i].probin = strdup(PQgetvalue(res, i, i_probin));

		finfo[i].prorettype = strdup(PQgetvalue(res, i, i_prorettype));
		finfo[i].retset = (strcmp(PQgetvalue(res, i, i_proretset), "t") == 0);
		finfo[i].nargs = atoi(PQgetvalue(res, i, i_pronargs));
		finfo[i].lang = atooid(PQgetvalue(res, i, i_prolang));
		finfo[i].usename = strdup(PQgetvalue(res, i, i_usename));
		finfo[i].iscachable = (strcmp(PQgetvalue(res, i, i_iscachable), "t") == 0);
		finfo[i].isstrict = (strcmp(PQgetvalue(res, i, i_isstrict), "t") == 0);

		if (strlen(finfo[i].usename) == 0)
			write_msg(NULL, "WARNING: owner of function \"%s\" appears to be invalid\n",
					  finfo[i].proname);

		if (finfo[i].nargs < 0 || finfo[i].nargs > FUNC_MAX_ARGS)
		{
			write_msg(NULL, "failed sanity check: function %s has more than %d (namely %d) arguments\n",
					  finfo[i].proname, FUNC_MAX_ARGS, finfo[i].nargs);
			exit_nicely();
		}
		parseNumericArray(PQgetvalue(res, i, i_proargtypes),
						  finfo[i].argtypes,
						  finfo[i].nargs);
		finfo[i].dumped = 0;
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return finfo;
}

/*
 * getTables
 *	  read all the user-defined tables (no indexes, no catalogs)
 * in the system catalogs return them in the TableInfo* structure
 *
 * numTables is set to the number of tables read in
 */
TableInfo *
getTables(int *numTables, FuncInfo *finfo, int numFuncs, const char *tablename)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer delqry = createPQExpBuffer();
	PQExpBuffer lockquery = createPQExpBuffer();
	TableInfo  *tblinfo;

	int			i_reloid;
	int			i_relname;
	int			i_relkind;
	int			i_relacl;
	int			i_usename;
	int			i_relchecks;
	int			i_reltriggers;
	int			i_relhasindex;
	int			i_relhasoids;

	/*
	 * find all the user-defined tables (no indexes and no catalogs),
	 * ordering by oid is important so that we always process the parent
	 * tables before the child tables when traversing the tblinfo*
	 *
	 * we ignore tables that are not type 'r' (ordinary relation) or 'S'
	 * (sequence) or 'v' (view).
	 */

	if (g_fout->remoteVersion >= 70200)
	{
		appendPQExpBuffer(query,
						"SELECT pg_class.oid, relname, relacl, relkind, "
						  "(select usename from pg_user where relowner = usesysid) as usename, "
					   "relchecks, reltriggers, relhasindex, relhasoids "
						  "from pg_class "
						  "where relname !~ '^pg_' "
						  "and relkind in ('%c', '%c', '%c') "
						  "order by oid",
					   RELKIND_RELATION, RELKIND_SEQUENCE, RELKIND_VIEW);
	}
	else if (g_fout->remoteVersion >= 70100)
	{
		/* all tables have oids in 7.1 */
		appendPQExpBuffer(query,
						"SELECT pg_class.oid, relname, relacl, relkind, "
						  "(select usename from pg_user where relowner = usesysid) as usename, "
		  "relchecks, reltriggers, relhasindex, 't'::bool as relhasoids "
						  "from pg_class "
						  "where relname !~ '^pg_' "
						  "and relkind in ('%c', '%c', '%c') "
						  "order by oid",
					   RELKIND_RELATION, RELKIND_SEQUENCE, RELKIND_VIEW);
	}
	else
	{
		/*
		 * Before 7.1, view relkind was not set to 'v', so we must check
		 * if we have a view by looking for a rule in pg_rewrite.
		 */
		appendPQExpBuffer(query,
						  "SELECT c.oid, relname, relacl, "
						  "CASE WHEN relhasrules and relkind = 'r' "
				  "  and EXISTS(SELECT rulename FROM pg_rewrite r WHERE "
				  "             r.ev_class = c.oid AND r.ev_type = '1') "
						  "THEN '%c'::\"char\" "
						  "ELSE relkind END AS relkind,"
						  "(select usename from pg_user where relowner = usesysid) as usename, "
		  "relchecks, reltriggers, relhasindex, 't'::bool as relhasoids "
						  "from pg_class c "
						  "where relname !~ '^pg_' "
						  "and relkind in ('%c', '%c', '%c') "
						  "order by oid",
						  RELKIND_VIEW,
					   RELKIND_RELATION, RELKIND_SEQUENCE, RELKIND_VIEW);
	}

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain list of tables failed: %s",
				  PQerrorMessage(g_conn));
		exit_nicely();
	}

	ntups = PQntuples(res);

	*numTables = ntups;

	/*
	 * First pass: extract data from result and lock tables.  We do the
	 * locking before anything else, to minimize the window wherein a
	 * table could disappear under us.
	 *
	 * Note that we have to collect info about all tables here, even when
	 * dumping only one, because we don't know which tables might be
	 * inheritance ancestors of the target table.  Possible future
	 * improvement: suppress later collection of schema info about tables
	 * that are determined not to be either targets or ancestors of
	 * targets.
	 */
	tblinfo = (TableInfo *) malloc(ntups * sizeof(TableInfo));

	i_reloid = PQfnumber(res, "oid");
	i_relname = PQfnumber(res, "relname");
	i_relacl = PQfnumber(res, "relacl");
	i_relkind = PQfnumber(res, "relkind");
	i_usename = PQfnumber(res, "usename");
	i_relchecks = PQfnumber(res, "relchecks");
	i_reltriggers = PQfnumber(res, "reltriggers");
	i_relhasindex = PQfnumber(res, "relhasindex");
	i_relhasoids = PQfnumber(res, "relhasoids");

	for (i = 0; i < ntups; i++)
	{
		tblinfo[i].oid = strdup(PQgetvalue(res, i, i_reloid));
		tblinfo[i].relname = strdup(PQgetvalue(res, i, i_relname));
		tblinfo[i].relacl = strdup(PQgetvalue(res, i, i_relacl));
		tblinfo[i].relkind = *(PQgetvalue(res, i, i_relkind));
		tblinfo[i].sequence = (tblinfo[i].relkind == RELKIND_SEQUENCE);
		tblinfo[i].hasindex = (strcmp(PQgetvalue(res, i, i_relhasindex), "t") == 0);
		tblinfo[i].hasoids = (strcmp(PQgetvalue(res, i, i_relhasoids), "t") == 0);
		tblinfo[i].usename = strdup(PQgetvalue(res, i, i_usename));
		tblinfo[i].ncheck = atoi(PQgetvalue(res, i, i_relchecks));
		tblinfo[i].ntrig = atoi(PQgetvalue(res, i, i_reltriggers));

		/*
		 * Read-lock target tables to make sure they aren't DROPPED or
		 * altered in schema before we get around to dumping them.
		 *
		 * If no target tablename was specified, lock all tables we see,
		 * otherwise lock only the specified table.  (This is incomplete
		 * because we'll still try to collect schema info about all
		 * tables, and could possibly lose during that phase.  But for the
		 * typical use where we're dumping all tables anyway, it matters
		 * not.)
		 *
		 * NOTE: it'd be kinda nice to lock views and sequences too, not only
		 * plain tables, but the backend doesn't presently allow that.
		 */
		if ((tblinfo[i].relkind == RELKIND_RELATION) &&
		(tablename == NULL || strcmp(tblinfo[i].relname, tablename) == 0))
		{
			PGresult   *lres;

			resetPQExpBuffer(lockquery);
			appendPQExpBuffer(lockquery,
							  "LOCK TABLE %s IN ACCESS SHARE MODE",
							  fmtId(tblinfo[i].relname, force_quotes));
			lres = PQexec(g_conn, lockquery->data);
			if (!lres || PQresultStatus(lres) != PGRES_COMMAND_OK)
			{
				write_msg(NULL, "Attempt to lock table \"%s\" failed.  %s",
						  tblinfo[i].relname, PQerrorMessage(g_conn));
				exit_nicely();
			}
			PQclear(lres);
		}
	}

	PQclear(res);
	res = NULL;

	/*
	 * Second pass: pick up additional information about each table, as
	 * required.
	 */
	for (i = 0; i < *numTables; i++)
	{
		/* Emit notice if join for owner failed */
		if (strlen(tblinfo[i].usename) == 0)
			write_msg(NULL, "WARNING: owner of table \"%s\" appears to be invalid\n",
					  tblinfo[i].relname);

		/* Get definition if it's a view */
		if (tblinfo[i].relkind == RELKIND_VIEW)
		{
			PGresult   *res2;

			resetPQExpBuffer(query);
			appendPQExpBuffer(query, "SELECT definition as viewdef, ");

			/*
			 * XXX 7.2 - replace with att from pg_views or some other
			 * generic source
			 */
			appendPQExpBuffer(query, "(select oid from pg_rewrite where "
					  " rulename=('_RET' || viewname)::name) as view_oid"
							  " from pg_views where viewname = ");
			formatStringLiteral(query, tblinfo[i].relname, CONV_ALL);
			appendPQExpBuffer(query, ";");

			res2 = PQexec(g_conn, query->data);
			if (!res2 || PQresultStatus(res2) != PGRES_TUPLES_OK)
			{
				write_msg(NULL, "query to obtain definition of view \"%s\" failed: %s",
						  tblinfo[i].relname, PQerrorMessage(g_conn));
				exit_nicely();
			}

			if (PQntuples(res2) != 1)
			{
				if (PQntuples(res2) < 1)
					write_msg(NULL, "query to obtain definition of view \"%s\" returned no data\n",
							  tblinfo[i].relname);
				else
					write_msg(NULL, "query to obtain definition of view \"%s\" returned more than one definition\n",
							  tblinfo[i].relname);
				exit_nicely();
			}

			if (PQgetisnull(res2, 0, 1))
			{
				write_msg(NULL, "query to obtain definition of view \"%s\" returned NULL oid\n",
						  tblinfo[i].relname);
				exit_nicely();
			}

			tblinfo[i].viewdef = strdup(PQgetvalue(res2, 0, 0));
			tblinfo[i].viewoid = strdup(PQgetvalue(res2, 0, 1));

			if (strlen(tblinfo[i].viewdef) == 0)
			{
				write_msg(NULL, "definition of view \"%s\" appears to be empty (length zero)\n",
						  tblinfo[i].relname);
				exit_nicely();
			}
			PQclear(res2);
		}
		else
			tblinfo[i].viewdef = NULL;

		/*
		 * Get non-inherited CHECK constraints, if any.
		 *
		 * Exclude inherited CHECKs from CHECK constraints total. If a
		 * constraint matches by name and condition with a constraint
		 * belonging to a parent class (OR conditions match and both names
		 * start with '$', we assume it was inherited.
		 */
		if (tblinfo[i].ncheck > 0)
		{
			PGresult   *res2;
			int			i_rcname,
						i_rcsrc;
			int			ntups2;
			int			i2;

			if (g_verbose)
				write_msg(NULL, "finding CHECK constraints for table %s\n",
						  tblinfo[i].relname);

			resetPQExpBuffer(query);
			appendPQExpBuffer(query, "SELECT rcname, rcsrc from pg_relcheck "
							  " where rcrelid = '%s'::oid "
							  "   and not exists "
				   "  (select * from pg_relcheck as c, pg_inherits as i "
							"    where i.inhrelid = pg_relcheck.rcrelid "
							  "      and (c.rcname = pg_relcheck.rcname "
							  "          or (    c.rcname[0] = '$' "
						 "              and pg_relcheck.rcname[0] = '$')"
							  "          )"
							  "      and c.rcsrc = pg_relcheck.rcsrc "
							  "      and c.rcrelid = i.inhparent) "
							  " order by rcname ",
							  tblinfo[i].oid);
			res2 = PQexec(g_conn, query->data);
			if (!res2 ||
				PQresultStatus(res2) != PGRES_TUPLES_OK)
			{
				write_msg(NULL, "query to obtain check constraints failed: %s", PQerrorMessage(g_conn));
				exit_nicely();
			}
			ntups2 = PQntuples(res2);
			if (ntups2 > tblinfo[i].ncheck)
			{
				write_msg(NULL, "expected %d check constraints on table \"%s\" but found %d\n",
						  tblinfo[i].ncheck, tblinfo[i].relname, ntups2);
				write_msg(NULL, "(The system catalogs might be corrupted.)\n");
				exit_nicely();
			}

			/*
			 * Set ncheck to the number of *non-inherited* CHECK
			 * constraints
			 */
			tblinfo[i].ncheck = ntups2;

			i_rcname = PQfnumber(res2, "rcname");
			i_rcsrc = PQfnumber(res2, "rcsrc");
			tblinfo[i].check_expr = (char **) malloc(ntups2 * sizeof(char *));
			for (i2 = 0; i2 < ntups2; i2++)
			{
				const char *name = PQgetvalue(res2, i2, i_rcname);
				const char *expr = PQgetvalue(res2, i2, i_rcsrc);

				resetPQExpBuffer(query);
				if (name[0] != '$')
				{
					appendPQExpBuffer(query, "CONSTRAINT %s ",
									  fmtId(name, force_quotes));
				}
				appendPQExpBuffer(query, "CHECK (%s)", expr);
				tblinfo[i].check_expr[i2] = strdup(query->data);
			}
			PQclear(res2);
		}
		else
			tblinfo[i].check_expr = NULL;

		/* Get primary key */
		if (tblinfo[i].hasindex)
		{
			PGresult   *res2;

			resetPQExpBuffer(query);
			appendPQExpBuffer(query,
							  "SELECT indexrelid FROM pg_index i WHERE i.indisprimary AND i.indrelid = '%s'::oid ",
							  tblinfo[i].oid);
			res2 = PQexec(g_conn, query->data);
			if (!res2 || PQresultStatus(res2) != PGRES_TUPLES_OK)
			{
				write_msg(NULL, "query to obtain primary key of table \"%s\" failed: %s",
						  tblinfo[i].relname, PQerrorMessage(g_conn));
				exit_nicely();
			}

			if (PQntuples(res2) > 1)
			{
				write_msg(NULL, "query to obtain primary key of table \"%s\" produced more than one result\n",
						  tblinfo[i].relname);
				exit_nicely();
			}

			if (PQntuples(res2) == 1)
				tblinfo[i].pkIndexOid = strdup(PQgetvalue(res2, 0, 0));
			else
				tblinfo[i].pkIndexOid = NULL;

			PQclear(res2);
		}
		else
			tblinfo[i].pkIndexOid = NULL;

		/* Get primary key name (if primary key exist) */
		if (tblinfo[i].pkIndexOid != NULL)
		{
			PGresult   *res2;
			int			n;

			resetPQExpBuffer(query);
			appendPQExpBuffer(query,
							  "SELECT relname FROM pg_class "
							  "WHERE oid = '%s'::oid",
							  tblinfo[i].pkIndexOid);

			res2 = PQexec(g_conn, query->data);
			if (!res2 || PQresultStatus(res2) != PGRES_TUPLES_OK)
			{
				write_msg(NULL, "query to obtain name of primary key of table \"%s\" failed: %s",
						  tblinfo[i].relname, PQerrorMessage(g_conn));
				exit_nicely();
			}

			n = PQntuples(res2);
			if (n != 1)
			{
				write_msg(NULL, "query to obtain name of primary key of table \"%s\" did not return exactly one result\n",
						  tblinfo[i].relname);
				exit_nicely();
			}

			tblinfo[i].primary_key_name =
				strdup(fmtId(PQgetvalue(res2, 0, 0), force_quotes));
			if (tblinfo[i].primary_key_name == NULL)
			{
				write_msg(NULL, "out of memory\n");
				exit_nicely();
			}
			PQclear(res2);
		}
		else
			tblinfo[i].primary_key_name = NULL;

		/* Get Triggers */
		if (tblinfo[i].ntrig > 0)
		{
			PGresult   *res2;
			int			i_tgoid,
						i_tgname,
						i_tgfoid,
						i_tgtype,
						i_tgnargs,
						i_tgargs,
						i_tgisconstraint,
						i_tgconstrname,
						i_tgdeferrable,
						i_tgconstrrelid,
						i_tgconstrrelname,
						i_tginitdeferred;
			int			ntups2;
			int			i2;

			if (g_verbose)
				write_msg(NULL, "finding triggers for table %s\n", tblinfo[i].relname);

			resetPQExpBuffer(query);
			appendPQExpBuffer(query,
					   "SELECT tgname, tgfoid, tgtype, tgnargs, tgargs, "
						   "tgisconstraint, tgconstrname, tgdeferrable, "
							  "tgconstrrelid, tginitdeferred, oid, "
			  "(select relname from pg_class where oid = tgconstrrelid) "
							  "		as tgconstrrelname "
							  "from pg_trigger "
							  "where tgrelid = '%s'::oid ",
							  tblinfo[i].oid);
			res2 = PQexec(g_conn, query->data);
			if (!res2 ||
				PQresultStatus(res2) != PGRES_TUPLES_OK)
			{
				write_msg(NULL, "query to obtain list of triggers failed: %s", PQerrorMessage(g_conn));
				exit_nicely();
			}
			ntups2 = PQntuples(res2);
			if (ntups2 != tblinfo[i].ntrig)
			{
				write_msg(NULL, "expected %d triggers on table \"%s\" but found %d\n",
						  tblinfo[i].ntrig, tblinfo[i].relname, ntups2);
				exit_nicely();
			}
			i_tgname = PQfnumber(res2, "tgname");
			i_tgfoid = PQfnumber(res2, "tgfoid");
			i_tgtype = PQfnumber(res2, "tgtype");
			i_tgnargs = PQfnumber(res2, "tgnargs");
			i_tgargs = PQfnumber(res2, "tgargs");
			i_tgoid = PQfnumber(res2, "oid");
			i_tgisconstraint = PQfnumber(res2, "tgisconstraint");
			i_tgconstrname = PQfnumber(res2, "tgconstrname");
			i_tgdeferrable = PQfnumber(res2, "tgdeferrable");
			i_tgconstrrelid = PQfnumber(res2, "tgconstrrelid");
			i_tgconstrrelname = PQfnumber(res2, "tgconstrrelname");
			i_tginitdeferred = PQfnumber(res2, "tginitdeferred");

			tblinfo[i].triggers = (TrigInfo *) malloc(ntups2 * sizeof(TrigInfo));
			resetPQExpBuffer(query);
			for (i2 = 0; i2 < ntups2; i2++)
			{
				const char *tgfuncoid = PQgetvalue(res2, i2, i_tgfoid);
				char	   *tgfunc = NULL;
				int2		tgtype = atoi(PQgetvalue(res2, i2, i_tgtype));
				int			tgnargs = atoi(PQgetvalue(res2, i2, i_tgnargs));
				const char *tgargs = PQgetvalue(res2, i2, i_tgargs);
				int			tgisconstraint;
				int			tgdeferrable;
				int			tginitdeferred;
				char	   *tgconstrrelid;
				char	   *tgname;
				const char *p;
				int			findx;

				tgname = PQgetvalue(res2, i2, i_tgname);

				if (strcmp(PQgetvalue(res2, i2, i_tgisconstraint), "f") == 0)
					tgisconstraint = 0;
				else
					tgisconstraint = 1;

				if (strcmp(PQgetvalue(res2, i2, i_tgdeferrable), "f") == 0)
					tgdeferrable = 0;
				else
					tgdeferrable = 1;

				if (strcmp(PQgetvalue(res2, i2, i_tginitdeferred), "f") == 0)
					tginitdeferred = 0;
				else
					tginitdeferred = 1;

				for (findx = 0; findx < numFuncs; findx++)
				{
					if (strcmp(finfo[findx].oid, tgfuncoid) == 0 &&
						finfo[findx].nargs == 0 &&
						strcmp(finfo[findx].prorettype, "0") == 0)
						break;
				}

				if (findx == numFuncs)
				{
					PGresult   *r;
					int			numFuncs;

					/*
					 * the funcname is an oid which we use to find the
					 * name of the pg_proc.  We need to do this because
					 * getFuncs() only reads in the user-defined funcs not
					 * all the funcs.  We might not find what we want by
					 * looking in FuncInfo*
					 */
					resetPQExpBuffer(query);
					appendPQExpBuffer(query,
									  "SELECT proname from pg_proc "
									  "where pg_proc.oid = '%s'::oid",
									  tgfuncoid);

					r = PQexec(g_conn, query->data);
					if (!r || PQresultStatus(r) != PGRES_TUPLES_OK)
					{
						write_msg(NULL, "query to obtain procedure name for trigger \"%s\" failed: %s",
								  tgname, PQerrorMessage(g_conn));
						exit_nicely();
					}

					/* Sanity: Check we got only one tuple */
					numFuncs = PQntuples(r);
					if (numFuncs != 1)
					{
						write_msg(NULL, "query to obtain procedure name for trigger \"%s\" did not return exactly one result\n",
								  tgname);
						exit_nicely();
					}

					tgfunc = strdup(PQgetvalue(r, 0, PQfnumber(r, "proname")));
					PQclear(r);
				}
				else
					tgfunc = strdup(finfo[findx].proname);

				resetPQExpBuffer(delqry);
				appendPQExpBuffer(delqry, "DROP TRIGGER %s ", fmtId(tgname, force_quotes));
				appendPQExpBuffer(delqry, "ON %s;\n",
								fmtId(tblinfo[i].relname, force_quotes));

				resetPQExpBuffer(query);
				if (tgisconstraint)
				{
					appendPQExpBuffer(query, "CREATE CONSTRAINT TRIGGER ");
					appendPQExpBuffer(query, fmtId(PQgetvalue(res2, i2, i_tgconstrname), force_quotes));
				}
				else
				{
					appendPQExpBuffer(query, "CREATE TRIGGER ");
					appendPQExpBuffer(query, fmtId(tgname, force_quotes));
				}
				appendPQExpBufferChar(query, ' ');
				/* Trigger type */
				findx = 0;
				if (TRIGGER_FOR_BEFORE(tgtype))
					appendPQExpBuffer(query, "BEFORE");
				else
					appendPQExpBuffer(query, "AFTER");
				if (TRIGGER_FOR_INSERT(tgtype))
				{
					appendPQExpBuffer(query, " INSERT");
					findx++;
				}
				if (TRIGGER_FOR_DELETE(tgtype))
				{
					if (findx > 0)
						appendPQExpBuffer(query, " OR DELETE");
					else
						appendPQExpBuffer(query, " DELETE");
					findx++;
				}
				if (TRIGGER_FOR_UPDATE(tgtype))
				{
					if (findx > 0)
						appendPQExpBuffer(query, " OR UPDATE");
					else
						appendPQExpBuffer(query, " UPDATE");
				}
				appendPQExpBuffer(query, " ON %s ", fmtId(tblinfo[i].relname, force_quotes));

				if (tgisconstraint)
				{
					tgconstrrelid = PQgetvalue(res2, i2, i_tgconstrrelid);

					if (strcmp(tgconstrrelid, "0") != 0)
					{

						if (PQgetisnull(res2, i2, i_tgconstrrelname))
						{
							write_msg(NULL, "query produced NULL referenced table name for foreign key trigger \"%s\" on table \"%s\" (oid of table: %s)\n",
							  tgname, tblinfo[i].relname, tgconstrrelid);
							exit_nicely();
						}

						appendPQExpBuffer(query, " FROM %s",
										  fmtId(PQgetvalue(res2, i2, i_tgconstrrelname), force_quotes));
					}
					if (!tgdeferrable)
						appendPQExpBuffer(query, " NOT");
					appendPQExpBuffer(query, " DEFERRABLE INITIALLY ");
					if (tginitdeferred)
						appendPQExpBuffer(query, "DEFERRED");
					else
						appendPQExpBuffer(query, "IMMEDIATE");

				}

				appendPQExpBuffer(query, " FOR EACH ROW");
				appendPQExpBuffer(query, " EXECUTE PROCEDURE %s (",
								  fmtId(tgfunc, force_quotes));
				for (findx = 0; findx < tgnargs; findx++)
				{
					const char *s;

					for (p = tgargs;;)
					{
						p = strchr(p, '\\');
						if (p == NULL)
						{
							write_msg(NULL, "bad argument string (%s) for trigger \"%s\" on table \"%s\"\n",
									  PQgetvalue(res2, i2, i_tgargs),
									  tgname,
									  tblinfo[i].relname);
							exit_nicely();
						}
						p++;
						if (*p == '\\')
						{
							p++;
							continue;
						}
						if (p[0] == '0' && p[1] == '0' && p[2] == '0')
							break;
					}
					p--;
					appendPQExpBufferChar(query, '\'');
					for (s = tgargs; s < p;)
					{
						if (*s == '\'')
							appendPQExpBufferChar(query, '\\');
						appendPQExpBufferChar(query, *s++);
					}
					appendPQExpBufferChar(query, '\'');
					appendPQExpBuffer(query, (findx < tgnargs - 1) ? ", " : "");
					tgargs = p + 4;
				}
				appendPQExpBuffer(query, ");\n");

				tblinfo[i].triggers[i2].tgsrc = strdup(query->data);

				/*** Initialize trcomments and troids ***/

				resetPQExpBuffer(query);
				appendPQExpBuffer(query, "TRIGGER %s ",
								  fmtId(tgname, force_quotes));
				appendPQExpBuffer(query, "ON %s",
								fmtId(tblinfo[i].relname, force_quotes));
				tblinfo[i].triggers[i2].tgcomment = strdup(query->data);
				tblinfo[i].triggers[i2].oid = strdup(PQgetvalue(res2, i2, i_tgoid));
				tblinfo[i].triggers[i2].tgname = strdup(fmtId(tgname, false));
				tblinfo[i].triggers[i2].tgdel = strdup(delqry->data);

				if (tgfunc)
					free(tgfunc);
			}
			PQclear(res2);
		}
		else
			tblinfo[i].triggers = NULL;

	}

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(delqry);
	destroyPQExpBuffer(lockquery);

	return tblinfo;
}

/*
 * getInherits
 *	  read all the inheritance information
 * from the system catalogs return them in the InhInfo* structure
 *
 * numInherits is set to the number of tables read in
 */
InhInfo *
getInherits(int *numInherits)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	InhInfo    *inhinfo;

	int			i_inhrelid;
	int			i_inhparent;

	/* find all the inheritance information */

	appendPQExpBuffer(query, "SELECT inhrelid, inhparent from pg_inherits");

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain inheritance relationships failed: %s",
				  PQerrorMessage(g_conn));
		exit_nicely();
	}

	ntups = PQntuples(res);

	*numInherits = ntups;

	inhinfo = (InhInfo *) malloc(ntups * sizeof(InhInfo));

	i_inhrelid = PQfnumber(res, "inhrelid");
	i_inhparent = PQfnumber(res, "inhparent");

	for (i = 0; i < ntups; i++)
	{
		inhinfo[i].inhrelid = strdup(PQgetvalue(res, i, i_inhrelid));
		inhinfo[i].inhparent = strdup(PQgetvalue(res, i, i_inhparent));
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return inhinfo;
}

/*
 * getTableAttrs -
 *	  for each table in tblinfo, read its attributes types and names
 *
 * this is implemented in a very inefficient way right now, looping
 * through the tblinfo and doing a join per table to find the attrs and their
 * types
 *
 *	modifies tblinfo
 */
void
getTableAttrs(TableInfo *tblinfo, int numTables)
{
	int			i,
				j;
	PQExpBuffer q = createPQExpBuffer();
	int			i_attname;
	int			i_typname;
	int			i_atttypmod;
	int			i_attnotnull;
	int			i_atthasdef;
	int			i_atttypedefn;
	PGresult   *res;
	int			ntups;

	for (i = 0; i < numTables; i++)
	{
		if (tblinfo[i].sequence)
			continue;

		/* find all the user attributes and their types */
		/* we must read the attribute names in attribute number order! */

		/*
		 * because we will use the attnum to index into the attnames array
		 * later
		 */
		if (g_verbose)
			write_msg(NULL, "finding the columns and types for table %s\n",
					  tblinfo[i].relname);

		resetPQExpBuffer(q);

		if (g_fout->remoteVersion < 70100)
		{
			/* Fake the LOJ below */
			appendPQExpBuffer(q,
				 "  SELECT a.attnum, a.attname, t.typname, a.atttypmod, "
				"        a.attnotnull, a.atthasdef, NULL as atttypedefn "
							  "    from pg_attribute a, pg_type t "
							  "    where a.attrelid = '%s'::oid "
							  "        and a.attnum > 0 "
							  "        and a.atttypid = t.oid "
							  " UNION ALL SELECT a.attnum, a.attname, NULL as typname, a.atttypmod, "
				"        a.attnotnull, a.atthasdef, NULL as atttypedefn "
							  "    from pg_attribute a "
							  "    where a.attrelid = '%s'::oid "
							  "        and a.attnum > 0 "
							  "        and Not Exists(Select * From pg_type t where a.atttypid = t.oid)"
							  "    order by attnum",
							  tblinfo[i].oid, tblinfo[i].oid);

		}
		else
		{
			appendPQExpBuffer(q, "SELECT a.attnum, a.attname, t.typname, a.atttypmod, "
							  "a.attnotnull, a.atthasdef, format_type(a.atttypid, a.atttypmod) as atttypedefn "
							  "from pg_attribute a LEFT OUTER JOIN pg_type t ON a.atttypid = t.oid "
							  "where a.attrelid = '%s'::oid "
							  "and a.attnum > 0 order by attnum",
							  tblinfo[i].oid);
		}

		res = PQexec(g_conn, q->data);
		if (!res ||
			PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			write_msg(NULL, "query to get table columns failed: %s", PQerrorMessage(g_conn));
			exit_nicely();
		}

		ntups = PQntuples(res);

		i_attname = PQfnumber(res, "attname");
		i_typname = PQfnumber(res, "typname");
		i_atttypmod = PQfnumber(res, "atttypmod");
		i_attnotnull = PQfnumber(res, "attnotnull");
		i_atthasdef = PQfnumber(res, "atthasdef");
		i_atttypedefn = PQfnumber(res, "atttypedefn");

		tblinfo[i].numatts = ntups;
		tblinfo[i].attnames = (char **) malloc(ntups * sizeof(char *));
		tblinfo[i].atttypedefns = (char **) malloc(ntups * sizeof(char *));
		tblinfo[i].typnames = (char **) malloc(ntups * sizeof(char *));
		tblinfo[i].atttypmod = (int *) malloc(ntups * sizeof(int));
		tblinfo[i].inhAttrs = (int *) malloc(ntups * sizeof(int));
		tblinfo[i].inhAttrDef = (int *) malloc(ntups * sizeof(int));
		tblinfo[i].inhNotNull = (int *) malloc(ntups * sizeof(int));
		tblinfo[i].notnull = (bool *) malloc(ntups * sizeof(bool));
		tblinfo[i].adef_expr = (char **) malloc(ntups * sizeof(char *));
		tblinfo[i].parentRels = NULL;
		tblinfo[i].numParents = 0;
		for (j = 0; j < ntups; j++)
		{
			/* Sanity check on LOJ */
			if (PQgetisnull(res, j, i_typname))
			{
				write_msg(NULL, "query produced NULL name for data type of column %d of table %s\n",
						  j + 1, tblinfo[i].relname);
				exit_nicely();
			}

			tblinfo[i].attnames[j] = strdup(PQgetvalue(res, j, i_attname));
			tblinfo[i].atttypedefns[j] = strdup(PQgetvalue(res, j, i_atttypedefn));
			tblinfo[i].typnames[j] = strdup(PQgetvalue(res, j, i_typname));
			tblinfo[i].atttypmod[j] = atoi(PQgetvalue(res, j, i_atttypmod));
			tblinfo[i].inhAttrs[j] = 0; /* this flag is set in
										 * flagInhAttrs() */
			tblinfo[i].inhAttrDef[j] = 0;
			tblinfo[i].inhNotNull[j] = 0;

			tblinfo[i].notnull[j] = (PQgetvalue(res, j, i_attnotnull)[0] == 't') ? true : false;
			if (PQgetvalue(res, j, i_atthasdef)[0] == 't')
			{
				PGresult   *res2;
				int			numAttr;

				if (g_verbose)
					write_msg(NULL, "finding DEFAULT expression for column %s\n",
							  tblinfo[i].attnames[j]);

				resetPQExpBuffer(q);
				appendPQExpBuffer(q, "SELECT adsrc from pg_attrdef "
							 "where adrelid = '%s'::oid and adnum = %d ",
								  tblinfo[i].oid, j + 1);
				res2 = PQexec(g_conn, q->data);
				if (!res2 ||
					PQresultStatus(res2) != PGRES_TUPLES_OK)
				{
					write_msg(NULL, "query to get column default value failed: %s",
							  PQerrorMessage(g_conn));
					exit_nicely();
				}

				/* Sanity: Check we got only one tuple */
				numAttr = PQntuples(res2);
				if (numAttr != 1)
				{
					write_msg(NULL, "query to get default value for column \"%s\" returned %d rows; expected 1\n",
							  tblinfo[i].attnames[j], numAttr);
					exit_nicely();
				}

				tblinfo[i].adef_expr[j] = strdup(PQgetvalue(res2, 0, PQfnumber(res2, "adsrc")));
				PQclear(res2);
			}
			else
				tblinfo[i].adef_expr[j] = NULL;
		}
		PQclear(res);
	}

	destroyPQExpBuffer(q);
}


/*
 * getIndexes
 *	  read all the user-defined indexes information
 * from the system catalogs return them in the InhInfo* structure
 *
 * numIndexes is set to the number of indexes read in
 *
 *
 */
IndInfo *
getIndexes(int *numIndexes)
{
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	int			ntups;
	IndInfo    *indinfo;

	int			i_indexreloid;
	int			i_indreloid;
	int			i_indexrelname;
	int			i_indrelname;
	int			i_indexdef;
	int			i_indisprimary;
	int			i_indkey;

	/*
	 * find all the user-defined indexes.
	 *
	 * Notice we skip indexes on system classes
	 *
	 * XXXX: Use LOJ
	 */

	appendPQExpBuffer(query,
					  "SELECT i.indexrelid as indexreloid, "
					  "i.indrelid as indreloid, "
				 "t1.relname as indexrelname, t2.relname as indrelname, "
					  "pg_get_indexdef(i.indexrelid) as indexdef, "
					  "i.indisprimary, i.indkey "
					  "from pg_index i, pg_class t1, pg_class t2 "
				   "WHERE t1.oid = i.indexrelid and t2.oid = i.indrelid "
					  "and i.indexrelid > '%u'::oid "
					  "and t2.relname !~ '^pg_' ",
					  g_last_builtin_oid);

	if (g_fout->remoteVersion < 70100)
		appendPQExpBuffer(query, " and t2.relkind != 'l'");

	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain list of indexes failed: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}

	ntups = PQntuples(res);

	*numIndexes = ntups;

	indinfo = (IndInfo *) malloc(ntups * sizeof(IndInfo));

	memset((char *) indinfo, 0, ntups * sizeof(IndInfo));

	i_indexreloid = PQfnumber(res, "indexreloid");
	i_indreloid = PQfnumber(res, "indreloid");
	i_indexrelname = PQfnumber(res, "indexrelname");
	i_indrelname = PQfnumber(res, "indrelname");
	i_indexdef = PQfnumber(res, "indexdef");
	i_indisprimary = PQfnumber(res, "indisprimary");
	i_indkey = PQfnumber(res, "indkey");

	for (i = 0; i < ntups; i++)
	{
		indinfo[i].indexreloid = strdup(PQgetvalue(res, i, i_indexreloid));
		indinfo[i].indreloid = strdup(PQgetvalue(res, i, i_indreloid));
		indinfo[i].indexrelname = strdup(PQgetvalue(res, i, i_indexrelname));
		indinfo[i].indrelname = strdup(PQgetvalue(res, i, i_indrelname));
		indinfo[i].indexdef = strdup(PQgetvalue(res, i, i_indexdef));
		indinfo[i].indisprimary = strdup(PQgetvalue(res, i, i_indisprimary));
		parseNumericArray(PQgetvalue(res, i, i_indkey),
						  indinfo[i].indkey,
						  INDEX_MAX_KEYS);
	}
	PQclear(res);

	destroyPQExpBuffer(query);

	return indinfo;
}

/*------------------------------------------------------------------
 * dumpComment --
 *
 * This routine is used to dump any comments associated with the
 * oid handed to this routine. The routine takes a constant character
 * string for the target part of the comment-creation command, plus
 * OID, class name, and subid which are the primary key for pg_description.
 * If a matching pg_description entry is found, it is dumped.
 * Additional dependencies can be passed for the comment, too --- this is
 * needed for VIEWs, whose comments are filed under the table OID but
 * which are dumped in order by their rule OID.
 *------------------------------------------------------------------
*/

static void
dumpComment(Archive *fout, const char *target, const char *oid,
			const char *classname, int subid,
			const char *((*deps)[]))
{
	PGresult   *res;
	PQExpBuffer query;
	int			i_description;

	/* Comments are SCHEMA not data */
	if (dataOnly)
		return;

	/*** Build query to find comment ***/

	query = createPQExpBuffer();

	if (fout->remoteVersion >= 70200)
	{
		appendPQExpBuffer(query, "SELECT description FROM pg_description "
						  "WHERE objoid = '%s'::oid and classoid = "
					   "(SELECT oid FROM pg_class where relname = '%s') "
						  "and objsubid = %d",
						  oid, classname, subid);
	}
	else
	{
		/* Note: this will fail to find attribute comments in pre-7.2... */
		appendPQExpBuffer(query, "SELECT description FROM pg_description WHERE objoid = '%s'::oid", oid);
	}

	/*** Execute query ***/

	res = PQexec(g_conn, query->data);
	if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to get comment on oid %s failed: %s",
				  oid, PQerrorMessage(g_conn));
		exit_nicely();
	}

	/*** If a comment exists, build COMMENT ON statement ***/

	if (PQntuples(res) == 1)
	{
		i_description = PQfnumber(res, "description");
		resetPQExpBuffer(query);
		appendPQExpBuffer(query, "COMMENT ON %s IS ", target);
		formatStringLiteral(query, PQgetvalue(res, 0, i_description),
							PASS_LFTAB);
		appendPQExpBuffer(query, ";\n");

		ArchiveEntry(fout, oid, target, "COMMENT", deps,
					 query->data, "" /* Del */ ,
					 "" /* Copy */ , "" /* Owner */ , NULL, NULL);
	}

	/*** Clear the statement buffer and return ***/

	PQclear(res);
	destroyPQExpBuffer(query);
}

/*------------------------------------------------------------------
 * dumpDBComment --
 *
 * This routine is used to dump any comments associated with the
 * database to which we are currently connected. If the user chose
 * to dump the schema of the database, then this is the first
 * statement issued.
 *------------------------------------------------------------------
*/

void
dumpDBComment(Archive *fout)
{
	PGresult   *res;
	PQExpBuffer query;
	int			i_oid;

	/*** Build query to find comment ***/

	query = createPQExpBuffer();
	appendPQExpBuffer(query, "SELECT oid FROM pg_database WHERE datname = ");
	formatStringLiteral(query, PQdb(g_conn), CONV_ALL);

	/*** Execute query ***/

	res = PQexec(g_conn, query->data);
	if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to get database oid failed: %s",
				  PQerrorMessage(g_conn));
		exit_nicely();
	}

	/*** If a comment exists, build COMMENT ON statement ***/

	if (PQntuples(res) != 0)
	{
		i_oid = PQfnumber(res, "oid");
		resetPQExpBuffer(query);
		appendPQExpBuffer(query, "DATABASE %s", fmtId(PQdb(g_conn), force_quotes));
		dumpComment(fout, query->data, PQgetvalue(res, 0, i_oid),
					"pg_database", 0, NULL);
	}

	/*** Clear the statement buffer and return ***/

	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * dumpTypes
 *	  writes out to fout the queries to recreate all the user-defined types
 *
 */
void
dumpTypes(Archive *fout, FuncInfo *finfo, int numFuncs,
		  TypeInfo *tinfo, int numTypes)
{
	int			i;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	int			funcInd;
	const char *((*deps)[]);
	int			depIdx;

	for (i = 0; i < numTypes; i++)
	{
		/* skip all the builtin types */
		if (atooid(tinfo[i].oid) <= g_last_builtin_oid)
			continue;

		/* skip relation types */
		if (atooid(tinfo[i].typrelid) != 0)
			continue;

		/* skip undefined placeholder types */
		if (!tinfo[i].isDefined)
			continue;

		/* skip all array types that start w/ underscore */
		if ((tinfo[i].typname[0] == '_') &&
			(strcmp(tinfo[i].typinput, "array_in") == 0))
			continue;

		deps = malloc(sizeof(char *) * 10);
		depIdx = 0;

		/*
		 * before we create a type, we need to create the input and output
		 * functions for it, if they haven't been created already
		 */
		funcInd = findFuncByName(finfo, numFuncs, tinfo[i].typinput);
		if (funcInd != -1)
		{
			(*deps)[depIdx++] = strdup(finfo[funcInd].oid);
			dumpOneFunc(fout, finfo, funcInd, tinfo, numTypes);
		}

		funcInd = findFuncByName(finfo, numFuncs, tinfo[i].typoutput);
		if (funcInd != -1)
		{
			(*deps)[depIdx++] = strdup(finfo[funcInd].oid);
			dumpOneFunc(fout, finfo, funcInd, tinfo, numTypes);
		}

		resetPQExpBuffer(delq);
		appendPQExpBuffer(delq, "DROP TYPE %s;\n", fmtId(tinfo[i].typname, force_quotes));

		resetPQExpBuffer(q);
		appendPQExpBuffer(q,
						  "CREATE TYPE %s "
						  "( internallength = %s, externallength = %s,",
						  fmtId(tinfo[i].typname, force_quotes),
						  (strcmp(tinfo[i].typlen, "-1") == 0) ?
						  "variable" : tinfo[i].typlen,
						  (strcmp(tinfo[i].typprtlen, "-1") == 0) ?
						  "variable" : tinfo[i].typprtlen);
		/* cannot combine these because fmtId uses static result area */
		appendPQExpBuffer(q, " input = %s,",
						  fmtId(tinfo[i].typinput, force_quotes));
		appendPQExpBuffer(q, " output = %s,",
						  fmtId(tinfo[i].typoutput, force_quotes));
		appendPQExpBuffer(q, " send = %s,",
						  fmtId(tinfo[i].typsend, force_quotes));
		appendPQExpBuffer(q, " receive = %s",
						  fmtId(tinfo[i].typreceive, force_quotes));

		if (tinfo[i].typdefault != NULL)
		{
			appendPQExpBuffer(q, ", default = ");
			formatStringLiteral(q, tinfo[i].typdefault, CONV_ALL);
		}

		if (tinfo[i].isArray)
		{
			char	   *elemType;

			elemType = findTypeByOid(tinfo, numTypes, tinfo[i].typelem, zeroAsOpaque);
			if (elemType == NULL)
			{
				write_msg(NULL, "notice: array type %s - type for elements (oid %s) is not dumped\n",
						  tinfo[i].typname, tinfo[i].typelem);
				continue;
			}

			appendPQExpBuffer(q, ", element = %s, delimiter = ", elemType);
			formatStringLiteral(q, tinfo[i].typdelim, CONV_ALL);

			(*deps)[depIdx++] = strdup(tinfo[i].typelem);
		}

		if (strcmp(tinfo[i].typalign, "c") == 0)
			appendPQExpBuffer(q, ", alignment = char");
		else if (strcmp(tinfo[i].typalign, "s") == 0)
			appendPQExpBuffer(q, ", alignment = int2");
		else if (strcmp(tinfo[i].typalign, "i") == 0)
			appendPQExpBuffer(q, ", alignment = int4");
		else if (strcmp(tinfo[i].typalign, "d") == 0)
			appendPQExpBuffer(q, ", alignment = double");

		if (strcmp(tinfo[i].typstorage, "p") == 0)
			appendPQExpBuffer(q, ", storage = plain");
		else if (strcmp(tinfo[i].typstorage, "e") == 0)
			appendPQExpBuffer(q, ", storage = external");
		else if (strcmp(tinfo[i].typstorage, "x") == 0)
			appendPQExpBuffer(q, ", storage = extended");
		else if (strcmp(tinfo[i].typstorage, "m") == 0)
			appendPQExpBuffer(q, ", storage = main");

		if (tinfo[i].passedbyvalue)
			appendPQExpBuffer(q, ", passedbyvalue);\n");
		else
			appendPQExpBuffer(q, ");\n");

		(*deps)[depIdx++] = NULL;		/* End of List */

		ArchiveEntry(fout, tinfo[i].oid, tinfo[i].typname, "TYPE", deps,
				  q->data, delq->data, "", tinfo[i].usename, NULL, NULL);

		/*** Dump Type Comments ***/

		resetPQExpBuffer(q);

		appendPQExpBuffer(q, "TYPE %s", fmtId(tinfo[i].typname, force_quotes));
		dumpComment(fout, q->data, tinfo[i].oid, "pg_type", 0, NULL);
	}

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
}

/*
 * dumpProcLangs
 *		  writes out to fout the queries to recreate user-defined procedural languages
 *
 */
void
dumpProcLangs(Archive *fout, FuncInfo *finfo, int numFuncs,
			  TypeInfo *tinfo, int numTypes)
{
	PGresult   *res;
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer defqry = createPQExpBuffer();
	PQExpBuffer delqry = createPQExpBuffer();
	int			ntups;
	int			i_oid;
	int			i_lanname;
	int			i_lanpltrusted;
	int			i_lanplcallfoid;
	int			i_lancompiler;
	Oid			lanoid;
	char	   *lanname;
	char	   *lancompiler;
	const char *lanplcallfoid;
	int			i,
				fidx;

	appendPQExpBuffer(query, "SELECT oid, * FROM pg_language "
					  "WHERE lanispl "
					  "ORDER BY oid");
	res = PQexec(g_conn, query->data);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to obtain list of procedural languages failed: %s",
				  PQerrorMessage(g_conn));
		exit_nicely();
	}
	ntups = PQntuples(res);

	i_lanname = PQfnumber(res, "lanname");
	i_lanpltrusted = PQfnumber(res, "lanpltrusted");
	i_lanplcallfoid = PQfnumber(res, "lanplcallfoid");
	i_lancompiler = PQfnumber(res, "lancompiler");
	i_oid = PQfnumber(res, "oid");

	for (i = 0; i < ntups; i++)
	{
		lanoid = atooid(PQgetvalue(res, i, i_oid));
		if (lanoid <= g_last_builtin_oid)
			continue;

		lanplcallfoid = PQgetvalue(res, i, i_lanplcallfoid);


		for (fidx = 0; fidx < numFuncs; fidx++)
		{
			if (!strcmp(finfo[fidx].oid, lanplcallfoid))
				break;
		}
		if (fidx >= numFuncs)
		{
			write_msg(NULL, "handler procedure for procedural language %s not found\n",
					  PQgetvalue(res, i, i_lanname));
			exit_nicely();
		}

		dumpOneFunc(fout, finfo, fidx, tinfo, numTypes);

		lanname = PQgetvalue(res, i, i_lanname);
		lancompiler = PQgetvalue(res, i, i_lancompiler);

		appendPQExpBuffer(delqry, "DROP PROCEDURAL LANGUAGE ");
		formatStringLiteral(delqry, lanname, CONV_ALL);
		appendPQExpBuffer(delqry, ";\n");

		appendPQExpBuffer(defqry, "CREATE %sPROCEDURAL LANGUAGE ",
						  (PQgetvalue(res, i, i_lanpltrusted)[0] == 't') ?
						  "TRUSTED " : "");
		formatStringLiteral(defqry, lanname, CONV_ALL);
		appendPQExpBuffer(defqry, " HANDLER %s LANCOMPILER ",
						  fmtId(finfo[fidx].proname, force_quotes));
		formatStringLiteral(defqry, lancompiler, CONV_ALL);
		appendPQExpBuffer(defqry, ";\n");

		ArchiveEntry(fout, PQgetvalue(res, i, i_oid), lanname, "PROCEDURAL LANGUAGE",
				   NULL, defqry->data, delqry->data, "", "", NULL, NULL);

		resetPQExpBuffer(defqry);
		resetPQExpBuffer(delqry);
	}

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(defqry);
	destroyPQExpBuffer(delqry);
}

/*
 * dumpFuncs
 *	  writes out to fout the queries to recreate all the user-defined functions
 *
 */
void
dumpFuncs(Archive *fout, FuncInfo *finfo, int numFuncs,
		  TypeInfo *tinfo, int numTypes)
{
	int			i;

	for (i = 0; i < numFuncs; i++)
		dumpOneFunc(fout, finfo, i, tinfo, numTypes);
}

/*
 * dumpOneFunc:
 *	  dump out only one function,  the index of which is given in the third
 *	argument
 *
 */

static void
dumpOneFunc(Archive *fout, FuncInfo *finfo, int i,
			TypeInfo *tinfo, int numTypes)
{
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer fn = createPQExpBuffer();
	PQExpBuffer delqry = createPQExpBuffer();
	PQExpBuffer fnlist = createPQExpBuffer();
	PQExpBuffer asPart = createPQExpBuffer();
	char		func_lang[NAMEDATALEN + 1];
	PGresult   *res;
	int			nlangs;
	int			j;
	int			i_lanname;
	char		query[256];

	char	   *listSep;
	char	   *listSepComma = ",";
	char	   *listSepNone = "";
	char	   *rettypename;

	if (finfo[i].dumped)
		goto done;

	finfo[i].dumped = 1;

	/* becomeUser(fout, finfo[i].usename); */

	sprintf(query, "SELECT lanname FROM pg_language WHERE oid = '%u'::oid",
			finfo[i].lang);
	res = PQexec(g_conn, query);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to get name of procedural language failed: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}
	nlangs = PQntuples(res);

	if (nlangs != 1)
	{
		write_msg(NULL, "procedural language for function %s not found\n", finfo[i].proname);
		exit_nicely();
	}

	i_lanname = PQfnumber(res, "lanname");

	/*
	 * See backend/commands/define.c for details of how the 'AS' clause is
	 * used.
	 */
	if (strcmp(finfo[i].probin, "-") != 0)
	{
		appendPQExpBuffer(asPart, "AS ");
		formatStringLiteral(asPart, finfo[i].probin, CONV_ALL);
		if (strcmp(finfo[i].prosrc, "-") != 0)
		{
			appendPQExpBuffer(asPart, ", ");
			formatStringLiteral(asPart, finfo[i].prosrc, PASS_LFTAB);
		}
	}
	else
	{
		if (strcmp(finfo[i].prosrc, "-") != 0)
		{
			appendPQExpBuffer(asPart, "AS ");
			formatStringLiteral(asPart, finfo[i].prosrc, PASS_LFTAB);
		}
	}

	strcpy(func_lang, PQgetvalue(res, 0, i_lanname));

	PQclear(res);

	resetPQExpBuffer(fn);
	appendPQExpBuffer(fn, "%s (", fmtId(finfo[i].proname, force_quotes));
	for (j = 0; j < finfo[i].nargs; j++)
	{
		char	   *typname;

		typname = findTypeByOid(tinfo, numTypes, finfo[i].argtypes[j], zeroAsOpaque);
		if (typname == NULL)
		{
			write_msg(NULL, "WARNING: function \"%s\" not dumped\n",
					  finfo[i].proname);

			write_msg(NULL, "reason: data type name of argument %d (oid %s) not found\n",
					  j, finfo[i].argtypes[j]);
			goto done;
		}

		appendPQExpBuffer(fn, "%s%s",
						  (j > 0) ? "," : "",
						  typname);
		appendPQExpBuffer(fnlist, "%s%s",
						  (j > 0) ? "," : "",
						  typname);
	}
	appendPQExpBuffer(fn, ")");

	resetPQExpBuffer(delqry);
	appendPQExpBuffer(delqry, "DROP FUNCTION %s;\n", fn->data);

	rettypename = findTypeByOid(tinfo, numTypes, finfo[i].prorettype, zeroAsOpaque);

	if (rettypename == NULL)
	{
		write_msg(NULL, "WARNING: function \"%s\" not dumped\n",
				  finfo[i].proname);

		write_msg(NULL, "reason: name of return data type (oid %s) not found\n",
				  finfo[i].prorettype);
		goto done;
	}

	resetPQExpBuffer(q);
	appendPQExpBuffer(q, "CREATE FUNCTION %s ", fn->data);
	appendPQExpBuffer(q, "RETURNS %s%s %s LANGUAGE ",
					  (finfo[i].retset) ? "SETOF " : "",
					  rettypename,
					  asPart->data);
	formatStringLiteral(q, func_lang, CONV_ALL);

	if (finfo[i].iscachable || finfo[i].isstrict)		/* OR in new attrs here */
	{
		appendPQExpBuffer(q, " WITH (");
		listSep = listSepNone;

		if (finfo[i].iscachable)
		{
			appendPQExpBuffer(q, "%s iscachable", listSep);
			listSep = listSepComma;
		}

		if (finfo[i].isstrict)
		{
			appendPQExpBuffer(q, "%s isstrict", listSep);
			listSep = listSepComma;
		}
		appendPQExpBuffer(q, " )");
	}

	appendPQExpBuffer(q, ";\n");

	ArchiveEntry(fout, finfo[i].oid, fn->data, "FUNCTION", NULL, q->data, delqry->data,
				 "", finfo[i].usename, NULL, NULL);

	/*** Dump Function Comments ***/

	resetPQExpBuffer(q);
	appendPQExpBuffer(q, "FUNCTION %s ",
					  fmtId(finfo[i].proname, force_quotes));
	appendPQExpBuffer(q, "( %s )", fnlist->data);
	dumpComment(fout, q->data, finfo[i].oid, "pg_proc", 0, NULL);

done:
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(fn);
	destroyPQExpBuffer(delqry);
	destroyPQExpBuffer(fnlist);
	destroyPQExpBuffer(asPart);
}

/*
 * dumpOprs
 *	  writes out to fout the queries to recreate all the user-defined operators
 *
 */
void
dumpOprs(Archive *fout, OprInfo *oprinfo, int numOperators,
		 TypeInfo *tinfo, int numTypes)
{
	int			i;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer leftarg = createPQExpBuffer();
	PQExpBuffer rightarg = createPQExpBuffer();
	PQExpBuffer commutator = createPQExpBuffer();
	PQExpBuffer negator = createPQExpBuffer();
	PQExpBuffer restrictor = createPQExpBuffer();
	PQExpBuffer join = createPQExpBuffer();
	PQExpBuffer sort1 = createPQExpBuffer();
	PQExpBuffer sort2 = createPQExpBuffer();

	for (i = 0; i < numOperators; i++)
	{
		char	   *name;

		resetPQExpBuffer(leftarg);
		resetPQExpBuffer(rightarg);
		resetPQExpBuffer(commutator);
		resetPQExpBuffer(negator);
		resetPQExpBuffer(restrictor);
		resetPQExpBuffer(join);
		resetPQExpBuffer(sort1);
		resetPQExpBuffer(sort2);

		/* skip all the builtin oids */
		if (atooid(oprinfo[i].oid) <= g_last_builtin_oid)
			continue;

		/*
		 * some operator are invalid because they were the result of user
		 * defining operators before commutators exist
		 */
		if (strcmp(oprinfo[i].oprcode, "-") == 0)
			continue;

		/*
		 * right unary means there's a left arg and left unary means
		 * there's a right arg
		 */
		if (strcmp(oprinfo[i].oprkind, "r") == 0 ||
			strcmp(oprinfo[i].oprkind, "b") == 0)
		{
			name = findTypeByOid(tinfo, numTypes,
								 oprinfo[i].oprleft, zeroAsOpaque);
			if (name == NULL)
			{
				write_msg(NULL, "WARNING: operator \"%s\" (oid %s) not dumped\n",
						  oprinfo[i].oprname, oprinfo[i].oid);
				write_msg(NULL, "reason: oprleft (oid %s) not found\n",
						  oprinfo[i].oprleft);
				continue;
			}
			appendPQExpBuffer(leftarg, ",\n\tLEFTARG = %s ", name);
		}

		if (strcmp(oprinfo[i].oprkind, "l") == 0 ||
			strcmp(oprinfo[i].oprkind, "b") == 0)
		{
			name = findTypeByOid(tinfo, numTypes,
								 oprinfo[i].oprright, zeroAsOpaque);
			if (name == NULL)
			{
				write_msg(NULL, "WARNING: operator \"%s\" (oid %s) not dumped\n",
						  oprinfo[i].oprname, oprinfo[i].oid);
				write_msg(NULL, "reason: oprright (oid %s) not found\n",
						  oprinfo[i].oprright);
				continue;
			}
			appendPQExpBuffer(rightarg, ",\n\tRIGHTARG = %s ", name);
		}

		if (!(strcmp(oprinfo[i].oprcom, "0") == 0))
		{
			name = findOprByOid(oprinfo, numOperators, oprinfo[i].oprcom);
			if (name == NULL)
			{
				write_msg(NULL, "WARNING: operator \"%s\" (oid %s) not dumped\n",
						  oprinfo[i].oprname, oprinfo[i].oid);
				write_msg(NULL, "reason: oprcom (oid %s) not found\n",
						  oprinfo[i].oprcom);
				continue;
			}
			appendPQExpBuffer(commutator, ",\n\tCOMMUTATOR = %s ", name);
		}

		if (!(strcmp(oprinfo[i].oprnegate, "0") == 0))
		{
			name = findOprByOid(oprinfo, numOperators, oprinfo[i].oprnegate);
			if (name == NULL)
			{
				write_msg(NULL, "WARNING: operator \"%s\" (oid %s) not dumped\n",
						  oprinfo[i].oprname, oprinfo[i].oid);
				write_msg(NULL, "reason: oprnegate (oid %s) not found\n",
						  oprinfo[i].oprnegate);
				continue;
			}
			appendPQExpBuffer(negator, ",\n\tNEGATOR = %s ", name);
		}

		if (!(strcmp(oprinfo[i].oprrest, "-") == 0))
			appendPQExpBuffer(restrictor, ",\n\tRESTRICT = %s ", oprinfo[i].oprrest);

		if (!(strcmp(oprinfo[i].oprjoin, "-") == 0))
			appendPQExpBuffer(join, ",\n\tJOIN = %s ", oprinfo[i].oprjoin);

		if (!(strcmp(oprinfo[i].oprlsortop, "0") == 0))
		{
			name = findOprByOid(oprinfo, numOperators, oprinfo[i].oprlsortop);
			if (name == NULL)
			{
				write_msg(NULL, "WARNING: operator \"%s\" (oid %s) not dumped\n",
						  oprinfo[i].oprname, oprinfo[i].oid);
				write_msg(NULL, "reason: oprlsortop (oid %s) not found\n",
						  oprinfo[i].oprlsortop);
				continue;
			}
			appendPQExpBuffer(sort1, ",\n\tSORT1 = %s ", name);
		}

		if (!(strcmp(oprinfo[i].oprrsortop, "0") == 0))
		{
			name = findOprByOid(oprinfo, numOperators, oprinfo[i].oprrsortop);
			if (name == NULL)
			{
				write_msg(NULL, "WARNING: operator \"%s\" (oid %s) not dumped\n",
						  oprinfo[i].oprname, oprinfo[i].oid);
				write_msg(NULL, "reason: oprrsortop (oid %s) not found\n",
						  oprinfo[i].oprrsortop);
				continue;
			}
			appendPQExpBuffer(sort2, ",\n\tSORT2 = %s ", name);
		}

		resetPQExpBuffer(delq);
		appendPQExpBuffer(delq, "DROP OPERATOR %s (%s",
						  oprinfo[i].oprname,
					   findTypeByOid(tinfo, numTypes, oprinfo[i].oprleft,
									 zeroAsNone));
		appendPQExpBuffer(delq, ", %s);\n",
					  findTypeByOid(tinfo, numTypes, oprinfo[i].oprright,
									zeroAsNone));

		resetPQExpBuffer(q);
		appendPQExpBuffer(q,
						  "CREATE OPERATOR %s "
						  "(PROCEDURE = %s %s%s%s%s%s%s%s%s%s);\n",
						  oprinfo[i].oprname,
						  oprinfo[i].oprcode,
						  leftarg->data,
						  rightarg->data,
						  commutator->data,
						  negator->data,
						  restrictor->data,
		  (strcmp(oprinfo[i].oprcanhash, "t") == 0) ? ",\n\tHASHES" : "",
						  join->data,
						  sort1->data,
						  sort2->data);

		ArchiveEntry(fout, oprinfo[i].oid, oprinfo[i].oprname, "OPERATOR", NULL,
				q->data, delq->data, "", oprinfo[i].usename, NULL, NULL);
	}

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(leftarg);
	destroyPQExpBuffer(rightarg);
	destroyPQExpBuffer(commutator);
	destroyPQExpBuffer(negator);
	destroyPQExpBuffer(restrictor);
	destroyPQExpBuffer(join);
	destroyPQExpBuffer(sort1);
	destroyPQExpBuffer(sort2);
}

/*
 * dumpAggs
 *	  writes out to fout the queries to create all the user-defined aggregates
 *
 */
void
dumpAggs(Archive *fout, AggInfo *agginfo, int numAggs,
		 TypeInfo *tinfo, int numTypes)
{
	int			i;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer aggSig = createPQExpBuffer();
	PQExpBuffer details = createPQExpBuffer();

	for (i = 0; i < numAggs; i++)
	{
		char	   *name;

		resetPQExpBuffer(details);

		/* skip all the builtin oids */
		if (oidle(atooid(agginfo[i].oid), g_last_builtin_oid))
			continue;

		resetPQExpBuffer(aggSig);
		appendPQExpBuffer(aggSig, "%s(%s)", agginfo[i].aggname,
						  findTypeByOid(tinfo, numTypes,
										agginfo[i].aggbasetype,
										zeroAsStar + useBaseTypeName));

		if (!agginfo[i].convertok)
		{
			write_msg(NULL, "WARNING: aggregate function %s could not be dumped correctly for this database version; ignored\n",
					  aggSig->data);

			resetPQExpBuffer(q);
			appendPQExpBuffer(q, "-- WARNING: aggregate function %s could not be dumped correctly for this database version; ignored\n",
							  aggSig->data);
			ArchiveEntry(fout, agginfo[i].oid, aggSig->data, "WARNING", NULL,
			 q->data, "" /* Del */ , "", agginfo[i].usename, NULL, NULL);
			continue;
		}

		name = findTypeByOid(tinfo, numTypes, agginfo[i].aggbasetype,
							 zeroAsAny + useBaseTypeName);
		if (name == NULL)
		{
			write_msg(NULL, "WARNING: aggregate function \"%s\" (oid %s) not dumped\n",
					  agginfo[i].aggname, agginfo[i].oid);
			write_msg(NULL, "reason: aggbasetype (oid %s) not found\n",
					  agginfo[i].aggbasetype);

			resetPQExpBuffer(q);
			appendPQExpBuffer(q, "-- WARNING: aggregate function \"%s\" (oid %s) not dumped\n", agginfo[i].aggname, agginfo[i].oid);
			appendPQExpBuffer(q, "-- reason: aggbasetype (oid %s) not found\n", agginfo[i].aggbasetype);
			ArchiveEntry(fout, agginfo[i].oid, aggSig->data, "WARNING", NULL,
			 q->data, "" /* Del */ , "", agginfo[i].usename, NULL, NULL);
			continue;
		}
		appendPQExpBuffer(details, "BASETYPE = %s, ", name);

		name = findTypeByOid(tinfo, numTypes, agginfo[i].aggtranstype,
							 zeroAsOpaque + useBaseTypeName);
		if (name == NULL)
		{
			write_msg(NULL, "WARNING: aggregate function \"%s\" (oid %s) not dumped\n",
					  agginfo[i].aggname, agginfo[i].oid);
			write_msg(NULL, "reason: aggtranstype (oid %s) not found\n",
					  agginfo[i].aggtranstype);

			resetPQExpBuffer(q);
			appendPQExpBuffer(q, "-- WARNING: aggregate function \"%s\" (oid %s) not dumped\n", agginfo[i].aggname, agginfo[i].oid);
			appendPQExpBuffer(q, "-- reason: aggtranstype (oid %s) not found\n", agginfo[i].aggtranstype);
			ArchiveEntry(fout, agginfo[i].oid, aggSig->data, "WARNING", NULL,
			 q->data, "" /* Del */ , "", agginfo[i].usename, NULL, NULL);
			continue;
		}
		appendPQExpBuffer(details,
						  "SFUNC = %s, STYPE = %s",
						  agginfo[i].aggtransfn, name);

		if (agginfo[i].agginitval)
		{
			appendPQExpBuffer(details, ", INITCOND = ");
			formatStringLiteral(details, agginfo[i].agginitval, CONV_ALL);
		}

		if (!(strcmp(agginfo[i].aggfinalfn, "-") == 0))
			appendPQExpBuffer(details, ", FINALFUNC = %s",
							  agginfo[i].aggfinalfn);

		resetPQExpBuffer(delq);
		appendPQExpBuffer(delq, "DROP AGGREGATE %s;\n", aggSig->data);

		resetPQExpBuffer(q);
		appendPQExpBuffer(q, "CREATE AGGREGATE %s ( %s );\n",
						  agginfo[i].aggname,
						  details->data);

		ArchiveEntry(fout, agginfo[i].oid, aggSig->data, "AGGREGATE", NULL,
				q->data, delq->data, "", agginfo[i].usename, NULL, NULL);

		/*** Dump Aggregate Comments ***/

		resetPQExpBuffer(q);
		appendPQExpBuffer(q, "AGGREGATE %s", aggSig->data);
		dumpComment(fout, q->data, agginfo[i].oid, "pg_aggregate", 0, NULL);
	}

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(aggSig);
	destroyPQExpBuffer(details);
}

/*
 * These are some support functions to fix the acl problem of pg_dump
 *
 * Matthew C. Aycock 12/02/97
 */

/* Append a keyword to a keyword list, inserting comma if needed.
 * Caller must make aclbuf big enough for all possible keywords.
 */
static void
AddAcl(char *aclbuf, const char *keyword)
{
	if (*aclbuf)
		strcat(aclbuf, ",");
	strcat(aclbuf, keyword);
}

/*
 * This will take a string of privilege code letters and return a malloced,
 * comma delimited string of keywords for GRANT.
 *
 * Note: for cross-version compatibility, it's important to use ALL when
 * appropriate.
 */
static char *
GetPrivileges(Archive *AH, const char *s)
{
	char		aclbuf[100];
	bool		all = true;

	aclbuf[0] = '\0';

#define CONVERT_PRIV(code,keywd) \
	if (strchr(s, code)) \
		AddAcl(aclbuf, keywd); \
	else \
		all = false

	CONVERT_PRIV('a', "INSERT");
	CONVERT_PRIV('r', "SELECT");
	CONVERT_PRIV('R', "RULE");

	if (AH->remoteVersion >= 70200)
	{
		CONVERT_PRIV('w', "UPDATE");
		CONVERT_PRIV('d', "DELETE");
		CONVERT_PRIV('x', "REFERENCES");
		CONVERT_PRIV('t', "TRIGGER");
	}
	else
	{
		/* 7.0 and 7.1 have a simpler worldview */
		CONVERT_PRIV('w', "UPDATE,DELETE");
	}

#undef CONVERT_PRIV

	if (all)
		return strdup("ALL");
	else
		return strdup(aclbuf);
}

/*
 * The name says it all; a function to append a string if the dest
 * is big enough. If not, it does a realloc.
 */
static void
strcatalloc(char **dest, int *dSize, char *src)
{
	int			dLen = strlen(*dest);
	int			sLen = strlen(src);

	if ((dLen + sLen) >= *dSize)
	{
		*dSize = (dLen + sLen) * 2;
		*dest = realloc(*dest, *dSize);
	}
	strcpy(*dest + dLen, src);
}


/*
 * dumpACL:
 *	  Write out grant/revoke information
 *	  Called for sequences and tables
 */

static void
dumpACL(Archive *fout, TableInfo tbinfo)
{
	const char *acls = tbinfo.relacl;
	char	   *aclbuf,
			   *tok,
			   *eqpos,
			   *priv;
	char	   *objoid;
	char	   *sql;
	char		tmp[1024];
	int			sSize = 4096;

	if (strlen(acls) == 0)
		return;					/* table has default permissions */

	/*
	 * Allocate a larginsh buffer for the output SQL.
	 */
	sql = (char *) malloc(sSize);

	/*
	 * Revoke Default permissions for PUBLIC. Is this actually necessary,
	 * or is it just a waste of time?
	 */
	sprintf(sql, "REVOKE ALL on %s from PUBLIC;\n",
			fmtId(tbinfo.relname, force_quotes));

	/* Make a working copy of acls so we can use strtok */
	aclbuf = strdup(acls);

	/* Scan comma-separated ACL items */
	for (tok = strtok(aclbuf, ","); tok != NULL; tok = strtok(NULL, ","))
	{
		/*
		 * Token may start with '{' and/or '"'.  Actually only the start
		 * of the string should have '{', but we don't verify that.
		 */
		if (*tok == '{')
			tok++;
		if (*tok == '"')
			tok++;

		/* User name is string up to = in tok */
		eqpos = strchr(tok, '=');
		if (!eqpos)
		{
			write_msg(NULL, "could not parse ACL list ('%s') for relation %s\n",
					  acls, tbinfo.relname);
			exit_nicely();
		}

		/*
		 * Parse the privileges (right-hand side).	Skip if there are
		 * none.
		 */
		priv = GetPrivileges(fout, eqpos + 1);
		if (*priv)
		{
			sprintf(tmp, "GRANT %s on %s to ",
					priv, fmtId(tbinfo.relname, force_quotes));
			strcatalloc(&sql, &sSize, tmp);

			/*
			 * Note: fmtId() can only be called once per printf, so don't
			 * try to merge printing of username into the above printf.
			 */
			if (eqpos == tok)
			{
				/* Empty left-hand side means "PUBLIC" */
				strcatalloc(&sql, &sSize, "PUBLIC;\n");
			}
			else
			{
				*eqpos = '\0';	/* it's ok to clobber aclbuf */
				if (strncmp(tok, "group ", strlen("group ")) == 0)
					sprintf(tmp, "GROUP %s;\n",
							fmtId(tok + strlen("group "), force_quotes));
				else
					sprintf(tmp, "%s;\n", fmtId(tok, force_quotes));
				strcatalloc(&sql, &sSize, tmp);
			}
		}
		free(priv);
	}

	free(aclbuf);

	if (tbinfo.viewdef != NULL)
		objoid = tbinfo.viewoid;
	else
		objoid = tbinfo.oid;

	ArchiveEntry(fout, objoid, tbinfo.relname, "ACL", NULL, sql, "", "", "", NULL, NULL);

}

static void
_dumpTableAttr70(Archive *fout, TableInfo *tblinfo, int i, int j, PQExpBuffer q)
{
	int32		tmp_typmod;
	int			precision;
	int			scale;

	/* Show lengths on bpchar and varchar */
	if (!strcmp(tblinfo[i].typnames[j], "bpchar"))
	{
		int			len = (tblinfo[i].atttypmod[j] - VARHDRSZ);

		appendPQExpBuffer(q, "character");
		if (len > 1)
			appendPQExpBuffer(q, "(%d)",
							  tblinfo[i].atttypmod[j] - VARHDRSZ);
	}
	else if (!strcmp(tblinfo[i].typnames[j], "varchar"))
	{
		appendPQExpBuffer(q, "character varying");
		if (tblinfo[i].atttypmod[j] != -1)
		{
			appendPQExpBuffer(q, "(%d)",
							  tblinfo[i].atttypmod[j] - VARHDRSZ);
		}
	}
	else if (!strcmp(tblinfo[i].typnames[j], "numeric"))
	{
		appendPQExpBuffer(q, "numeric");
		if (tblinfo[i].atttypmod[j] != -1)
		{
			tmp_typmod = tblinfo[i].atttypmod[j] - VARHDRSZ;
			precision = (tmp_typmod >> 16) & 0xffff;
			scale = tmp_typmod & 0xffff;
			appendPQExpBuffer(q, "(%d,%d)",
							  precision, scale);
		}
	}

	/*
	 * char is an internal single-byte data type; Let's make sure we force
	 * it through with quotes. - thomas 1998-12-13
	 */
	else if (!strcmp(tblinfo[i].typnames[j], "char"))
	{
		appendPQExpBuffer(q, "%s",
						  fmtId(tblinfo[i].typnames[j], true));
	}
	else
	{
		appendPQExpBuffer(q, "%s",
						  fmtId(tblinfo[i].typnames[j], false));
	}
}

/*
 * dumpTables:
 *	  write out to fout all the user-define tables
 */

void
dumpTables(Archive *fout, TableInfo *tblinfo, int numTables,
		   IndInfo *indinfo, int numIndexes,
		   InhInfo *inhinfo, int numInherits,
		   TypeInfo *tinfo, int numTypes, const char *tablename,
		   const bool aclsSkip, const bool oids,
		   const bool schemaOnly, const bool dataOnly)
{
	int			i,
				j,
				k;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	char	   *serialSeq = NULL;		/* implicit sequence name created
										 * by SERIAL datatype */
	const char *serialSeqSuffix = "_id_seq";	/* suffix for implicit
												 * SERIAL sequences */
	char	  **parentRels;		/* list of names of parent relations */
	int			numParents;
	int			actual_atts;	/* number of attrs in this CREATE statment */
	char	   *reltypename;
	char	   *objoid;
	const char *((*commentDeps)[]);

	/* First - dump SEQUENCEs */
	if (tablename && strlen(tablename) > 0)
	{
		/* XXX this code only works for serial columns named "id" */
		/* We really need dependency analysis! */
		serialSeq = malloc(strlen(tablename) + strlen(serialSeqSuffix) + 1);
		strcpy(serialSeq, tablename);
		strcat(serialSeq, serialSeqSuffix);
	}
	for (i = 0; i < numTables; i++)
	{
		if (!(tblinfo[i].sequence))
			continue;
		if (!tablename || (!strcmp(tblinfo[i].relname, tablename))
			|| (serialSeq && !strcmp(tblinfo[i].relname, serialSeq)))
		{
			/* becomeUser(fout, tblinfo[i].usename); */
			dumpSequence(fout, tblinfo[i], schemaOnly, dataOnly);
			if (!aclsSkip)
				dumpACL(fout, tblinfo[i]);
		}
	}
	if (serialSeq)
		free(serialSeq);

	for (i = 0; i < numTables; i++)
	{
		if (tblinfo[i].sequence)	/* already dumped */
			continue;

		if (!tablename || (!strcmp(tblinfo[i].relname, tablename)) || (strlen(tablename) == 0))
		{

			resetPQExpBuffer(delq);
			resetPQExpBuffer(q);

			/* Use the view definition if there is one */
			if (tblinfo[i].viewdef != NULL)
			{
				reltypename = "VIEW";
				objoid = tblinfo[i].viewoid;
				appendPQExpBuffer(delq, "DROP VIEW %s;\n", fmtId(tblinfo[i].relname, force_quotes));
				appendPQExpBuffer(q, "CREATE VIEW %s as %s\n", fmtId(tblinfo[i].relname, force_quotes), tblinfo[i].viewdef);
				commentDeps = malloc(sizeof(char *) * 2);
				(*commentDeps)[0] = strdup(objoid);
				(*commentDeps)[1] = NULL;		/* end of list */
			}
			else
			{
				reltypename = "TABLE";
				objoid = tblinfo[i].oid;
				commentDeps = NULL;
				parentRels = tblinfo[i].parentRels;
				numParents = tblinfo[i].numParents;

				appendPQExpBuffer(delq, "DROP TABLE %s;\n", fmtId(tblinfo[i].relname, force_quotes));

				appendPQExpBuffer(q, "CREATE TABLE %s (\n\t", fmtId(tblinfo[i].relname, force_quotes));
				actual_atts = 0;
				for (j = 0; j < tblinfo[i].numatts; j++)
				{
					/* Is this one of the table's own attrs ? */
					if (tblinfo[i].inhAttrs[j] == 0)
					{
						/* Format properly if not first attr */
						if (actual_atts > 0)
							appendPQExpBuffer(q, ",\n\t");

						/* Attr name & type */
						appendPQExpBuffer(q, "%s ", fmtId(tblinfo[i].attnames[j], force_quotes));

						if (g_fout->remoteVersion >= 70100)
							appendPQExpBuffer(q, "%s", tblinfo[i].atttypedefns[j]);
						else
							_dumpTableAttr70(fout, tblinfo, i, j, q);

						/* Default value */
						if (tblinfo[i].adef_expr[j] != NULL && tblinfo[i].inhAttrDef[j] == 0)
							appendPQExpBuffer(q, " DEFAULT %s",
											  tblinfo[i].adef_expr[j]);

						/* Not Null constraint */
						if (tblinfo[i].notnull[j] && tblinfo[i].inhNotNull[j] == 0)
							appendPQExpBuffer(q, " NOT NULL");

						actual_atts++;
					}
				}



				/* put the CONSTRAINTS inside the table def */
				for (k = 0; k < tblinfo[i].ncheck; k++)
				{
					if (actual_atts + k > 0)
						appendPQExpBuffer(q, ",\n\t");

					appendPQExpBuffer(q, "%s",
									  tblinfo[i].check_expr[k]);
				}

				/* Primary Key */
				if (tblinfo[i].pkIndexOid != NULL)
				{
					PQExpBuffer consDef;

					/* Find the corresponding index */
					for (k = 0; k < numIndexes; k++)
					{
						if (strcmp(indinfo[k].indexreloid,
								   tblinfo[i].pkIndexOid) == 0)
							break;
					}

					if (k >= numIndexes)
					{
						write_msg(NULL, "dumpTables(): failed sanity check, could not find index (%s) for primary key constraint\n",
								  tblinfo[i].pkIndexOid);
						exit_nicely();
					}

					consDef = getPKconstraint(&tblinfo[i], &indinfo[k]);

					if ((actual_atts + tblinfo[i].ncheck) > 0)
						appendPQExpBuffer(q, ",\n\t");

					appendPQExpBuffer(q, "%s", consDef->data);

					destroyPQExpBuffer(consDef);
				}


				appendPQExpBuffer(q, "\n)");

				if (numParents > 0)
				{
					appendPQExpBuffer(q, "\nINHERITS (");
					for (k = 0; k < numParents; k++)
					{
						appendPQExpBuffer(q, "%s%s",
										  (k > 0) ? ", " : "",
									 fmtId(parentRels[k], force_quotes));
					}
					appendPQExpBuffer(q, ")");
				}

				if (!tblinfo[i].hasoids)
					appendPQExpBuffer(q, " WITHOUT OIDS");

				appendPQExpBuffer(q, ";\n");
			}

			if (!dataOnly)
			{

				ArchiveEntry(fout, objoid, tblinfo[i].relname,
							 reltypename, NULL, q->data, delq->data, "", tblinfo[i].usename,
							 NULL, NULL);

				if (!aclsSkip)
					dumpACL(fout, tblinfo[i]);

			}

			/* Dump Field Comments */

			for (j = 0; j < tblinfo[i].numatts; j++)
			{
				resetPQExpBuffer(q);
				appendPQExpBuffer(q, "COLUMN %s", fmtId(tblinfo[i].relname, force_quotes));
				appendPQExpBuffer(q, ".");
				appendPQExpBuffer(q, "%s", fmtId(tblinfo[i].attnames[j], force_quotes));
				dumpComment(fout, q->data, tblinfo[i].oid,
							"pg_class", j + 1, commentDeps);
			}

			/* Dump Table Comments */

			resetPQExpBuffer(q);
			appendPQExpBuffer(q, "%s %s", reltypename, fmtId(tblinfo[i].relname, force_quotes));
			dumpComment(fout, q->data, tblinfo[i].oid,
						"pg_class", 0, commentDeps);

		}
	}

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
}

static PQExpBuffer
getPKconstraint(TableInfo *tblInfo, IndInfo *indInfo)
{
	PQExpBuffer pkBuf = createPQExpBuffer();
	int			k;

	appendPQExpBuffer(pkBuf, "Constraint %s Primary Key (",
					  tblInfo->primary_key_name);

	for (k = 0; k < INDEX_MAX_KEYS; k++)
	{
		int			indkey;
		const char *attname;

		indkey = atoi(indInfo->indkey[k]);
		if (indkey == InvalidAttrNumber)
			break;
		attname = getAttrName(indkey, tblInfo);

		appendPQExpBuffer(pkBuf, "%s%s",
						  (k == 0) ? "" : ", ",
						  fmtId(attname, force_quotes));
	}

	appendPQExpBuffer(pkBuf, ")");

	return pkBuf;
}

/*
 * getAttrName: extract the correct name for an attribute
 *
 * The array tblInfo->attnames[] only provides names of user attributes;
 * if a system attribute number is supplied, we have to fake it.
 * We also do a little bit of bounds checking for safety's sake.
 */
static const char *
getAttrName(int attrnum, TableInfo *tblInfo)
{
	if (attrnum > 0 && attrnum <= tblInfo->numatts)
		return tblInfo->attnames[attrnum - 1];
	switch (attrnum)
	{
		case SelfItemPointerAttributeNumber:
			return "ctid";
		case ObjectIdAttributeNumber:
			return "oid";
		case MinTransactionIdAttributeNumber:
			return "xmin";
		case MinCommandIdAttributeNumber:
			return "cmin";
		case MaxTransactionIdAttributeNumber:
			return "xmax";
		case MaxCommandIdAttributeNumber:
			return "cmax";
		case TableOidAttributeNumber:
			return "tableoid";
	}
	write_msg(NULL, "getAttrName(): invalid column number %d for table %s\n",
			  attrnum, tblInfo->relname);
	exit_nicely();
	return NULL;				/* keep compiler quiet */
}

/*
 * dumpIndexes:
 *	  write out to fout all the user-defined indexes
 */
void
dumpIndexes(Archive *fout, IndInfo *indinfo, int numIndexes,
			TableInfo *tblinfo, int numTables, const char *tablename)
{
	int			i;
	int			tableInd;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer id1 = createPQExpBuffer();

	for (i = 0; i < numIndexes; i++)
	{
		if (tablename && tablename[0] &&
			(strcmp(indinfo[i].indrelname, tablename) != 0))
			continue;

		tableInd = findTableByName(tblinfo, numTables,
								   indinfo[i].indrelname);
		if (tableInd < 0)
		{
			write_msg(NULL, "dumpIndexes(): failed sanity check, table %s was not found\n",
					  indinfo[i].indrelname);
			exit_nicely();
		}

		/* Handle PK indexes */
		if (strcmp(indinfo[i].indisprimary, "t") == 0)
		{
#if 0

			/*
			 * PK: Enable this code when ALTER TABLE supports PK
			 * constraints.
			 */

			PQExpBuffer consDef = getPKconstraint(&tblinfo[tableInd], &indinfo[i]);

			resetPQExpBuffer(q);

			appendPQExpBuffer(q, "Alter Table %s Add %s;",
						  fmtId(tblinfo[tableInd].relname, force_quotes),
							  consDef->data);

			ArchiveEntry(fout, indinfo[i].oid, tblinfo[tableInd].primary_key_name,
						 "CONSTRAINT", NULL, q->data, "",
						 "", tblinfo[tableInd].usename, NULL, NULL);

			destroyPQExpBuffer(consDef);
#endif

			/*
			 * Don't need to do anything else for this system-generated
			 * index
			 */
			continue;
		}

		resetPQExpBuffer(id1);
		appendPQExpBuffer(id1, fmtId(indinfo[i].indexrelname, force_quotes));

		resetPQExpBuffer(q);
		appendPQExpBuffer(q, "%s;\n", indinfo[i].indexdef);

		resetPQExpBuffer(delq);
		appendPQExpBuffer(delq, "DROP INDEX %s;\n", id1->data);

		/*
		 * We make the index belong to the owner of its table, which is
		 * not necessarily right but should answer 99% of the time. Would
		 * have to add owner name to IndInfo to do it right.
		 */
		ArchiveEntry(fout, indinfo[i].indexreloid, id1->data,
					 "INDEX", NULL, q->data, delq->data,
					 "", tblinfo[tableInd].usename, NULL, NULL);

		/* Dump Index Comments */
		resetPQExpBuffer(q);
		appendPQExpBuffer(q, "INDEX %s", id1->data);
		dumpComment(fout, q->data, indinfo[i].indexreloid,
					"pg_class", 0, NULL);
	}

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(id1);
}

/*
 * dumpTuples
 *	  prints out the tuples in ASCII representation. The output is a valid
 *	  input to COPY FROM stdin.
 *
 *	  We only need to do this for POSTGRES 4.2 databases since the
 *	  COPY TO statment doesn't escape newlines properly. It's been fixed
 *	  in PostgreSQL.
 *
 * the attrmap passed in tells how to map the attributes copied in to the
 * attributes copied out
 */
#ifdef NOT_USED
void
dumpTuples(PGresult *res, FILE *fout, int *attrmap)
{
	int			j,
				k;
	int			m,
				n;
	char	  **outVals = NULL; /* values to copy out */

	n = PQntuples(res);
	m = PQnfields(res);

	if (m > 0)
	{
		/*
		 * Print out the tuples but only print tuples with at least 1
		 * field.
		 */
		outVals = (char **) malloc(m * sizeof(char *));

		for (j = 0; j < n; j++)
		{
			for (k = 0; k < m; k++)
				outVals[attrmap[k]] = PQgetvalue(res, j, k);
			for (k = 0; k < m; k++)
			{
				char	   *pval = outVals[k];

				if (k != 0)
					fputc('\t', fout);	/* delimiter for attribute */

				if (pval)
				{
					while (*pval != '\0')
					{
						/* escape tabs, newlines and backslashes */
						if (*pval == '\t' || *pval == '\n' || *pval == '\\')
							fputc('\\', fout);
						fputc(*pval, fout);
						pval++;
					}
				}
			}
			fputc('\n', fout);	/* delimiter for a tuple */
		}
		free(outVals);
	}
}
#endif

/*
 * setMaxOid -
 * find the maximum oid and generate a COPY statement to set it
*/

static void
setMaxOid(Archive *fout)
{
	PGresult   *res;
	Oid			max_oid;
	char		sql[1024];

	res = PQexec(g_conn, "CREATE TEMPORARY TABLE pgdump_oid (dummy int4)");
	if (!res ||
		PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		write_msg(NULL, "could not create pgdump_oid table: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}
	PQclear(res);
	res = PQexec(g_conn, "INSERT INTO pgdump_oid VALUES (0)");
	if (!res ||
		PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		write_msg(NULL, "could not insert into pgdump_oid table: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}
	max_oid = PQoidValue(res);
	if (max_oid == 0)
	{
		write_msg(NULL, "inserted invalid oid\n");
		exit_nicely();
	}
	PQclear(res);
	res = PQexec(g_conn, "DROP TABLE pgdump_oid;");
	if (!res ||
		PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		write_msg(NULL, "could not drop pgdump_oid table: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}
	PQclear(res);
	if (g_verbose)
		write_msg(NULL, "maximum system oid is %u\n", max_oid);
	snprintf(sql, 1024,
			 "CREATE TEMPORARY TABLE pgdump_oid (dummy int4);\n"
			 "COPY pgdump_oid WITH OIDS FROM stdin;\n"
			 "%u\t0\n"
			 "\\.\n"
			 "DROP TABLE pgdump_oid;\n",
			 max_oid);

	ArchiveEntry(fout, "0", "Max OID", "<Init>", NULL, sql, "", "", "", NULL, NULL);
}

/*
 * findLastBuiltInOid -
 * find the last built in oid
 * we do this by retrieving datlastsysoid from the pg_database entry for this database,
 */

static Oid
findLastBuiltinOid_V71(const char *dbname)
{
	PGresult   *res;
	int			ntups;
	Oid			last_oid;
	PQExpBuffer query = createPQExpBuffer();

	resetPQExpBuffer(query);
	appendPQExpBuffer(query, "SELECT datlastsysoid from pg_database where datname = ");
	formatStringLiteral(query, dbname, CONV_ALL);

	res = PQexec(g_conn, query->data);
	if (res == NULL ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "error in finding the last system oid: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}
	ntups = PQntuples(res);
	if (ntups < 1)
	{
		write_msg(NULL, "missing pg_database entry for this database\n");
		exit_nicely();
	}
	if (ntups > 1)
	{
		write_msg(NULL, "found more than one pg_database entry for this database\n");
		exit_nicely();
	}
	last_oid = atooid(PQgetvalue(res, 0, PQfnumber(res, "datlastsysoid")));
	PQclear(res);
	destroyPQExpBuffer(query);
	return last_oid;
}

/*
 * findLastBuiltInOid -
 * find the last built in oid
 * we do this by looking up the oid of 'template1' in pg_database,
 * this is probably not foolproof but comes close
*/

static Oid
findLastBuiltinOid_V70(void)
{
	PGresult   *res;
	int			ntups;
	int			last_oid;

	res = PQexec(g_conn,
			  "SELECT oid from pg_database where datname = 'template1'");
	if (res == NULL ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "error in finding the template1 database: %s", PQerrorMessage(g_conn));
		exit_nicely();
	}
	ntups = PQntuples(res);
	if (ntups < 1)
	{
		write_msg(NULL, "could not find template1 database entry in the pg_database table\n");
		exit_nicely();
	}
	if (ntups > 1)
	{
		write_msg(NULL, "found more than one template1 database entry in the pg_database table\n");
		exit_nicely();
	}
	last_oid = atooid(PQgetvalue(res, 0, PQfnumber(res, "oid")));
	PQclear(res);
	return last_oid;
}

static void
dumpSequence(Archive *fout, TableInfo tbinfo, const bool schemaOnly, const bool dataOnly)
{
	PGresult   *res;
	char	   *last,
			   *incby,
			   *maxv,
			   *minv,
			   *cache;
	bool		cycled,
				called;
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer delqry = createPQExpBuffer();

	appendPQExpBuffer(query,
			"SELECT sequence_name, last_value, increment_by, max_value, "
				  "min_value, cache_value, is_cycled, is_called from %s",
					  fmtId(tbinfo.relname, force_quotes));

	res = PQexec(g_conn, query->data);
	if (!res || PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		write_msg(NULL, "query to get data of sequence \"%s\" failed: %s", tbinfo.relname, PQerrorMessage(g_conn));
		exit_nicely();
	}

	if (PQntuples(res) != 1)
	{
		write_msg(NULL, "query to get data of sequence \"%s\" returned %d rows (expected 1)\n",
				  tbinfo.relname, PQntuples(res));
		exit_nicely();
	}

	if (strcmp(PQgetvalue(res, 0, 0), tbinfo.relname) != 0)
	{
		write_msg(NULL, "query to get data of sequence \"%s\" returned name \"%s\"\n",
				  tbinfo.relname, PQgetvalue(res, 0, 0));
		exit_nicely();
	}

	last = PQgetvalue(res, 0, 1);
	incby = PQgetvalue(res, 0, 2);
	maxv = PQgetvalue(res, 0, 3);
	minv = PQgetvalue(res, 0, 4);
	cache = PQgetvalue(res, 0, 5);
	cycled = (strcmp(PQgetvalue(res, 0, 6), "t") == 0);
	called = (strcmp(PQgetvalue(res, 0, 7), "t") == 0);

	/*
	 * The logic we use for restoring sequences is as follows: -   Add a
	 * basic CREATE SEQUENCE statement (use last_val for start if called
	 * is false, else use min_val for start_val).
	 *
	 * Add a 'SETVAL(seq, last_val, iscalled)' at restore-time iff we load
	 * data
	 */

	if (!dataOnly)
	{
		resetPQExpBuffer(delqry);
		appendPQExpBuffer(delqry, "DROP SEQUENCE %s;\n",
						  fmtId(tbinfo.relname, force_quotes));

		resetPQExpBuffer(query);
		appendPQExpBuffer(query,
						  "CREATE SEQUENCE %s start %s increment %s "
						  "maxvalue %s minvalue %s cache %s%s;\n",
						  fmtId(tbinfo.relname, force_quotes),
						  (called ? minv : last),
						  incby, maxv, minv, cache,
						  (cycled ? " cycle" : ""));

		ArchiveEntry(fout, tbinfo.oid, tbinfo.relname, "SEQUENCE", NULL,
					 query->data, delqry->data, "", tbinfo.usename,
					 NULL, NULL);
	}

	if (!schemaOnly)
	{
		resetPQExpBuffer(query);
		appendPQExpBuffer(query, "SELECT setval (");
		formatStringLiteral(query, fmtId(tbinfo.relname, force_quotes), CONV_ALL);
		appendPQExpBuffer(query, ", %s, %s);\n",
						  last, (called ? "true" : "false"));

		ArchiveEntry(fout, tbinfo.oid, tbinfo.relname, "SEQUENCE SET", NULL,
					 query->data, "" /* Del */ , "", tbinfo.usename,
					 NULL, NULL);
	}

	if (!dataOnly)
	{
		/* Dump Sequence Comments */

		resetPQExpBuffer(query);
		appendPQExpBuffer(query, "SEQUENCE %s", fmtId(tbinfo.relname, force_quotes));
		dumpComment(fout, query->data, tbinfo.oid,
					"pg_class", 0, NULL);
	}

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(delqry);
}


static void
dumpTriggers(Archive *fout, const char *tablename,
			 TableInfo *tblinfo, int numTables)
{
	int			i,
				j;

	if (g_verbose)
		write_msg(NULL, "dumping out triggers\n");

	for (i = 0; i < numTables; i++)
	{
		if (tablename && (strcmp(tblinfo[i].relname, tablename) != 0) && (strlen(tablename) > 0))
			continue;

		for (j = 0; j < tblinfo[i].ntrig; j++)
		{
			ArchiveEntry(fout, tblinfo[i].triggers[j].oid, tblinfo[i].triggers[j].tgname,
				   "TRIGGER", NULL, tblinfo[i].triggers[j].tgsrc, "", "",
						 tblinfo[i].usename, NULL, NULL);
			dumpComment(fout, tblinfo[i].triggers[j].tgcomment, tblinfo[i].triggers[j].oid,
						"pg_trigger", 0, NULL);
		}
	}
}


static void
dumpRules(Archive *fout, const char *tablename,
		  TableInfo *tblinfo, int numTables)
{
	PGresult   *res;
	int			nrules;
	int			i,
				t;
	PQExpBuffer query = createPQExpBuffer();

	int			i_definition;
	int			i_oid;
	int			i_owner;
	int			i_rulename;

	if (g_verbose)
		write_msg(NULL, "dumping out rules\n");

	/*
	 * For each table we dump
	 */
	for (t = 0; t < numTables; t++)
	{
		if (tablename && (strcmp(tblinfo[t].relname, tablename) != 0) && (strlen(tablename) > 0))
			continue;

		/*
		 * Get all rules defined for this table We include pg_rules in the
		 * cross since it filters out all view rules (pjw 15-Sep-2000).
		 *
		 * XXXX: Use LOJ here
		 */
		resetPQExpBuffer(query);
		appendPQExpBuffer(query, "SELECT definition,"
						  "   (select usename from pg_user where pg_class.relowner = usesysid) AS viewowner, "
						  "   pg_rewrite.oid, pg_rewrite.rulename "
						  "FROM pg_rewrite, pg_class, pg_rules "
						  "WHERE pg_class.relname = ");
		formatStringLiteral(query, tblinfo[t].relname, CONV_ALL);
		appendPQExpBuffer(query,
						  "    AND pg_rewrite.ev_class = pg_class.oid "
						  "    AND pg_rules.tablename = pg_class.relname "
					   "    AND pg_rules.rulename = pg_rewrite.rulename "
						  "ORDER BY pg_rewrite.oid");
		res = PQexec(g_conn, query->data);
		if (!res ||
			PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			write_msg(NULL, "query to get rules associated with table \"%s\" failed: %s",
					  tblinfo[t].relname, PQerrorMessage(g_conn));
			exit_nicely();
		}

		nrules = PQntuples(res);
		i_definition = PQfnumber(res, "definition");
		i_owner = PQfnumber(res, "viewowner");
		i_oid = PQfnumber(res, "oid");
		i_rulename = PQfnumber(res, "rulename");

		/*
		 * Dump them out
		 */

		for (i = 0; i < nrules; i++)
		{
			ArchiveEntry(fout, PQgetvalue(res, i, i_oid), PQgetvalue(res, i, i_rulename),
						 "RULE", NULL, PQgetvalue(res, i, i_definition),
						 "", "", PQgetvalue(res, i, i_owner), NULL, NULL);

			/* Dump rule comments */

			resetPQExpBuffer(query);
			appendPQExpBuffer(query, "RULE %s", fmtId(PQgetvalue(res, i, i_rulename), force_quotes));
			dumpComment(fout, query->data, PQgetvalue(res, i, i_oid),
						"pg_rewrite", 0, NULL);

		}

		PQclear(res);
	}

	destroyPQExpBuffer(query);
}
