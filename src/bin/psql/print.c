#include <config.h>
#include <c.h>
#include "print.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <signal.h>
#ifndef WIN32
#include <unistd.h>				/* for isatty() */
#include <sys/ioctl.h>			/* for ioctl() */
#else
#define popen(x,y) _popen(x,y)
#define pclose(x) _pclose(x)
#endif

#include <pqsignal.h>
#include <libpq-fe.h>
#include <postgres_ext.h>		/* for Oid type */

#define DEFAULT_PAGER "more"



/*************************/
/* Unaligned text		 */
/*************************/


static void
print_unaligned_text(const char *title, const char * const * headers,
		     const char * const * cells, const char * const * footers,
		     const char *opt_fieldsep, bool opt_barebones,
		     FILE *fout)
{
	unsigned int col_count = 0;
	unsigned int i;
	const char * const * ptr;

	if (!opt_fieldsep)
		opt_fieldsep = "";

	/* print title */
	if (!opt_barebones && title)
		fprintf(fout, "%s\n", title);

	/* print headers and count columns */
	for (ptr = headers; *ptr; ptr++)
	{
		col_count++;
		if (!opt_barebones)
		{
			if (col_count > 1)
				fputs(opt_fieldsep, fout);
			fputs(*ptr, fout);
		}
	}
	if (!opt_barebones)
		fputs("\n", fout);

	/* print cells */
	i = 0;
	for (ptr = cells; *ptr; ptr++)
	{
		fputs(*ptr, fout);
		if ((i + 1) % col_count)
			fputs(opt_fieldsep, fout);
		else
			fputs("\n", fout);
		i++;
	}

	/* print footers */

	if (!opt_barebones && footers)
		for (ptr = footers; *ptr; ptr++)
			fprintf(fout, "%s\n", *ptr);

}



static void
print_unaligned_vertical(const char *title, const char * const * headers,
			 const char * const * cells, const char * const * footers,
			 const char *opt_fieldsep, bool opt_barebones,
			 FILE *fout)
{
	unsigned int col_count = 0;
	unsigned int i;
	unsigned int record = 1;
	const char * const * ptr;

	if (!opt_fieldsep)
		opt_fieldsep = "";

	/* print title */
	if (!opt_barebones && title)
		fprintf(fout, "%s\n", title);

	/* count columns */
	for (ptr = headers; *ptr; ptr++)
		col_count++;

	/* print records */
	for (i = 0, ptr = cells; *ptr; i++, ptr++)
	{
		if (i % col_count == 0)
		{
			if (!opt_barebones)
				fprintf(fout, "-- RECORD %d\n", record++);
			else
				fputc('\n', fout);
		}
		fprintf(fout, "%s%s%s\n", headers[i % col_count], opt_fieldsep, *ptr);
	}

	/* print footers */

	if (!opt_barebones && footers)
	{
		fputs("--- END ---\n", fout);
		for (ptr = footers; *ptr; ptr++)
			fprintf(fout, "%s\n", *ptr);
	}
}



/********************/
/* Aligned text		*/
/********************/


/* draw "line" */
static void
_print_horizontal_line(const unsigned int col_count, const unsigned int *widths, unsigned short border, FILE *fout)
{
	unsigned int i,
				j;

	if (border == 1)
		fputc('-', fout);
	else if (border == 2)
		fputs("+-", fout);

	for (i = 0; i < col_count; i++)
	{
		for (j = 0; j < widths[i]; j++)
			fputc('-', fout);

		if (i < col_count - 1)
		{
			if (border == 0)
				fputc(' ', fout);
			else
				fputs("-+-", fout);
		}
	}

	if (border == 2)
		fputs("-+", fout);
	else if (border == 1)
		fputc('-', fout);

	fputc('\n', fout);
}



