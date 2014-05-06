/*
 * entab.c - adds/removes tabs from text files
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

#if defined(WIN32) || defined(__CYGWIN__)
#define PG_BINARY_R "rb"
#else
#define PG_BINARY_R "r"
#endif

#define NUL		'\0'

#ifndef TRUE
#define TRUE	1
#endif
#ifndef FALSE
#define FALSE	0
#endif

extern char *optarg;
extern int	optind;


static void
output_accumulated_spaces(int *prv_spaces, char **dst)
{
	for (; *prv_spaces > 0; (*prv_spaces)--)
		*((*dst)++) = ' ';
}


static void
trim_trailing_whitespace(int *prv_spaces, char **dst, char *out_line)
{
	while (*dst > out_line &&
		   (*((*dst) - 1) == ' ' || *((*dst) - 1) == '\t'))
		(*dst)--;
	*prv_spaces = 0;
}


int
main(int argc, char **argv)
{
	int			tab_size = 8,
				min_spaces = 2,
				only_comment_periods = FALSE,
				protect_quotes = FALSE,
				protect_leading_whitespace = FALSE,
				del_tabs = FALSE,
				clip_lines = FALSE,
				in_comment = FALSE,
				was_period = FALSE,
				prv_spaces,
				col_in_tab,
				escaped,
				nxt_spaces,
				in_leading_whitespace;
	char		in_line[BUFSIZ],
				out_line[BUFSIZ],
			   *src,
			   *dst,
				quote_char,
			   *cp;
	int			ch;
	FILE	   *in_file;

	if ((cp = strrchr(argv[0], '/')) != NULL)
		++cp;
	else
		cp = argv[0];
	if (strcmp(cp, "detab") == 0)
		del_tabs = 1;

	while ((ch = getopt(argc, argv, "cdhlmqs:t:")) != -1)
		switch (ch)
		{
			case 'c':
				clip_lines = TRUE;
				break;
			case 'd':
				del_tabs = TRUE;
				break;
			case 'l':
				protect_leading_whitespace = TRUE;
				break;
			case 'm':
				/* only process text followed by periods in C comments */
				only_comment_periods = TRUE;
				break;
			case 'q':
				protect_quotes = TRUE;
				break;
			case 's':
				min_spaces = atoi(optarg);
				break;
			case 't':
				tab_size = atoi(optarg);
				break;
			case 'h':
			case '?':
				fprintf(stderr, "USAGE: %s [ -cdqst ] [file ...]\n\
	-c (clip trailing whitespace)\n\
	-d (delete tabs)\n\
	-l (protect leading whitespace)\n\
	-m (only C comment periods)\n\
	-q (protect quotes)\n\
	-s minimum_spaces\n\
	-t tab_width\n",
						cp);
				exit(0);
		}

	argv += optind;
	argc -= optind;

	/* process arguments */
	do
	{
		if (argc < 1)
			in_file = stdin;
		else
		{
			if ((in_file = fopen(*argv, PG_BINARY_R)) == NULL)
			{
				fprintf(stderr, "Cannot open file %s: %s\n", argv[0], strerror(errno));
				exit(1);
			}
			argv++;
		}

		escaped = FALSE;

		/* process lines */
		while (fgets(in_line, sizeof(in_line), in_file) != NULL)
		{
			col_in_tab = 0;
			prv_spaces = 0;
			src = in_line;		/* points to current processed char */
			dst = out_line;		/* points to next unallocated char */
			if (escaped == FALSE)
				quote_char = ' ';
			escaped = FALSE;
			in_leading_whitespace = TRUE;

			/* process line */
			while (*src != NUL)
			{
				col_in_tab++;

				/* look backward so we handle slash-star-slash properly */
				if (!in_comment && src > in_line &&
					*(src - 1) == '/' && *src == '*')
					in_comment = TRUE;
				else if (in_comment && *src == '*' && *(src + 1) == '/')
					in_comment = FALSE;

				/* Is this a potential space/tab replacement? */
				if ((!only_comment_periods || (in_comment && was_period)) &&
					(!protect_leading_whitespace || !in_leading_whitespace) &&
					quote_char == ' ' && (*src == ' ' || *src == '\t'))
				{
					if (*src == '\t')
					{
						prv_spaces += tab_size - col_in_tab + 1;
						col_in_tab = tab_size;
					}
					else
						prv_spaces++;

					/* Are we at a tab stop? */
					if (col_in_tab == tab_size)
					{
						/*
						 * Is the next character going to be a tab?  We do tab
						 * replacement in the current spot if the next char is
						 * going to be a tab and ignore min_spaces.
						 */
						nxt_spaces = 0;
						while (1)
						{
							/* Have we reached non-whitespace? */
							if (*(src + nxt_spaces + 1) == NUL ||
								(*(src + nxt_spaces + 1) != ' ' &&
								 *(src + nxt_spaces + 1) != '\t'))
								break;
							/* count spaces */
							if (*(src + nxt_spaces + 1) == ' ')
								++nxt_spaces;
							/* Have we found a forward tab? */
							if (*(src + nxt_spaces + 1) == '\t' ||
								nxt_spaces == tab_size)
							{
								nxt_spaces = tab_size;
								break;
							}
						}
						/* Do tab replacment for spaces? */
						if ((prv_spaces >= min_spaces ||
							 nxt_spaces == tab_size) &&
							del_tabs == FALSE)
						{
							*(dst++) = '\t';
							prv_spaces = 0;
						}
						else
							output_accumulated_spaces(&prv_spaces, &dst);
					}
				}
				/* Not a potential space/tab replacement */
				else
				{
					/* allow leading stars in comments */
					if (in_leading_whitespace && *src != ' ' && *src != '\t' &&
						(!in_comment || *src != '*'))
						in_leading_whitespace = FALSE;
					was_period = (*src == '.');
					/* output accumulated spaces */
					output_accumulated_spaces(&prv_spaces, &dst);
					/* This can only happen in a quote. */
					if (*src == '\t')
						col_in_tab = 0;
					/* visual backspace? */
					if (*src == '\b')
						col_in_tab -= 2;
					/* Do we process quotes? */
					if (escaped == FALSE && protect_quotes == TRUE)
					{
						if (*src == '\\')
							escaped = TRUE;
						/* Is this a quote character? */
						if (*src == '"' || *src == '\'')
						{
							/* toggle quote mode */
							if (quote_char == ' ')
								quote_char = *src;
							else if (*src == quote_char)
								quote_char = ' ';
						}
					}
					/* newlines/CRs do not terminate escapes */
					else if (*src != '\r' && *src != '\n')
						escaped = FALSE;

					/* reached newline/CR;	clip line? */
					if ((*src == '\r' || *src == '\n') &&
						clip_lines == TRUE &&
						quote_char == ' ' &&
						escaped == FALSE)
						trim_trailing_whitespace(&prv_spaces, &dst, out_line);
					*(dst++) = *src;
				}
				col_in_tab %= tab_size;
				++src;
			}
			/* for cases where the last line of file has no newline */
			if (clip_lines == TRUE && escaped == FALSE)
				trim_trailing_whitespace(&prv_spaces, &dst, out_line);
			output_accumulated_spaces(&prv_spaces, &dst);
			*dst = NUL;

			if (fputs(out_line, stdout) == EOF)
			{
				fprintf(stderr, "Cannot write to output file %s: %s\n", argv[0], strerror(errno));
				exit(1);
			}
		}
	} while (--argc > 0);
	return 0;
}
