/* src/interfaces/ecpg/preproc/ecpg.c */

/* Main for ecpg, the PostgreSQL embedded SQL precompiler. */
/* Copyright (c) 1996-2024, PostgreSQL Global Development Group */

#include "postgres_fe.h"

#include <unistd.h>

#include "getopt_long.h"

#include "preproc_extern.h"

int			ret_value = 0;
bool		autocommit = false,
			auto_create_c = false,
			system_includes = false,
			force_indicator = true,
			questionmarks = false,
			regression_mode = false,
			auto_prepare = false;

static const char *progname;
char	   *output_filename;

enum COMPAT_MODE compat = ECPG_COMPAT_PGSQL;

struct _include_path *include_paths = NULL;
struct cursor *cur = NULL;
struct typedefs *types = NULL;
struct _defines *defines = NULL;
struct declared_list *g_declared_list = NULL;

static void
help(const char *progname)
{
	printf(_("%s is the PostgreSQL embedded SQL preprocessor for C programs.\n\n"),
		   progname);
	printf(_("Usage:\n"
			 "  %s [OPTION]... FILE...\n\n"),
		   progname);
	printf(_("Options:\n"));
	printf(_("  -c             automatically generate C code from embedded SQL code;\n"
			 "                 this affects EXEC SQL TYPE\n"));
	printf(_("  -C MODE        set compatibility mode; MODE can be one of\n"
			 "                 \"INFORMIX\", \"INFORMIX_SE\", \"ORACLE\"\n"));
#ifdef YYDEBUG
	printf(_("  -d             generate parser debug output\n"));
#endif
	printf(_("  -D SYMBOL      define SYMBOL\n"));
	printf(_("  -h             parse a header file, this option includes option \"-c\"\n"));
	printf(_("  -i             parse system include files as well\n"));
	printf(_("  -I DIRECTORY   search DIRECTORY for include files\n"));
	printf(_("  -o OUTFILE     write result to OUTFILE\n"));
	printf(_("  -r OPTION      specify run-time behavior; OPTION can be:\n"
			 "                 \"no_indicator\", \"prepare\", \"questionmarks\"\n"));
	printf(_("  --regression   run in regression testing mode\n"));
	printf(_("  -t             turn on autocommit of transactions\n"));
	printf(_("  -V, --version  output version information, then exit\n"));
	printf(_("  -?, --help     show this help, then exit\n"));
	printf(_("\nIf no output file is specified, the name is formed by adding .c to the\n"
			 "input file name, after stripping off .pgc if present.\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
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

/*
 * Process a command line -D switch
 */
static void
add_preprocessor_define(char *define)
{
	/* copy the argument to avoid relying on argv storage */
	char	   *define_copy = mm_strdup(define);
	char	   *ptr;
	struct _defines *newdef;

	newdef = mm_alloc(sizeof(struct _defines));

	/* look for = sign */
	ptr = strchr(define_copy, '=');
	if (ptr != NULL)
	{
		/* symbol has a value */
		char	   *tmp;

		/* strip any spaces between name and '=' */
		for (tmp = ptr - 1; tmp >= define_copy && *tmp == ' '; tmp--);
		tmp[1] = '\0';

		/*
		 * Note we don't bother to separately malloc cmdvalue; it will never
		 * be freed so that's not necessary.
		 */
		newdef->cmdvalue = ptr + 1;
	}
	else
	{
		/* define it as "1"; again no need to malloc it */
		newdef->cmdvalue = "1";
	}
	newdef->name = define_copy;
	newdef->value = mm_strdup(newdef->cmdvalue);
	newdef->used = NULL;
	newdef->next = defines;
	defines = newdef;
}

#define ECPG_GETOPT_LONG_REGRESSION		1
int
main(int argc, char *const argv[])
{
	static struct option ecpg_options[] = {
		{"regression", no_argument, NULL, ECPG_GETOPT_LONG_REGRESSION},
		{NULL, 0, NULL, 0}
	};

	int			fnr,
				c,
				out_option = 0;
	bool		verbose = false,
				header_mode = false;
	struct _include_path *ip;
	char		my_exec_path[MAXPGPATH];
	char		include_path[MAXPGPATH];

	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("ecpg"));

	progname = get_progname(argv[0]);

	if (find_my_exec(argv[0], my_exec_path) < 0)
	{
		fprintf(stderr, _("%s: could not locate my own executable path\n"), argv[0]);
		return ILLEGAL_OPTION;
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
			printf("ecpg (PostgreSQL) %s\n", PG_VERSION);
			exit(0);
		}
	}

	output_filename = NULL;
	while ((c = getopt_long(argc, argv, "cC:dD:hiI:o:r:tv", ecpg_options, NULL)) != -1)
	{
		switch (c)
		{
			case 'c':
				auto_create_c = true;
				break;
			case 'C':
				if (pg_strcasecmp(optarg, "INFORMIX") == 0 || pg_strcasecmp(optarg, "INFORMIX_SE") == 0)
				{
					char		pkginclude_path[MAXPGPATH];
					char		informix_path[MAXPGPATH];

					compat = (pg_strcasecmp(optarg, "INFORMIX") == 0) ? ECPG_COMPAT_INFORMIX : ECPG_COMPAT_INFORMIX_SE;
					get_pkginclude_path(my_exec_path, pkginclude_path);
					snprintf(informix_path, MAXPGPATH, "%s/informix/esql", pkginclude_path);
					add_include_path(informix_path);
				}
				else if (pg_strcasecmp(optarg, "ORACLE") == 0)
				{
					compat = ECPG_COMPAT_ORACLE;
				}
				else
				{
					fprintf(stderr, _("Try \"%s --help\" for more information.\n"), argv[0]);
					return ILLEGAL_OPTION;
				}
				break;
			case 'd':
#ifdef YYDEBUG
				base_yydebug = 1;
#else
				fprintf(stderr, _("%s: parser debug support (-d) not available\n"),
						progname);
#endif
				break;
			case 'D':
				add_preprocessor_define(optarg);
				break;
			case 'h':
				header_mode = true;
				/* this must include "-c" to make sense: */
				auto_create_c = true;
				break;
			case 'i':
				system_includes = true;
				break;
			case 'I':
				add_include_path(optarg);
				break;
			case 'o':
				output_filename = mm_strdup(optarg);
				if (strcmp(output_filename, "-") == 0)
					base_yyout = stdout;
				else
					base_yyout = fopen(output_filename, PG_BINARY_W);

				if (base_yyout == NULL)
				{
					fprintf(stderr, _("%s: could not open file \"%s\": %m\n"),
							progname, output_filename);
					output_filename = NULL;
				}
				else
					out_option = 1;
				break;
			case 'r':
				if (pg_strcasecmp(optarg, "no_indicator") == 0)
					force_indicator = false;
				else if (pg_strcasecmp(optarg, "prepare") == 0)
					auto_prepare = true;
				else if (pg_strcasecmp(optarg, "questionmarks") == 0)
					questionmarks = true;
				else
				{
					fprintf(stderr, _("Try \"%s --help\" for more information.\n"), argv[0]);
					return ILLEGAL_OPTION;
				}
				break;
			case 't':
				autocommit = true;
				break;
			case 'v':
				verbose = true;
				break;
			case ECPG_GETOPT_LONG_REGRESSION:
				regression_mode = true;
				break;
			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"), argv[0]);
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
		fprintf(stderr,
				_("%s, the PostgreSQL embedded C preprocessor, version %s\n"),
				progname, PG_VERSION);
		fprintf(stderr, _("EXEC SQL INCLUDE ... search starts here:\n"));
		for (ip = include_paths; ip != NULL; ip = ip->next)
			fprintf(stderr, " %s\n", ip->path);
		fprintf(stderr, _("end of search list\n"));
		return 0;
	}

	if (optind >= argc)			/* no files specified */
	{
		fprintf(stderr, _("%s: no input files specified\n"), progname);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"), argv[0]);
		return ILLEGAL_OPTION;
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
				base_yyin = stdin;
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

				base_yyin = fopen(input_filename, PG_BINARY_R);
			}

			if (out_option == 0)	/* calculate the output name */
			{
				if (strcmp(input_filename, "stdin") == 0)
					base_yyout = stdout;
				else
				{
					output_filename = mm_alloc(strlen(input_filename) + 3);
					strcpy(output_filename, input_filename);

					ptr2ext = strrchr(output_filename, '.');
					/* make extension = .c resp. .h */
					ptr2ext[1] = (header_mode == true) ? 'h' : 'c';
					ptr2ext[2] = '\0';

					base_yyout = fopen(output_filename, PG_BINARY_W);
					if (base_yyout == NULL)
					{
						fprintf(stderr, _("%s: could not open file \"%s\": %m\n"),
								progname, output_filename);
						free(output_filename);
						output_filename = NULL;
						free(input_filename);
						continue;
					}
				}
			}

			if (base_yyin == NULL)
				fprintf(stderr, _("%s: could not open file \"%s\": %m\n"),
						progname, argv[fnr]);
			else
			{
				struct cursor *ptr;
				struct _defines *defptr;
				struct _defines *prevdefptr;
				struct _defines *nextdefptr;
				struct typedefs *typeptr;
				struct declared_list *list;

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

				/* remove old declared statements if any are still there */
				for (list = g_declared_list; list != NULL;)
				{
					struct declared_list *this = list;

					list = list->next;
					free(this);
				}

				/* restore defines to their command-line state */
				prevdefptr = NULL;
				for (defptr = defines; defptr != NULL; defptr = nextdefptr)
				{
					nextdefptr = defptr->next;
					if (defptr->cmdvalue != NULL)
					{
						/* keep it, resetting the value */
						free(defptr->value);
						defptr->value = mm_strdup(defptr->cmdvalue);
						prevdefptr = defptr;
					}
					else
					{
						/* remove it */
						if (prevdefptr != NULL)
							prevdefptr->next = nextdefptr;
						else
							defines = nextdefptr;
						free(defptr->name);
						free(defptr->value);
						free(defptr);
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

				/*
				 * and our variable counter for out of scope cursors'
				 * variables
				 */
				ecpg_internal_var = 0;

				/* finally the actual connection */
				connection = NULL;

				/* initialize lex */
				lex_init();

				/* we need several includes */
				/* but not if we are in header mode */
				if (regression_mode)
					fprintf(base_yyout, "/* Processed by ecpg (regression mode) */\n");
				else
					fprintf(base_yyout, "/* Processed by ecpg (%s) */\n", PG_VERSION);

				if (header_mode == false)
				{
					fprintf(base_yyout, "/* These include files are added by the preprocessor */\n#include <ecpglib.h>\n#include <ecpgerrno.h>\n#include <sqlca.h>\n");

					/* add some compatibility headers */
					if (INFORMIX_MODE)
						fprintf(base_yyout, "/* Needed for informix compatibility */\n#include <ecpg_informix.h>\n");

					fprintf(base_yyout, "/* End of automatic include section */\n");
				}

				if (regression_mode)
					fprintf(base_yyout, "#define ECPGdebug(X,Y) ECPGdebug((X)+100,(Y))\n");

				output_line_number();

				/* and parse the source */
				base_yyparse();

				/*
				 * Check whether all cursors were indeed opened.  It does not
				 * really make sense to declare a cursor but not open it.
				 */
				for (ptr = cur; ptr != NULL; ptr = ptr->next)
					if (!(ptr->opened))
						mmerror(PARSE_ERROR, ET_WARNING, "cursor \"%s\" has been declared but not opened", ptr->name);

				if (base_yyin != NULL && base_yyin != stdin)
					fclose(base_yyin);
				if (out_option == 0 && base_yyout != stdout)
					fclose(base_yyout);

				/*
				 * If there was an error, delete the output file.
				 */
				if (ret_value != 0)
				{
					if (strcmp(output_filename, "-") != 0 && unlink(output_filename) != 0)
						fprintf(stderr, _("could not remove output file \"%s\"\n"), output_filename);
				}
			}

			if (output_filename && out_option == 0)
			{
				free(output_filename);
				output_filename = NULL;
			}

			free(input_filename);
		}
	}
	return ret_value;
}