static void
print_aligned_text(const char *title, const char * const * headers,
		   const char * const * cells, const char * const * footers,
		   const char *opt_align, bool opt_barebones, unsigned short int opt_border,
		   FILE *fout)
{
	unsigned int col_count = 0;
	unsigned int i,
				tmp;
	unsigned int *widths,
				total_w;
	const char * const * ptr;

	/* count columns */
	for (ptr = headers; *ptr; ptr++)
		col_count++;

	widths = calloc(col_count, sizeof(*widths));
	if (!widths)
	{
		perror("calloc");
		exit(EXIT_FAILURE);
	}

	/* calc column widths */
	for (i = 0; i < col_count; i++)
		if ((tmp = strlen(headers[i])) > widths[i])
			widths[i] = tmp;	/* don't wanna call strlen twice */

	for (i = 0, ptr = cells; *ptr; ptr++, i++)
		if ((tmp = strlen(*ptr)) > widths[i % col_count])
			widths[i % col_count] = tmp;

	if (opt_border == 0)
		total_w = col_count - 1;
	else if (opt_border == 1)
		total_w = col_count * 3 - 1;
	else
		total_w = col_count * 3 + 1;

	for (i = 0; i < col_count; i++)
		total_w += widths[i];

	/* print title */
	if (title && !opt_barebones)
	{
		if (strlen(title) >= total_w)
			fprintf(fout, "%s\n", title);
		else
			fprintf(fout, "%-*s%s\n", (total_w - strlen(title)) / 2, "", title);
	}

	/* print headers */
	if (!opt_barebones)
	{
		if (opt_border == 2)
			_print_horizontal_line(col_count, widths, opt_border, fout);

		if (opt_border == 2)
			fputs("| ", fout);
		else if (opt_border == 1)
			fputc(' ', fout);

		for (i = 0; i < col_count; i++)
		{
			/* centered */
			fprintf(fout, "%-*s%s%-*s", (int) floor((widths[i] - strlen(headers[i])) / 2.0), "", headers[i], (int) ceil((widths[i] - strlen(headers[i])) / 2.0), "");

			if (i < col_count - 1)
			{
				if (opt_border == 0)
					fputc(' ', fout);
				else
					fputs(" | ", fout);
			}
		}

		if (opt_border == 2)
			fputs(" |", fout);
		else if (opt_border == 1)
			fputc(' ', fout);;
		fputc('\n', fout);

		_print_horizontal_line(col_count, widths, opt_border, fout);
	}

	/* print cells */
	for (i = 0, ptr = cells; *ptr; i++, ptr++)
	{
		/* beginning of line */
		if (i % col_count == 0)
		{
			if (opt_border == 2)
				fputs("| ", fout);
			else if (opt_border == 1)
				fputc(' ', fout);
		}

		/* content */
		if (opt_align[(i) % col_count] == 'r')
			fprintf(fout, "%*s", widths[i % col_count], cells[i]);
		else
		{
			if ((i + 1) % col_count == 0 && opt_border != 2)
				fputs(cells[i], fout);
			else
				fprintf(fout, "%-*s", widths[i % col_count], cells[i]);
		}

		/* divider */
		if ((i + 1) % col_count)
		{
			if (opt_border == 0)
				fputc(' ', fout);
			else
				fputs(" | ", fout);
		}
		/* end of line */
		else
		{
			if (opt_border == 2)
				fputs(" |", fout);
			fputc('\n', fout);
		}
	}

	if (opt_border == 2)
		_print_horizontal_line(col_count, widths, opt_border, fout);

	/* print footers */
	if (footers && !opt_barebones)
		for (ptr = footers; *ptr; ptr++)
			fprintf(fout, "%s\n", *ptr);

	fputc('\n', fout);

	/* clean up */
	free(widths);
}



