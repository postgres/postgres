/* New main for ecpg, the PostgreSQL embedded SQL precompiler. */
/* (C) Michael Meskes <meskes@debian.org> Feb 5th, 1998 */
/* Placed under the same copyright as PostgresSQL */

#include <stdio.h>
#if HAVE_GETOPT_H
# include <getopt.h>
#else
# include <unistd.h>
#endif
#include <stdlib.h>
#include <strings.h>

#include "extern.h"

static void
usage(char *progname)
{
	fprintf(stderr, "ecpg - the postgresql preprocessor, version: %d.%d.%d\n", MAJOR_VERSION, MINOR_VERSION, PATCHLEVEL);
	fprintf(stderr, "Usage: %s: [-v] [-d] [ -o outout file name] file1 [file2] ...\n", progname);
}

int
main(int argc, char *const argv[])
{
	char			c, out_option = 0;
	int			fnr;

	while ((c = getopt(argc, argv, "vdo:")) != EOF)
	{
		switch (c)
		{
			case 'o':
				yyout = fopen(optarg, "w");
				if (yyout == NULL)
					perror(optarg);
				else
					out_option = 1;
				break;
			case 'd':
				debugging = 1;
				break;
			case 'v':
			default:
				usage(argv[0]);
		}
	}

	if (optind >= argc) /* no files specified */
		usage(argv[0]);
	else
	{
		/* after the options there must not be anything but filenames */
		for (fnr = optind; fnr < argc; fnr++)
		{
			char	   *filename, *ptr2ext;
			int	   ext = 0;

			filename = mm_alloc(strlen(argv[fnr]) + 4);

			strcpy(filename, argv[fnr]);

			ptr2ext = strrchr(filename, '.');
			/* no extension or extension not equal .pgc */
			if (ptr2ext == NULL || strcmp(ptr2ext, ".pgc") != 0)
			{ 
				if (ptr2ext == NULL)
					ext = 1; /* we need this information a while later */
				ptr2ext = filename + strlen(filename);
				ptr2ext[0] = '.';
			}

			/* make extension = .c */
			ptr2ext[1] = 'c';
			ptr2ext[2] = '\0';

			if (out_option == 0)	/* calculate the output name */
			{
				yyout = fopen(filename, "w");
				if (yyout == NULL)
				{
					perror(filename);
					free(filename);
					continue;
				}
			}

			if (ext == 1) 
			{
				/* no extension => add .pgc */
				ptr2ext = strrchr(filename, '.');
				ptr2ext[1] = 'p';
				ptr2ext[2] = 'g';
				ptr2ext[3] = 'c';
				ptr2ext[4] = '\0';
				input_filename = filename;
			}
			else
				input_filename = argv[fnr];
			yyin = fopen(input_filename, "r");
			if (yyin == NULL)
				perror(argv[fnr]);
			else
			{
				/* initialize lex */
				lex_init();

				/* we need two includes */
				fprintf(yyout, "/* Processed by ecpg (%d.%d.%d) */\n/*These two include files are added by the preprocessor */\n#include <ecpgtype.h>\n#include <ecpglib.h>\n", MAJOR_VERSION, MINOR_VERSION, PATCHLEVEL);

				/* and parse the source */
				yyparse();

				fclose(yyin);
				if (out_option == 0)
					fclose(yyout);
			}

			free(filename);
		}
	}
	return (0);
}
