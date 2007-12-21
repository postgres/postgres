/* $PostgreSQL: pgsql/src/interfaces/ecpg/preproc/ecpg.c,v 1.103 2007/12/21 14:33:20 meskes Exp $ */

/* New main for ecpg, the PostgreSQL embedded SQL precompiler. */
/* (C) Michael Meskes <meskes@postgresql.org> Feb 5th, 1998 */
/* Placed under the same license as PostgreSQL */

#include "postgres_fe.h"

#include <unistd.h>
#include <string.h>
#include "getopt_long.h"

#include "extern.h"

int			ret_value = 0,
			autocommit = false,
			auto_create_c = false,
			system_includes = false,
			force_indicator = true,
			questionmarks = false,
			regression_mode = false,
			auto_prepare = false;

char	   *output_filename;

enum COMPAT_MODE compat = ECPG_COMPAT_PGSQL;

struct _include_path *include_paths = NULL;
struct cursor *cur = NULL;
struct typedefs *types = NULL;
struct _defines *defines = NULL;

static void
help(const char *progname)
{
	printf("%s is the PostgreSQL embedded SQL preprocessor for C programs.\n\n",
		   progname);
	printf("Usage:\n"
		   "  %s [OPTION]... FILE...\n\n",
		   progname);
	printf("Options:\n");
	printf("  -c             automatically generate C code from embedded SQL code;\n"
		   "                 currently this works for EXEC SQL TYPE\n");
	printf("  -C MODE        set compatibility mode;\n"
	  "                 MODE can be one of \"INFORMIX\", \"INFORMIX_SE\"\n");
#ifdef YYDEBUG
	printf("  -d             generate parser debug output\n");
#endif
	printf("  -D SYMBOL      define SYMBOL\n");
	printf("  -h             parse a header file, this option includes option \"-c\"\n");
	printf("  -i             parse system include files as well\n");
	printf("  -I DIRECTORY   search DIRECTORY for include files\n");
	printf("  -o OUTFILE     write result to OUTFILE\n");
	printf("  -r OPTION      specify runtime behaviour;\n"
		   "                 OPTION can be:\n"
		   "                  \"no_indicator\"\n"
		   "                  \"prepare\"\n"
		   "                  \"questionmarks\"\n");
	printf("  -t             turn on autocommit of transactions\n");
	printf("  --help         show this help, then exit\n");
	printf("  --regression   run in regression testing mode\n");
	printf("  --version      output version information, then exit\n");
	printf("\nIf no output file is specified, the name is formed by adding .c to the\n"
		   "input file name, after stripping off .pgc if present.\n");
	printf("\nReport bugs to <pgsql-bugs@postgresql.org>.\n");
}

static void
add_include_path(char *path)
{
	struct _include_path *ip = include_paths,
			   *new;

	new = mm_alloc(sizeof(struct _include_path));
	new->path = path;
	new->next = NULL;

	if (ip == NULL)
		include_paths = new;
	else
	{
		for (; ip->next != NULL; ip = ip->next);
		ip->next = new;
	}
}

static void
add_preprocessor_define(char *define)
{
	struct _defines *pd = defines;
	char	   *ptr,
			   *define_copy = mm_strdup(define);

	defines = mm_alloc(sizeof(struct _defines));

	/* look for = sign */
	ptr = strchr(define_copy, '=');
	if (ptr != NULL)
	{
		char	   *tmp;

		/* symbol has a value */
		for (tmp = ptr - 1; *tmp == ' '; tmp--);
		tmp[1] = '\0';
		defines->old = define_copy;
		defines->new = ptr + 1;
	}
	else
	{
		defines->old = define_copy;
		defines->new = mm_strdup("1");
	}
	defines->pertinent = true;
	defines->used = NULL;
	defines->next = pd;
}