static void
print_aligned_vertical(const char *title, const char * const * headers,
		       const char * const * cells, const char * const * footers,
		       bool opt_barebones, unsigned short int opt_border,
		       FILE *fout)
{
	unsigned int col_count = 0;
	unsigned int record = 1;
	const char * const *ptr;
	unsigned int i,
				tmp,
				hwidth = 0,
				dwidth = 0;
	char	   *divider;

    if (cells[0] == NULL) {
        puts("(No rows)\n");
        return;
    }

	/* count columns and find longest header */
	for (ptr = headers; *ptr; ptr++)
	{
		col_count++;
		if ((tmp = strlen(*ptr)) > hwidth)
			hwidth = tmp;		/* don't wanna call strlen twice */
	}

	/* find longest data cell */
	for (ptr = cells; *ptr; ptr++)
		if ((tmp = strlen(*ptr)) > dwidth)
			dwidth = tmp;

	/* print title */
	if (!opt_barebones && title)
		fprintf(fout, "%s\n", title);

	/* make horizontal border */
	divider = malloc(hwidth + dwidth + 10);
	if (!divider)
	{
		perror("malloc");
		exit(EXIT_FAILURE);
	}
	divider[0] = '\0';
	if (opt_border == 2)
		strcat(divider, "+-");
	for (i = 0; i < hwidth; i++)
		strcat(divider, opt_border > 0 ? "-" : " ");
	if (opt_border > 0)
		strcat(divider, "-+-");
	else
		strcat(divider, " ");
	for (i = 0; i < dwidth; i++)
		strcat(divider, opt_border > 0 ? "-" : " ");
	if (opt_border == 2)
		strcat(divider, "-+");


	/* print records */
	for (i = 0, ptr = cells; *ptr; i++, ptr++)
	{
		if (i % col_count == 0)
		{
			if (!opt_barebones)
			{
				char	   *record_str = malloc(32);
				size_t		record_str_len;

				if (!record_str)
				{
					perror("malloc");
					exit(EXIT_FAILURE);
				}

				if (opt_border == 0)
					sprintf(record_str, "* Record %d", record++);
				else
					sprintf(record_str, "[ RECORD %d ]", record++);
				record_str_len = strlen(record_str);

				if (record_str_len + opt_border > strlen(divider))
                    fprintf(fout, "%.*s%s\n", opt_border, divider, record_str);
                else
                {
                    char	   *div_copy = strdup(divider);

                    if (!div_copy) {
                        perror("malloc");
                        exit(EXIT_FAILURE);
                    }

                    strncpy(div_copy + opt_border, record_str, record_str_len);
                    fprintf(fout, "%s\n", div_copy);
                    free(div_copy);
                }
				free(record_str);
			}
			else if (i != 0 || opt_border == 2)
				fprintf(fout, "%s\n", divider);
		}

		if (opt_border == 2)
			fputs("| ", fout);
		fprintf(fout, "%-*s", hwidth, headers[i % col_count]);

		if (opt_border > 0)
			fputs(" | ", fout);
		else
			fputs(" ", fout);

		if (opt_border < 2)
			fprintf(fout, "%s\n", *ptr);
		else
			fprintf(fout, "%-*s |\n", dwidth, *ptr);
	}

	if (opt_border == 2)
		fprintf(fout, "%s\n", divider);


	/* print footers */

	if (!opt_barebones && footers && *footers)
	{
		if (opt_border < 2)
			fputc('\n', fout);
		for (ptr = footers; *ptr; ptr++)
			fprintf(fout, "%s\n", *ptr);
	}

	fputc('\n', fout);
	free(divider);
}





/**********************/
/* HTML printing ******/
/**********************/


static void
html_escaped_print(const char *in, FILE *fout)
{
	const char *p;

	for (p = in; *p; p++)
		switch (*p)
		{
			case '&':
				fputs("&amp;", fout);
				break;
			case '<':
				fputs("&lt;", fout);
				break;
			case '>':
				fputs("&gt;", fout);
				break;
			case '\n':
				fputs("<br>", fout);
				break;
			default:
				fputc(*p, fout);
		}
}



static void
print_html_text(const char *title, const char * const * headers,
		const char * const * cells, const char * const * footers,
		const char *opt_align, bool opt_barebones, unsigned short int opt_border,
		const char *opt_table_attr,
		FILE *fout)
{
	unsigned int col_count = 0;
	unsigned int i;
	const char * const *ptr;

	fprintf(fout, "<table border=%d", opt_border);
	if (opt_table_attr)
		fprintf(fout, " %s", opt_table_attr);
	fputs(">\n", fout);

	/* print title */
	if (!opt_barebones && title)
	{
		fputs("  <caption>", fout);
		html_escaped_print(title, fout);
		fputs("</caption>\n", fout);
	}

	/* print headers and count columns */
	if (!opt_barebones)
		fputs("  <tr>\n", fout);
	for (i = 0, ptr = headers; *ptr; i++, ptr++)
	{
		col_count++;
		if (!opt_barebones)
		{
			fputs("    <th align=center>", fout);
			html_escaped_print(*ptr, fout);
			fputs("</th>\n", fout);
		}
	}
	if (!opt_barebones)
		fputs("  </tr>\n", fout);

	/* print cells */
	for (i = 0, ptr = cells; *ptr; i++, ptr++)
	{
		if (i % col_count == 0)
			fputs("  <tr valign=top>\n", fout);

		fprintf(fout, "    <td align=%s>", opt_align[(i) % col_count] == 'r' ? "right" : "left");
		if ((*ptr)[strspn(*ptr, " \t")] == '\0')		/* is string only
														 * whitespace? */
			fputs("&nbsp;", fout);
		else
			html_escaped_print(*ptr, fout);
		fputs("</td>\n", fout);

		if ((i + 1) % col_count == 0)
			fputs("  </tr>\n", fout);
	}

	fputs("</table>\n", fout);

	/* print footers */

	if (footers && !opt_barebones)
		for (ptr = footers; *ptr; ptr++)
		{
			html_escaped_print(*ptr, fout);
			fputs("<br>\n", fout);
		}

	fputc('\n', fout);
}



