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
 *		  indices
 *		  aggregates
 *		  operators
 *		  ACL - grant/revoke
 *
 * the output script is SQL that is understood by PostgreSQL
 *
 * Basic process in a restore operation is:
 * 
 * 	Open the Archive and read the TOC.
 * 	Set flags in TOC entries, and *maybe* reorder them.
 * 	Generate script to stdout
 * 	Exit
 *
 * Copyright (c) 2000, Philip Warner
 *      Rights are granted to use this software in any way so long
 *      as this notice is not removed.
 *
 *	The author is not responsible for loss or damages that may
 *	result from it's use.
 *
 *
 * IDENTIFICATION
 *
 * Modifications - 28-Jun-2000 - pjw@rhyme.com.au
 *
 *	Initial version. Command processing taken from original pg_dump.
 *
 *-------------------------------------------------------------------------
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>


/*
#include "postgres.h"
#include "access/htup.h"
#include "catalog/pg_type.h"
#include "catalog/pg_language.h"
#include "catalog/pg_index.h"
#include "catalog/pg_trigger.h"
#include "libpq-fe.h"
*/

#include "pg_backup.h"

#ifndef HAVE_STRDUP
#include "strdup.h"
#endif

#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif

#ifdef HAVE_GETOPT_H 
#include <getopt.h>
#else
#include <unistd.h>
#endif

/* Forward decls */
static void usage(const char *progname);
static char* _cleanupName(char* name);

typedef struct option optType;

#ifdef HAVE_GETOPT_H
struct option cmdopts[] = {	
				{ "clean", 0, NULL, 'c' },
				{ "data-only", 0, NULL, 'a' },
				{ "file", 1, NULL, 'f' },
				{ "format", 1, NULL, 'F' },
				{ "function", 2, NULL, 'p' },
				{ "index", 2, NULL, 'i'},
				{ "list", 0, NULL, 'l'},
				{ "no-acl", 0, NULL, 'x' },
				{ "oid-order", 0, NULL, 'o'},
				{ "orig-order", 0, NULL, 'O' },
				{ "rearrange", 0, NULL, 'r'},
				{ "schema-only", 0, NULL, 's' },
				{ "table", 2, NULL, 't'},
				{ "trigger", 2, NULL, 'T' },
				{ "use-list", 1, NULL, 'u'},
				{ "verbose", 0, NULL, 'v' },
				{ NULL, 0, NULL, 0}
			    };
#endif

int main(int argc, char **argv)
{
	RestoreOptions	*opts;
	char		*progname;
	int		c;
	Archive*    	AH;
	char		*fileSpec;

	opts = NewRestoreOptions();

	progname = *argv;

#ifdef HAVE_GETOPT_LONG
	while ((c = getopt_long(argc, argv, "acf:F:i:loOp:st:T:u:vx", cmdopts, NULL)) != EOF)
#else
	while ((c = getopt(argc, argv, "acf:F:i:loOp:st:T:u:vx")) != -1)
#endif
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
			case 'f':			/* output file name */
				opts->filename = strdup(optarg);
				break;
			case 'F':
				if (strlen(optarg) != 0) 
				    opts->formatName = strdup(optarg);
				break;
			case 'o':
				opts->oidOrder = 1;
				break;
			case 'O':
				opts->origOrder = 1;
				break;
			case 'r':
				opts->rearrange = 1;
				break;

			case 'p': /* Function */
				opts->selTypes = 1;
				opts->selFunction = 1;
				opts->functionNames = _cleanupName(optarg);
				break;
			case 'i': /* Index */
				opts->selTypes = 1;
				opts->selIndex = 1;
				opts->indexNames = _cleanupName(optarg);
				break;
			case 'T': /* Trigger */
				opts->selTypes = 1;
				opts->selTrigger = 1;
				opts->triggerNames = _cleanupName(optarg);
				break;
			case 's':			/* dump schema only */
				opts->schemaOnly = 1;
				break;
			case 't':			/* Dump data for this table only */
				opts->selTypes = 1;
				opts->selTable = 1;
				opts->tableNames = _cleanupName(optarg);
				break;
			case 'l':			/* Dump the TOC summary */
				opts->tocSummary = 1;
				break;

			case 'u':			/* input TOC summary file name */
				opts->tocFile = strdup(optarg);
				break;

			case 'v':			/* verbose */
				opts->verbose = 1;
				break;
			case 'x':			/* skip ACL dump */
				opts->aclsSkip = 1;
				break;
			default:
				usage(progname);
				break;
		}
	}

	if (optind < argc) {
	    fileSpec = argv[optind];
	} else {
	    fileSpec = NULL;
	}

    if (opts->formatName) { 

	switch (opts->formatName[0]) {

	    case 'c':
	    case 'C':
		opts->format = archCustom;
		break;

	    case 'f':
	    case 'F':
		opts->format = archFiles;
		break;

	    default:
		fprintf(stderr, "%s: Unknown archive format '%s', please specify 'f' or 'c'\n", progname, opts->formatName);
		exit (1);
	}
    }

    AH = OpenArchive(fileSpec, opts->format);

    if (opts->tocFile)
	SortTocFromFile(AH, opts);

    if (opts->oidOrder)
	SortTocByOID(AH);
    else if (opts->origOrder)
	SortTocByID(AH);

    if (opts->rearrange) {
	MoveToEnd(AH, "TABLE DATA");
	MoveToEnd(AH, "INDEX");
	MoveToEnd(AH, "TRIGGER");
	MoveToEnd(AH, "RULE");
	MoveToEnd(AH, "ACL");
    }

    if (opts->tocSummary) {
	PrintTOCSummary(AH, opts);
    } else {
	RestoreArchive(AH, opts);
    }

    CloseArchive(AH);

    return 1;
}

