/* New main for ecpg, the PostgreSQL embedded SQL precompiler. */
/* (C) Michael Meskes <meskes@debian.org> Feb 5th, 1998 */
/* Placed under the same copyright as PostgresSQL */

#include "postgres.h"

#include <stdio.h>
#if HAVE_GETOPT_H
#include <getopt.h>
#else
#include <unistd.h>
extern int optind;
extern char *optarg;
#endif
#include <stdlib.h>
#if defined(HAVE_STRING_H)
#include <string.h>
#else
#include <strings.h>
#endif

#include "extern.h"

struct _include_path *include_paths;
 
static void
usage(char *progname)
{
	fprintf(stderr, "ecpg - the postgresql preprocessor, version: %d.%d.%d\n", MAJOR_VERSION, MINOR_VERSION, PATCHLEVEL);
	fprintf(stderr, "Usage: %s: [-v] [-d] [-I include path] [ -o output file name] file1 [file2] ...\n", progname);
}

static void
add_include_path(char * path)
{
	struct _include_path *ip = include_paths;
	
        include_paths = mm_alloc(sizeof(struct _include_path));
        include_paths->path = path;
        include_paths->next = ip;
}

int
main(int argc, char *const argv[])
{
	int			fnr, c, out_option = 0;
	struct _include_path	*ip;
	
	add_include_path("/usr/include");	
	add_include_path(INCLUDE_PATH);
	add_include_path("/usr/local/include");
	add_include_path(".");

	while ((c = getopt(argc, argv, "vdo:I:")) != EOF)
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
			case 'I':
				add_include_path(optarg);
		                break;
			case 'v':
				fprintf(stderr, "ecpg - the postgresql preprocessor, version: %d.%d.%d\n", MAJOR_VERSION, MINOR_VERSION, PATCHLEVEL);
				fprintf(stderr, "exec sql include ... search starts here:\n");
				for (ip = include_paths; ip != NULL; ip = ip->next)
					fprintf(stderr, " %s\n", ip->path);
				fprintf(stderr, "End of search list.\n");
				return (0);
			default:
				usage(argv[0]);
				return (1);
		}
	}

	if (optind >= argc)			/* no files specified */
	{
		usage(argv[0]);
		return(1);
	}
	else
	{
		/* after the options there must not be anything but filenames */
		for (fnr = optind; fnr < argc; fnr++)
		{
			char	*ptr2ext;

			input_filename = mm_alloc(strlen(argv[fnr]) + 5);

			strcpy(input_filename, argv[fnr]);

			ptr2ext = strrchr(input_filename, '.');
			/* no extension? */
			if (ptr2ext == NULL)
			{
				ptr2ext = input_filename + strlen(input_filename);
				
				/* no extension => add .pgc */
				ptr2ext[0] = '.';
				ptr2ext[1] = 'p';
				ptr2ext[2] = 'g';
				ptr2ext[3] = 'c';
				ptr2ext[4] = '\0';
			}

			if (out_option == 0)/* calculate the output name */
			{
				char *output_filename = strdup(input_filename);
				
				ptr2ext = strrchr(output_filename, '.');
				/* make extension = .c */
				ptr2ext[1] = 'c';
				ptr2ext[2] = '\0';
				
				yyout = fopen(output_filename, "w");
				if (yyout == NULL)
				{
					perror(output_filename);
					free(output_filename);
					free(input_filename);
					continue;
				}
				free(output_filename);
			}

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

				if (yyin != NULL)
					fclose(yyin);
				if (out_option == 0)
					fclose(yyout);
			}
			free(input_filename);
		}
	}
	return (0);
}