static void
print_html_vertical(const char *title, const char * const * headers,
		    const char * const * cells, const char * const * footers,
		    const char *opt_align, bool opt_barebones, unsigned short int opt_border,
		    const char *opt_table_attr,
					FILE *fout)
{
	unsigned int col_count = 0;
	unsigned int i;
	unsigned int record = 1;
	const char * const *ptr;

	fprintf(fout, "<table border=%d", opt_border);
	if (opt_table_attr)
		fprintf(fout, " %s", opt_table_attr);
	fputs(">\n", fout);

	/* print title */
	if (!opt_barebones && title)
	{
		fputs("  <caption>", fout);
		html_escaped_print(title, fout);
		fputs("</caption>\n", fout);
	}

	/* count columns */
	for (ptr = headers; *ptr; ptr++)
		col_count++;

	/* print records */
	for (i = 0, ptr = cells; *ptr; i++, ptr++)
	{
		if (i % col_count == 0)
		{
			if (!opt_barebones)
				fprintf(fout, "\n  <tr><td colspan=2 align=center>Record %d</td></tr>\n", record++);
			else
				fputs("\n  <tr><td colspan=2>&nbsp;</td></tr>\n", fout);
		}
		fputs("  <tr valign=top>\n"
			  "    <th>", fout);
		html_escaped_print(headers[i % col_count], fout);
		fputs("</th>\n", fout);

		fprintf(fout, "    <td align=%s>", opt_align[i % col_count] == 'r' ? "right" : "left");
		if ((*ptr)[strspn(*ptr, " \t")] == '\0')		/* is string only
														 * whitespace? */
			fputs("&nbsp;", fout);
		else
			html_escaped_print(*ptr, fout);
		fputs("</td>\n  </tr>\n", fout);
	}

	fputs("</table>\n", fout);

	/* print footers */
	if (footers && !opt_barebones)
		for (ptr = footers; *ptr; ptr++)
		{
			html_escaped_print(*ptr, fout);
			fputs("<br>\n", fout);
		}

	fputc('\n', fout);
}



/*************************/
/* LaTeX				 */
/*************************/


static void
latex_escaped_print(const char *in, FILE *fout)
{
	const char *p;

	for (p = in; *p; p++)
		switch (*p)
		{
			case '&':
				fputs("\\&", fout);
				break;
			case '%':
				fputs("\\%", fout);
				break;
			case '$':
				fputs("\\$", fout);
				break;
			case '{':
				fputs("\\{", fout);
				break;
			case '}':
				fputs("\\}", fout);
				break;
			case '\\':
				fputs("\\backslash", fout);
				break;
			case '\n':
				fputs("\\\\", fout);
				break;
			default:
				fputc(*p, fout);
		}
}



static void
print_latex_text(const char *title, const char * const * headers,
		 const char * const * cells, const char * const * footers,
		 const char *opt_align, bool opt_barebones, unsigned short int opt_border,
		 FILE *fout)
{
	unsigned int col_count = 0;
	unsigned int i;
	const char *cp;
	const char * const *ptr;


	/* print title */
	if (!opt_barebones && title)
	{
		fputs("\begin{center}\n", fout);
		latex_escaped_print(title, fout);
		fputs("\n\end{center}\n\n", fout);
	}

	/* begin environment and set alignments and borders */
	fputs("\\begin{tabular}{", fout);
	if (opt_border == 0)
		fputs(opt_align, fout);
	else if (opt_border == 1)
	{
		for (cp = opt_align; *cp; cp++)
		{
			if (cp != opt_align)
				fputc('|', fout);
			fputc(*cp, fout);
		}
	}
	else if (opt_border == 2)
	{
		for (cp = opt_align; *cp; cp++)
		{
			fputc('|', fout);
			fputc(*cp, fout);
		}
		fputc('|', fout);
	}
	fputs("}\n", fout);

	if (!opt_barebones && opt_border == 2)
		fputs("\\hline\n", fout);

	/* print headers and count columns */
	for (i = 0, ptr = headers; *ptr; i++, ptr++)
	{
		col_count++;
		if (!opt_barebones)
		{
			if (i != 0)
				fputs(" & ", fout);
			latex_escaped_print(*ptr, fout);
		}
	}

	if (!opt_barebones)
	{
		fputs(" \\\\\n", fout);
		fputs("\\hline\n", fout);
	}

	/* print cells */
	for (i = 0, ptr = cells; *ptr; i++, ptr++)
	{
		latex_escaped_print(*ptr, fout);

		if ((i + 1) % col_count == 0)
			fputs(" \\\\\n", fout);
		else
			fputs(" & ", fout);
	}

	if (opt_border == 2)
		fputs("\\hline\n", fout);

	fputs("\\end{tabular}\n\n", fout);


	/* print footers */

	if (footers && !opt_barebones)
		for (ptr = footers; *ptr; ptr++)
		{
			latex_escaped_print(*ptr, fout);
			fputs(" \\\\\n", fout);
		}

	fputc('\n', fout);
}