#define ECPG_GETOPT_LONG_HELP			1
#define ECPG_GETOPT_LONG_VERSION		2
#define ECPG_GETOPT_LONG_REGRESSION		3
int
main(int argc, char *const argv[])
{
	static struct option ecpg_options[] = {
		{"help", no_argument, NULL, ECPG_GETOPT_LONG_HELP},
		{"version", no_argument, NULL, ECPG_GETOPT_LONG_VERSION},
		{"regression", no_argument, NULL, ECPG_GETOPT_LONG_REGRESSION},
		{NULL, 0, NULL, 0}
	};

	int			fnr,
				c,
				verbose = false,
				header_mode = false,
				out_option = 0;
	struct _include_path *ip;
	const char *progname;
	char		my_exec_path[MAXPGPATH];
	char		include_path[MAXPGPATH];

	progname = get_progname(argv[0]);

	find_my_exec(argv[0], my_exec_path);

	output_filename = NULL;
	while ((c = getopt_long(argc, argv, "vcio:I:tD:dC:r:h?", ecpg_options, NULL)) != -1)
	{
		switch (c)
		{
			case ECPG_GETOPT_LONG_VERSION:
				printf("ecpg (PostgreSQL %s) %d.%d.%d\n", PG_VERSION,
					   MAJOR_VERSION, MINOR_VERSION, PATCHLEVEL);
				exit(0);
			case ECPG_GETOPT_LONG_HELP:
				help(progname);
				exit(0);

				/*
				 * -? is an alternative spelling of --help. However it is also
				 * returned by getopt_long for unknown options. We can
				 * distinguish both cases by means of the optopt variable
				 * which is set to 0 if it was really -? and not an unknown
				 * option character.
				 */
			case '?':
				if (optopt == 0)
				{
					help(progname);
					exit(0);
				}
				break;
			case ECPG_GETOPT_LONG_REGRESSION:
				regression_mode = true;
				break;
			case 'o':
				output_filename = optarg;
				if (strcmp(output_filename, "-") == 0)
					yyout = stdout;
				else
					yyout = fopen(output_filename, PG_BINARY_W);

				if (yyout == NULL)
				{
					fprintf(stderr, "%s: could not open file \"%s\": %s\n",
							progname, output_filename, strerror(errno));
					output_filename = NULL;
				}
				else
					out_option = 1;
				break;
			case 'I':
				add_include_path(optarg);
				break;
			case 't':
				autocommit = true;
				break;
			case 'v':
				verbose = true;
				break;
			case 'h':
				header_mode = true;
				/* this must include "-c" to make sense */
				/* so do not place a "break;" here */
			case 'c':
				auto_create_c = true;
				break;
			case 'i':
				system_includes = true;
				break;
			case 'C':
				if (strncmp(optarg, "INFORMIX", strlen("INFORMIX")) == 0)
				{
					char		pkginclude_path[MAXPGPATH];
					char		informix_path[MAXPGPATH];

					compat = (strcmp(optarg, "INFORMIX") == 0) ? ECPG_COMPAT_INFORMIX : ECPG_COMPAT_INFORMIX_SE;
					get_pkginclude_path(my_exec_path, pkginclude_path);
					snprintf(informix_path, MAXPGPATH, "%s/informix/esql", pkginclude_path);
					add_include_path(informix_path);
				}
				else
				{
					fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
					return ILLEGAL_OPTION;
				}
				break;
			case 'r':
				if (strcmp(optarg, "no_indicator") == 0)
					force_indicator = false;
				else if (strcmp(optarg, "prepare") == 0)
					auto_prepare = true;
				else if (strcmp(optarg, "questionmarks") == 0)
					questionmarks = true;
				else
				{
					fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
					return ILLEGAL_OPTION;
				}
				break;
			case 'D':
				add_preprocessor_define(optarg);
				break;
			case 'd':
#ifdef YYDEBUG
				yydebug = 1;
#else
				fprintf(stderr, "%s: parser debug support (-d) not available\n",
						progname);
#endif
				break;
			default:
				fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
				return ILLEGAL_OPTION;
		}
	}

	add_include_path(".");
	add_include_path("/usr/local/include");
	get_include_path(my_exec_path, include_path);
	add_include_path(include_path);
	add_include_path("/usr/include");

	if (verbose)
	{
		fprintf(stderr, "%s, the PostgreSQL embedded C preprocessor, version %d.%d.%d\n",
				progname, MAJOR_VERSION, MINOR_VERSION, PATCHLEVEL);
		fprintf(stderr, "exec sql include ... search starts here:\n");
		for (ip = include_paths; ip != NULL; ip = ip->next)
			fprintf(stderr, " %s\n", ip->path);
		fprintf(stderr, "end of search list\n");
		return 0;
	}

	if (optind >= argc)			/* no files specified */
	{
		fprintf(stderr, "%s: no input files specified\n", progname);
		fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
		return (ILLEGAL_OPTION);
	}
	else
	{
		/* after the options there must not be anything but filenames */
		for (fnr = optind; fnr < argc; fnr++)
		{
			char	   *ptr2ext;

			/* If argv[fnr] is "-" we have to read from stdin */
			if (strcmp(argv[fnr], "-") == 0)
			{
				input_filename = mm_alloc(strlen("stdin") + 1);
				strcpy(input_filename, "stdin");
				yyin = stdin;
			}
			else
			{
				input_filename = mm_alloc(strlen(argv[fnr]) + 5);
				strcpy(input_filename, argv[fnr]);

				/* take care of relative paths */
				ptr2ext = last_dir_separator(input_filename);
				ptr2ext = (ptr2ext ? strrchr(ptr2ext, '.') : strrchr(input_filename, '.'));

				/* no extension? */
				if (ptr2ext == NULL)
				{
					ptr2ext = input_filename + strlen(input_filename);

					/* no extension => add .pgc or .pgh */
					ptr2ext[0] = '.';
					ptr2ext[1] = 'p';
					ptr2ext[2] = 'g';
					ptr2ext[3] = (header_mode == true) ? 'h' : 'c';
					ptr2ext[4] = '\0';
				}

				yyin = fopen(input_filename, PG_BINARY_R);
			}

			if (out_option == 0)	/* calculate the output name */
			{
				if (strcmp(input_filename, "stdin") == 0)
					yyout = stdout;
				else
				{
					output_filename = strdup(input_filename);

					ptr2ext = strrchr(output_filename, '.');
					/* make extension = .c resp. .h */
					ptr2ext[1] = (header_mode == true) ? 'h' : 'c';
					ptr2ext[2] = '\0';

					yyout = fopen(output_filename, PG_BINARY_W);
					if (yyout == NULL)
					{
						fprintf(stderr, "%s: could not open file \"%s\": %s\n",
								progname, output_filename, strerror(errno));
						free(output_filename);
						free(input_filename);
						continue;
					}
				}
			}

			if (yyin == NULL)
				fprintf(stderr, "%s: could not open file \"%s\": %s\n",
						progname, argv[fnr], strerror(errno));
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
				while (defines && !defines->pertinent)
				{
					defptr = defines;
					defines = defines->next;

					free(defptr->new);
					free(defptr->old);
					free(defptr);
				}

				for (defptr = defines; defptr != NULL; defptr = defptr->next)
				{
					struct _defines *this = defptr->next;

					if (this && !this->pertinent)
					{
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

				/* and our variable counter for Informix compatibility */
				ecpg_informix_var = 0;

				/* finally the actual connection */
				connection = NULL;

				/* initialize lex */
				lex_init();

				/* we need several includes */
				/* but not if we are in header mode */
				if (regression_mode)
					fprintf(yyout, "/* Processed by ecpg (regression mode) */\n");
				else
					fprintf(yyout, "/* Processed by ecpg (%d.%d.%d) */\n", MAJOR_VERSION, MINOR_VERSION, PATCHLEVEL);

				if (header_mode == false)
				{
					fprintf(yyout, "/* These include files are added by the preprocessor */\n#include <ecpgtype.h>\n#include <ecpglib.h>\n#include <ecpgerrno.h>\n#include <sqlca.h>\n");

					/* add some compatibility headers */
					if (INFORMIX_MODE)
						fprintf(yyout, "/* Needed for informix compatibility */\n#include <ecpg_informix.h>\n");

					fprintf(yyout, "/* End of automatic include section */\n");
				}

				if (regression_mode)
					fprintf(yyout, "#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))\n");

				output_line_number();

				/* and parse the source */
				base_yyparse();

				/* check if all cursors were indeed opened */
				for (ptr = cur; ptr != NULL;)
				{
					char		errortext[128];

					if (!(ptr->opened))
					{
						/*
						 * Does not really make sense to declare a cursor but
						 * not open it
						 */
						snprintf(errortext, sizeof(errortext), "cursor \"%s\" has been declared but not opened\n", ptr->name);
						mmerror(PARSE_ERROR, ET_WARNING, errortext);
					}
					ptr = ptr->next;
				}

				if (yyin != NULL && yyin != stdin)
					fclose(yyin);
				if (out_option == 0 && yyout != stdout)
					fclose(yyout);
			}

			if (output_filename && out_option == 0)
				free(output_filename);

			free(input_filename);
		}
	}
	return ret_value;
}