static void usage(const char *progname)
{
#ifdef HAVE_GETOPT_LONG
	fprintf(stderr,
	"usage:  %s [options] [backup file]\n"
	    "  -a, --data-only             \t dump out only the data, no schema\n"
	    "  -c, --clean                 \t clean(drop) schema prior to create\n"
	    "  -f filename                 \t script output filename\n"
	    "  -F, --format {c|f}          \t specify backup file format\n"
	    "  -p, --function[=name]       \t dump functions or named function\n"
	    "  -i, --index[=name]          \t dump indexes or named index\n"
	    "  -l, --list                  \t dump summarized TOC for this file\n"
	    "  -o, --oid-order             \t dump in oid order\n"
	    "  -O, --orig-order            \t dump in original dump order\n"
	    "  -r, --rearrange             \t rearrange output to put indexes etc at end\n"
	    "  -s, --schema-only           \t dump out only the schema, no data\n"
	    "  -t [table], --table[=table] \t dump for this table only\n"
	    "  -T, --trigger[=name]        \t dump triggers or named trigger\n"
	    "  -u, --use-list filename     \t use specified TOC for ordering output from this file\n"
	    "  -v                          \t verbose\n"
	    "  -x, --no-acl                \t skip dumping of ACLs (grant/revoke)\n"
	    , progname);
#else
	fprintf(stderr,
	"usage:  %s [options] [backup file]\n"
	    "  -a                          \t dump out only the data, no schema\n"
	    "  -c                          \t clean(drop) schema prior to create\n"
	    "  -f filename NOT IMPLEMENTED \t script output filename\n"
	    "  -F           {c|f}          \t specify backup file format\n"
	    "  -p name                     \t dump functions or named function\n"
	    "  -i name                     \t dump indexes or named index\n"
	    "  -l                          \t dump summarized TOC for this file\n"
	    "  -o                          \t dump in oid order\n"
	    "  -O                          \t dump in original dump order\n"
	    "  -r                          \t rearrange output to put indexes etc at end\n"
	    "  -s                          \t dump out only the schema, no data\n"
	    "  -t name                     \t dump for this table only\n"
	    "  -T name                     \t dump triggers or named trigger\n"
	    "  -u filename                 \t use specified TOC for ordering output from this file\n"
	    "  -v                          \t verbose\n"
	    "  -x                          \t skip dumping of ACLs (grant/revoke)\n"
	    , progname);
#endif
	fprintf(stderr,
			"\nIf [backup file] is not supplied, then standard input "
			"is used.\n");
	fprintf(stderr, "\n");

	exit(1);
}

static char* _cleanupName(char* name)
{
    int		i;

    if (!name)
	return NULL;

    if (strlen(name) == 0)
	return NULL;

    name = strdup(name);

    if (name[0] == '"')
    {
	strcpy(name, &name[1]);
	if (*(name + strlen(name) - 1) == '"')
	    *(name + strlen(name) - 1) = '\0';
    }
    /* otherwise, convert table name to lowercase... */
    else
    {
	for (i = 0; name[i]; i++)
	    if (isascii((unsigned char) name[i]) && isupper(name[i]))
		name[i] = tolower(name[i]);
    }
    return name;
}