static void
print_latex_vertical(const char *title, const char * const * headers,
		     const char * const * cells, const char * const * footers,
		     const char *opt_align, bool opt_barebones, unsigned short int opt_border,
		     FILE *fout)
{
	unsigned int col_count = 0;
	unsigned int i;
	const char * const *ptr;
	unsigned int record = 1;

	(void) opt_align;			/* currently unused parameter */

	/* print title */
	if (!opt_barebones && title)
	{
		fputs("\begin{center}\n", fout);
		latex_escaped_print(title, fout);
		fputs("\n\end{center}\n\n", fout);
	}

	/* begin environment and set alignments and borders */
	fputs("\\begin{tabular}{", fout);
	if (opt_border == 0)
		fputs("cl", fout);
	else if (opt_border == 1)
		fputs("c|l", fout);
	else if (opt_border == 2)
		fputs("|c|l|", fout);
	fputs("}\n", fout);


	/* count columns */
	for (ptr = headers; *ptr; ptr++)
		col_count++;


	/* print records */
	for (i = 0, ptr = cells; *ptr; i++, ptr++)
	{
		/* new record */
		if (i % col_count == 0)
		{
			if (!opt_barebones)
			{
				if (opt_border == 2)
					fputs("\\hline\n", fout);
				fprintf(fout, "\\multicolumn{2}{c}{Record %d} \\\\\n", record++);
			}
			if (opt_border >= 1)
				fputs("\\hline\n", fout);
		}

		latex_escaped_print(headers[i % col_count], fout);
		fputs(" & ", fout);
		latex_escaped_print(*ptr, fout);
		fputs(" \\\\\n", fout);
	}

	if (opt_border == 2)
		fputs("\\hline\n", fout);

	fputs("\\end{tabular}\n\n", fout);


	/* print footers */

	if (footers && !opt_barebones)
		for (ptr = footers; *ptr; ptr++)
		{
			latex_escaped_print(*ptr, fout);
			fputs(" \\\\\n", fout);
		}

	fputc('\n', fout);
}






/********************************/
/* Public functions		*/
/********************************/


