/*-------------------------------------------------------------------------
 *
 * pg_encoding.c
 *
 *
 * Copyright (c) 1998-2003, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/bin/pg_encoding/Attic/pg_encoding.c,v 1.13 2003/08/04 23:59:39 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "miscadmin.h"
#include "mb/pg_wchar.h"

#include <ctype.h>

static void usage(void);

int
main(int argc, char **argv)
{
	char	   *p;
	int			enc;
	bool		be_only = FALSE;

	if (argc < 2)
	{
		usage();
		exit(1);
	}

	if (strcmp(argv[1], "-b") == 0)
	{
		if (argc < 3)
		{
			usage();
			exit(1);
		}
		be_only = TRUE;
		p = argv[2];
	}
	else
		p = argv[1];

	if (p && *p && isdigit((unsigned char) *p))
	{
		/*
		 * Encoding number to name
		 */
		char	   *name;

		enc = atoi(p);

		if ((name = (char *) pg_encoding_to_char(enc)))
		{
			if (be_only && pg_valid_server_encoding(name) < 0)
				exit(1);

			/*
			 * pg_encoding_to_char() returns "" if invalid encoding number
			 * is given
			 */
			else if (strcmp("", name))
				printf("%s\n", name);
			else
				exit(1);
		}
		exit(0);
	}
	else if (p && *p)
	{
		/*
		 * Encoding name to encoding number
		 */
		if ((enc = pg_char_to_encoding(p)) >= 0)
		{
			if (be_only && pg_valid_server_encoding(p) < 0)
				exit(1);
			printf("%d\n", enc);
		}
		else
			exit(1);

		exit(0);
	}
	exit(1);
}

static void
usage()
{
	fprintf(stderr,
	 "\nUsage: pg_encoding [options] encoding_name | encoding_number\n\n"
			"options:"
		  "         -b        check if encoding is valid for backend\n\n"
		);
}
