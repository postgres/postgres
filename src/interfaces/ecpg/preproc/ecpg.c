/* New main for ecpg, the PostgreSQL embedded SQL precompiler. */
/* (C) Michael Meskes <meskes@postgresql.org> Feb 5th, 1998 */
/* Placed under the same copyright as PostgresSQL */

#include <unistd.h>
#include "postgres.h"

#ifdef HAVE_GETOPT_H
#include "getopt.h"
#endif

#include "extern.h"

int		ret_value = OK, autocommit = 0;
struct _include_path *include_paths = NULL;
struct cursor *cur = NULL;
struct typedefs *types = NULL;
struct _defines *defines = NULL;

static void
usage(char *progname)
{
	fprintf(stderr, "ecpg - the postgresql preprocessor, version: %d.%d.%d\n", MAJOR_VERSION, MINOR_VERSION, PATCHLEVEL);
	fprintf(stderr, "Usage: %s: [-v] [-t] [-I include path] [ -o output file name] [-D define name] file1 [file2] ...\n", progname);
}

static void
add_include_path(char *path)
{
	struct _include_path *ip = include_paths;

	include_paths = mm_alloc(sizeof(struct _include_path));
	include_paths->path = path;
	include_paths->next = ip;
}

static void
add_preprocessor_define(char *define)
{
	struct _defines *pd = defines;

	defines = mm_alloc(sizeof(struct _defines));
	defines->old = strdup(define);
	defines->new = strdup("");
	defines->pertinent = true;
	defines->next = pd;
}

int
main(int argc, char *const argv[])
{
	int			fnr,
				c,
				verbose = false,
				out_option = 0;
	struct _include_path *ip;

	add_include_path("/usr/include");
	add_include_path(INCLUDE_PATH);
	add_include_path("/usr/local/include");
	add_include_path(".");

	while ((c = getopt(argc, argv, "vo:I:tD:")) != EOF)
	{
		switch (c)
		{
			case 'o':
#ifndef __CYGWIN32__
				yyout = fopen(optarg, "w");
#else
				yyout = fopen(optarg, "wb");
#endif
				if (yyout == NULL)
					perror(optarg);
				else
					out_option = 1;
				break;
			case 'I':
				add_include_path(optarg);
				break;
			case 't':
				autocommit = 1;
				break;
			case 'v':
				verbose = true;
				break;
			case 'D':
                		add_preprocessor_define(optarg);
		                break;
			default:
				usage(argv[0]);
				return ILLEGAL_OPTION;
		}
	}

	if (verbose)
	{
		fprintf(stderr, "ecpg - the postgresql preprocessor, version: %d.%d.%d\n", MAJOR_VERSION, MINOR_VERSION, PATCHLEVEL);
		fprintf(stderr, "exec sql include ... search starts here:\n");
		for (ip = include_paths; ip != NULL; ip = ip->next)
			fprintf(stderr, " %s\n", ip->path);
		fprintf(stderr, "End of search list.\n");
		return OK;
	}
	
	if (optind >= argc)			/* no files specified */
	{
		usage(argv[0]);
		return (ILLEGAL_OPTION);
	}
	else
	{
		/* after the options there must not be anything but filenames */
		for (fnr = optind; fnr < argc; fnr++)
		{
			char	   *output_filename = NULL,
					   *ptr2ext;

			input_filename = mm_alloc(strlen(argv[fnr]) + 5);

			strcpy(input_filename, argv[fnr]);

			/* take care of relative paths */
			ptr2ext = strrchr(input_filename, '/');
			ptr2ext = (ptr2ext ? strrchr(ptr2ext, '.') : strrchr(input_filename, '.'));
			
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

			if (out_option == 0) /* calculate the output name */
			{
				output_filename = strdup(input_filename);

				ptr2ext = strrchr(output_filename, '.');
				/* make extension = .c */
				ptr2ext[1] = 'c';
				ptr2ext[2] = '\0';

#ifndef __CYGWIN32__
				yyout = fopen(output_filename, "w");
#else
				yyout = fopen(output_filename, "wb");
#endif
				if (yyout == NULL)
				{
					perror(output_filename);
					free(output_filename);
					free(input_filename);
					continue;
				}
			}

#ifndef __CYGWIN32__
			yyin = fopen(input_filename, "r");
#else
			yyin = fopen(input_filename, "rb");
#endif
			if (yyin == NULL)
				perror(argv[fnr]);
			else
			{
				struct cursor *ptr;
				struct _defines *defptr;
				struct typedefs *typeptr;

				/* remove old cursor definitions if any are still there */
				for (ptr = cur; ptr != NULL;)
				{
					struct cursor *this = ptr;
					struct arguments *l1,
							   *l2;

					free(ptr->command);
					free(ptr->connection);
					free(ptr->name);
					for (l1 = ptr->argsinsert; l1; l1 = l2)
					{
						l2 = l1->next;
						free(l1);
					}
					for (l1 = ptr->argsresult; l1; l1 = l2)
					{
						l2 = l1->next;
						free(l1);
					}
					ptr = ptr->next;
					free(this);
				}
				cur = NULL;

				/* remove non-pertinent old defines as well */
				while ( defines && !defines->pertinent ) {
				    defptr = defines;
				    defines = defines->next;

				    free(defptr->new);
				    free(defptr->old);
				    free(defptr);
				}

				for (defptr = defines; defptr != NULL; defptr = defptr->next )
				{
				    struct _defines *this = defptr->next;
					
				    if ( this && !this->pertinent ) {
					defptr->next = this->next;

					free(this->new);
					free(this->old);
					free(this);
				    }
				}

				/* and old typedefs */
				for (typeptr = types; typeptr != NULL;)
				{
					struct typedefs *this = typeptr;

					free(typeptr->name);
					ECPGfree_struct_member(typeptr->struct_member_list);
					free(typeptr->type);
					typeptr = typeptr->next;
					free(this);
				}
				types = NULL;
				
				/* initialize whenever structures */
				memset(&when_error, 0, sizeof(struct when));
				memset(&when_nf, 0, sizeof(struct when));
				memset(&when_warn, 0, sizeof(struct when));
				
				/* and structure member lists */
				memset(struct_member_list, 0, sizeof(struct_member_list));
				/* initialize lex */
				lex_init();

				/* we need two includes */
				fprintf(yyout, "/* Processed by ecpg (%d.%d.%d) */\n/* These two include files are added by the preprocessor */\n#include <ecpgtype.h>\n#include <ecpglib.h>\n#line 1 \"%s\"\n", MAJOR_VERSION, MINOR_VERSION, PATCHLEVEL, input_filename);

				/* and parse the source */
				yyparse();

				if (yyin != NULL)
					fclose(yyin);
				if (out_option == 0)
					fclose(yyout);
			}

			if (output_filename)
				free(output_filename);

			free(input_filename);
		}
	}
	return ret_value;
}