void
printTable(const char *title,
	   const char * const * headers,
	   const char * const * cells,
	   const char * const * footers,
	   const char *align,
	   const printTableOpt * opt, FILE *fout)
{
	const char *default_footer[] = {NULL};
	unsigned short int border = opt->border;
	FILE	   *pager = NULL,
			   *output;


	if (opt->format == PRINT_NOTHING)
		return;

	if (!footers)
		footers = default_footer;

	if (opt->format != PRINT_HTML && border > 2)
		border = 2;


	/* check whether we need / can / are supposed to use pager */
	if (fout == stdout && opt->pager
#ifndef WIN32
		&&
		isatty(fileno(stdin)) &&
		isatty(fileno(stdout))
#endif
		)
	{
		const char *pagerprog;

#ifdef TIOCGWINSZ
		unsigned int col_count = 0,
					row_count = 0,
					lines;
		const char * const *ptr;
		int			result;
		struct winsize screen_size;

		/* rough estimate of columns and rows */
		if (headers)
			for (ptr = headers; *ptr; ptr++)
				col_count++;
		if (cells)
			for (ptr = cells; *ptr; ptr++)
				row_count++;
		row_count /= col_count;

		if (opt->expanded)
			lines = (col_count + 1) * row_count;
		else
			lines = row_count + 1;
		if (!opt->tuples_only)
			lines += 5;

		result = ioctl(fileno(stdout), TIOCGWINSZ, &screen_size);
		if (result == -1 || lines > screen_size.ws_row)
		{
#endif
			pagerprog = getenv("PAGER");
			if (!pagerprog)
				pagerprog = DEFAULT_PAGER;
			pager = popen(pagerprog, "w");
#ifdef TIOCGWINSZ
		}
#endif
	}

	if (pager)
	{
		output = pager;
		pqsignal(SIGPIPE, SIG_IGN);
	}
	else
		output = fout;


	/* print the stuff */

	switch (opt->format)
	{
		case PRINT_UNALIGNED:
			if (opt->expanded)
				print_unaligned_vertical(title, headers, cells, footers, opt->fieldSep, opt->tuples_only, output);
			else
				print_unaligned_text(title, headers, cells, footers, opt->fieldSep, opt->tuples_only, output);
			break;
		case PRINT_ALIGNED:
			if (opt->expanded)
				print_aligned_vertical(title, headers, cells, footers, opt->tuples_only, border, output);
			else
				print_aligned_text(title, headers, cells, footers, align, opt->tuples_only, border, output);
			break;
		case PRINT_HTML:
			if (opt->expanded)
				print_html_vertical(title, headers, cells, footers, align, opt->tuples_only, border, opt->tableAttr, output);
			else
				print_html_text(title, headers, cells, footers, align, opt->tuples_only, border, opt->tableAttr, output);
			break;
		case PRINT_LATEX:
			if (opt->expanded)
				print_latex_vertical(title, headers, cells, footers, align, opt->tuples_only, border, output);
			else
				print_latex_text(title, headers, cells, footers, align, opt->tuples_only, border, output);
			break;
		default:
			fprintf(stderr, "+ Oops, you shouldn't see this!\n");
	}

	if (pager)
	{
		pclose(pager);
		pqsignal(SIGPIPE, SIG_DFL);
	}
}



void
printQuery(const PGresult *result, const printQueryOpt * opt, FILE *fout)
{
	int			nfields;
	const char **headers;
	const char **cells;
	char **footers;
	char 	   *align;
	int			i;

	/* extract headers */

	nfields = PQnfields(result);

	headers = calloc(nfields + 1, sizeof(*headers));
	if (!headers)
	{
		perror("calloc");
		exit(EXIT_FAILURE);
	}

	for (i = 0; i < nfields; i++)
		headers[i] = PQfname(result, i);

	/* set cells */

	cells = calloc(nfields * PQntuples(result) + 1, sizeof(*cells));
	if (!cells)
	{
		perror("calloc");
		exit(EXIT_FAILURE);
	}

	for (i = 0; i < nfields * PQntuples(result); i++)
	{
		if (PQgetisnull(result, i / nfields, i % nfields))
			cells[i] = opt->nullPrint ? opt->nullPrint : "";
		else
			cells[i] = PQgetvalue(result, i / nfields, i % nfields);
	}

	/* set footers */

	if (opt->footers)
		footers = opt->footers;
	else if (!opt->topt.expanded)
	{
		footers = calloc(2, sizeof(*footers));
		if (!footers)
		{
			perror("calloc");
			exit(EXIT_FAILURE);
		}

		footers[0] = malloc(100);
		if (PQntuples(result) == 1)
			strcpy(footers[0], "(1 row)");
		else
			sprintf(footers[0], "(%d rows)", PQntuples(result));
	}
	else
		footers = NULL;

	/* set alignment */

	align = calloc(nfields + 1, sizeof(*align));
	if (!align)
	{
		perror("calloc");
		exit(EXIT_FAILURE);
	}

	for (i = 0; i < nfields; i++)
	{
		Oid			ftype = PQftype(result, i);

		if (ftype == 20 ||		/* int8 */
			ftype == 21 ||		/* int2 */
			ftype == 23 ||		/* int4 */
			(ftype >= 26 && ftype <= 30) ||		/* ?id */
			ftype == 700 ||		/* float4 */
			ftype == 701 ||		/* float8 */
			ftype == 790 ||		/* money */
			ftype == 1700		/* numeric */
			)
			align[i] = 'r';
		else
			align[i] = 'l';
	}

	/* call table printer */

	printTable(opt->title, headers, cells,
		   footers ? (const char * const *)footers : (const char * const *)(opt->footers),
		   align, &opt->topt, fout);

	free(headers);
	free(cells);
	if (footers)
	{
		free(footers[0]);
		free(footers);
	}
}


/* the end */
