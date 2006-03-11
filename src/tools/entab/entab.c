/*
**		entab.c			- add tabs to a text file
**		by Bruce Momjian (root@candle.pha.pa.us)
**
** $PostgreSQL: pgsql/src/tools/entab/entab.c,v 1.16 2006/03/11 04:38:41 momjian Exp $
**
**	version 1.3
**
**		tabsize = 4
**
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#if defined(WIN32) || defined(__CYGWIN__)
#define PG_BINARY_R "rb"
#else
#define PG_BINARY_R "r"
#endif

#define NUL				'\0'

#ifndef TRUE
#define TRUE	1
#endif
#ifndef FALSE
#define FALSE	0
#endif

void		halt();

extern char *optarg;
extern int	optind;

int
main(int argc, char **argv)
{
	int			tab_size = 8,
				min_spaces = 2,
				protect_quotes = FALSE,
				del_tabs = FALSE,
				clip_lines = FALSE,
				prv_spaces,
				col_in_tab,
				escaped,
				nxt_spaces;
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

	while ((ch = getopt(argc, argv, "cdhqs:t:")) != -1)
		switch (ch)
		{
			case 'c':
				clip_lines = TRUE;
				break;
			case 'd':
				del_tabs = TRUE;
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
				halt("USAGE: %s [ -cdqst ] [file ...]\n\
	-c (clip trailing whitespace)\n\
	-d (delete tabs)\n\
	-q (protect quotes)\n\
	-s minimum_spaces\n\
	-t tab_width\n",
					 cp);
		}

	argv += optind;
	argc -= optind;

	do
	{
		if (argc < 1)
			in_file = stdin;
		else
		{
			if ((in_file = fopen(*argv, PG_BINARY_R)) == NULL)
				halt("PERROR:  Can not open file %s\n", argv[0]);
			argv++;
		}

		escaped = FALSE;

		while (fgets(in_line, BUFSIZ, in_file) != NULL)
		{
			col_in_tab = 0;
			prv_spaces = 0;
			src = in_line;		/* points to current processed char */
			dst = out_line;		/* points to next unallocated char */
			if (escaped == FALSE)
				quote_char = ' ';
			escaped = FALSE;

			while (*src != NUL)
			{
				col_in_tab++;
				if (quote_char == ' ' && (*src == ' ' || *src == '\t'))
				{
					if (*src == '\t')
					{
						prv_spaces += tab_size - col_in_tab + 1;
						col_in_tab = tab_size;
					}
					else
						prv_spaces++;

					if (col_in_tab == tab_size)
					{
						/*
						 * Is the next character going to be a tab? Needed to
						 * do tab replacement in current spot if next char is
						 * going to be a tab, ignoring min_spaces
						 */
						nxt_spaces = 0;
						while (1)
						{
							if (*(src + nxt_spaces + 1) == NUL ||
								(*(src + nxt_spaces + 1) != ' ' &&
								 *(src + nxt_spaces + 1) != '\t'))
								break;
							if (*(src + nxt_spaces + 1) == ' ')
								++nxt_spaces;
							if (*(src + nxt_spaces + 1) == '\t' ||
								nxt_spaces == tab_size)
							{
								nxt_spaces = tab_size;
								break;
							}
						}
						if ((prv_spaces >= min_spaces ||
							 nxt_spaces == tab_size) &&
							del_tabs == FALSE)
						{
							*(dst++) = '\t';
							prv_spaces = 0;
						}
						else
						{
							for (; prv_spaces > 0; prv_spaces--)
								*(dst++) = ' ';
						}
					}
				}
				else
				{
					for (; prv_spaces > 0; prv_spaces--)
						*(dst++) = ' ';
					if (*src == '\t')	/* only when in quote */
						col_in_tab = 0;
					if (*src == '\b')
						col_in_tab -= 2;
					if (escaped == FALSE && protect_quotes == TRUE)
					{
						if (*src == '\\')
							escaped = TRUE;
						if (*src == '"' || *src == '\'')
							if (quote_char == ' ')
								quote_char = *src;
							else if (*src == quote_char)
								quote_char = ' ';
					}
					else if (*src != '\r' && *src != '\n')
						escaped = FALSE;

					if ((*src == '\r' || *src == '\n') &&
						quote_char == ' ' &&
						clip_lines == TRUE &&
						escaped == FALSE)
					{
						while (dst > out_line &&
							   (*(dst - 1) == ' ' || *(dst - 1) == '\t'))
							dst--;
						prv_spaces = 0;
					}
					*(dst++) = *src;
				}
				col_in_tab %= tab_size;
				++src;
			}
			/* for cases where the last line of file has no newline */
			if (clip_lines == TRUE && escaped == FALSE)
			{
				while (dst > out_line &&
					   (*(dst - 1) == ' ' || *(dst - 1) == '\t'))
					dst--;
				prv_spaces = 0;
			}
			for (; prv_spaces > 0; prv_spaces--)
				*(dst++) = ' ';
			*dst = NUL;
			if (fputs(out_line, stdout) == EOF)
				halt("PERROR:  Error writing output.\n");
		}
	} while (--argc > 0);
	return 0;
}
