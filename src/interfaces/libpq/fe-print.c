/*-------------------------------------------------------------------------
 *
 * fe-print.c--
 *	  functions for pretty-printing query results
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * These functions were formerly part of fe-exec.c, but they
 * didn't really belong there.
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/libpq/fe-print.c,v 1.17 1999/02/03 21:17:50 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "libpq-fe.h"
#include "libpq-int.h"
#include "postgres.h"
#include "pqsignal.h"

#ifdef WIN32
#include "win32.h"
#else
#if !defined(NO_UNISTD_H)
#include <unistd.h>
#endif
#include <sys/ioctl.h>
#ifndef HAVE_TERMIOS_H
#include <sys/termios.h>
#else
#include <termios.h>
#endif
#endif	 /* WIN32 */
#include <stdlib.h>
#include <signal.h>
#include <string.h>

#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#endif

#ifdef TIOCGWINSZ
static struct winsize screen_size;

#else
static struct winsize
{
	int			ws_row;
	int			ws_col;
}			screen_size;

#endif


static void do_field(PQprintOpt *po, PGresult *res,
		 const int i, const int j, char *buf, const int fs_len,
		 char **fields,
		 const int nFields, char **fieldNames,
		 unsigned char *fieldNotNum, int *fieldMax,
		 const int fieldMaxLen, FILE *fout);
static char *do_header(FILE *fout, PQprintOpt *po, const int nFields,
		  int *fieldMax, char **fieldNames, unsigned char *fieldNotNum,
		  const int fs_len, PGresult *res);
static void output_row(FILE *fout, PQprintOpt *po, const int nFields, char **fields,
		   unsigned char *fieldNotNum, int *fieldMax, char *border,
		   const int row_index);
static void fill(int length, int max, char filler, FILE *fp);


/*
 * PQprint()
 *
 * Format results of a query for printing.
 *
 * PQprintOpt is a typedef (structure) that containes
 * various flags and options. consult libpq-fe.h for
 * details
 *
 * Obsoletes PQprintTuples.
 */

void
PQprint(FILE *fout,
		PGresult *res,
		PQprintOpt *po)
{
	int			nFields;

	nFields = PQnfields(res);

	if (nFields > 0)
	{							/* only print rows with at least 1 field.  */
		int			i,
					j;
		int			nTups;
		int		   *fieldMax = NULL;	/* in case we don't use them */
		unsigned char *fieldNotNum = NULL;
		char	   *border = NULL;
		char	  **fields = NULL;
		char	  **fieldNames;
		int			fieldMaxLen = 0;
		int			numFieldName;
		int			fs_len = strlen(po->fieldSep);
		int			total_line_length = 0;
		int			usePipe = 0;
		pqsigfunc	oldsigpipehandler = NULL;
		char	   *pagerenv;
		char		buf[8192 * 2 + 1];

		nTups = PQntuples(res);
		if (!(fieldNames = (char **) calloc(nFields, sizeof(char *))))
		{
			perror("calloc");
			exit(1);
		}
		if (!(fieldNotNum = (unsigned char *) calloc(nFields, 1)))
		{
			perror("calloc");
			exit(1);
		}
		if (!(fieldMax = (int *) calloc(nFields, sizeof(int))))
		{
			perror("calloc");
			exit(1);
		}
		for (numFieldName = 0;
			 po->fieldName && po->fieldName[numFieldName];
			 numFieldName++)
			;
		for (j = 0; j < nFields; j++)
		{
			int			len;
			char	   *s = (j < numFieldName && po->fieldName[j][0]) ?
			po->fieldName[j] : PQfname(res, j);

			fieldNames[j] = s;
			len = s ? strlen(s) : 0;
			fieldMax[j] = len;
			len += fs_len;
			if (len > fieldMaxLen)
				fieldMaxLen = len;
			total_line_length += len;
		}

		total_line_length += nFields * strlen(po->fieldSep) + 1;

		if (fout == NULL)
			fout = stdout;
		if (po->pager && fout == stdout
#ifndef WIN32
			&&
			isatty(fileno(stdin)) &&
			isatty(fileno(stdout))
#endif
			)
		{
			/* try to pipe to the pager program if possible */
#ifdef TIOCGWINSZ
			if (ioctl(fileno(stdout), TIOCGWINSZ, &screen_size) == -1 ||
				screen_size.ws_col == 0 ||
				screen_size.ws_row == 0)
			{
#endif
				screen_size.ws_row = 24;
				screen_size.ws_col = 80;
#ifdef TIOCGWINSZ
			}
#endif
			pagerenv = getenv("PAGER");
			if (pagerenv != NULL &&
				pagerenv[0] != '\0' &&
				!po->html3 &&
				((po->expanded &&
				  nTups * (nFields + 1) >= screen_size.ws_row) ||
				 (!po->expanded &&
				  nTups * (total_line_length / screen_size.ws_col + 1) *
				  (1 + (po->standard != 0)) >= screen_size.ws_row -
				  (po->header != 0) *
				  (total_line_length / screen_size.ws_col + 1) * 2
				  - (po->header != 0) * 2		/* row count and newline */
				  )))
			{
#ifdef WIN32
				fout = _popen(pagerenv, "w");
#else
				fout = popen(pagerenv, "w");
#endif
				if (fout)
				{
					usePipe = 1;
#ifndef WIN32
					oldsigpipehandler = pqsignal(SIGPIPE, SIG_IGN);
#endif
				}
				else
					fout = stdout;
			}
		}

		if (!po->expanded && (po->align || po->html3))
		{
			if (!(fields = (char **) calloc(nFields * (nTups + 1), sizeof(char *))))
			{
				perror("calloc");
				exit(1);
			}
		}
		else if (po->header && !po->html3)
		{
			if (po->expanded)
			{
				if (po->align)
					fprintf(fout, "%-*s%s Value\n",
							fieldMaxLen - fs_len, "Field", po->fieldSep);
				else
					fprintf(fout, "%s%sValue\n", "Field", po->fieldSep);
			}
			else
			{
				int			len = 0;

				for (j = 0; j < nFields; j++)
				{
					char	   *s = fieldNames[j];

					fputs(s, fout);
					len += strlen(s) + fs_len;
					if ((j + 1) < nFields)
						fputs(po->fieldSep, fout);
				}
				fputc('\n', fout);
				for (len -= fs_len; len--; fputc('-', fout));
				fputc('\n', fout);
			}
		}
		if (po->expanded && po->html3)
		{
			if (po->caption)
				fprintf(fout, "<centre><h2>%s</h2></centre>\n", po->caption);
			else
				fprintf(fout,
						"<centre><h2>"
						"Query retrieved %d rows * %d fields"
						"</h2></centre>\n",
						nTups, nFields);
		}
		for (i = 0; i < nTups; i++)
		{
			if (po->expanded)
			{
				if (po->html3)
					fprintf(fout,
						  "<table %s><caption align=high>%d</caption>\n",
							po->tableOpt ? po->tableOpt : "", i);
				else
					fprintf(fout, "-- RECORD %d --\n", i);
			}
			for (j = 0; j < nFields; j++)
				do_field(po, res, i, j, buf, fs_len, fields, nFields,
						 fieldNames, fieldNotNum,
						 fieldMax, fieldMaxLen, fout);
			if (po->html3 && po->expanded)
				fputs("</table>\n", fout);
		}
		if (!po->expanded && (po->align || po->html3))
		{
			if (po->html3)
			{
				if (po->header)
				{
					if (po->caption)
						fprintf(fout,
						  "<table %s><caption align=high>%s</caption>\n",
								po->tableOpt ? po->tableOpt : "",
								po->caption);
					else
						fprintf(fout,
								"<table %s><caption align=high>"
								"Retrieved %d rows * %d fields"
								"</caption>\n",
						po->tableOpt ? po->tableOpt : "", nTups, nFields);
				}
				else
					fprintf(fout, "<table %s>", po->tableOpt ? po->tableOpt : "");
			}
			if (po->header)
				border = do_header(fout, po, nFields, fieldMax, fieldNames,
								   fieldNotNum, fs_len, res);
			for (i = 0; i < nTups; i++)
				output_row(fout, po, nFields, fields,
						   fieldNotNum, fieldMax, border, i);
			free(fields);
			if (border)
				free(border);
		}
		if (po->header && !po->html3)
			fprintf(fout, "(%d row%s)\n\n", PQntuples(res),
					(PQntuples(res) == 1) ? "" : "s");
		free(fieldMax);
		free(fieldNotNum);
		free(fieldNames);
		if (usePipe)
		{
#ifdef WIN32
			_pclose(fout);
#else
			pclose(fout);
			pqsignal(SIGPIPE, oldsigpipehandler);
#endif
		}
		if (po->html3 && !po->expanded)
			fputs("</table>\n", fout);
	}
}


/*
 * PQdisplayTuples()
 * kept for backward compatibility
 */

void
PQdisplayTuples(PGresult *res,
				FILE *fp,		/* where to send the output */
				int fillAlign,	/* pad the fields with spaces */
				const char *fieldSep,	/* field separator */
				int printHeader,/* display headers? */
				int quiet
)
{
#define DEFAULT_FIELD_SEP " "

	int			i,
				j;
	int			nFields;
	int			nTuples;
	int			fLength[MAX_FIELDS];

	if (fieldSep == NULL)
		fieldSep = DEFAULT_FIELD_SEP;

	/* Get some useful info about the results */
	nFields = PQnfields(res);
	nTuples = PQntuples(res);

	if (fp == NULL)
		fp = stdout;

	/* Zero the initial field lengths */
	for (j = 0; j < nFields; j++)
		fLength[j] = strlen(PQfname(res, j));
	/* Find the max length of each field in the result */
	/* will be somewhat time consuming for very large results */
	if (fillAlign)
	{
		for (i = 0; i < nTuples; i++)
		{
			for (j = 0; j < nFields; j++)
			{
				if (PQgetlength(res, i, j) > fLength[j])
					fLength[j] = PQgetlength(res, i, j);
			}
		}
	}

	if (printHeader)
	{
		/* first, print out the attribute names */
		for (i = 0; i < nFields; i++)
		{
			fputs(PQfname(res, i), fp);
			if (fillAlign)
				fill(strlen(PQfname(res, i)), fLength[i], ' ', fp);
			fputs(fieldSep, fp);
		}
		fprintf(fp, "\n");

		/* Underline the attribute names */
		for (i = 0; i < nFields; i++)
		{
			if (fillAlign)
				fill(0, fLength[i], '-', fp);
			fputs(fieldSep, fp);
		}
		fprintf(fp, "\n");
	}

	/* next, print out the instances */
	for (i = 0; i < nTuples; i++)
	{
		for (j = 0; j < nFields; j++)
		{
			fprintf(fp, "%s", PQgetvalue(res, i, j));
			if (fillAlign)
				fill(strlen(PQgetvalue(res, i, j)), fLength[j], ' ', fp);
			fputs(fieldSep, fp);
		}
		fprintf(fp, "\n");
	}

	if (!quiet)
		fprintf(fp, "\nQuery returned %d row%s.\n", PQntuples(res),
				(PQntuples(res) == 1) ? "" : "s");

	fflush(fp);
}



/*
 * PQprintTuples()
 *
 * kept for backward compatibility
 *
 */
void
PQprintTuples(PGresult *res,
			  FILE *fout,		/* output stream */
			  int PrintAttNames,/* print attribute names or not */
			  int TerseOutput,	/* delimiter bars or not? */
			  int colWidth		/* width of column, if 0, use variable
								 * width */
)
{
	int			nFields;
	int			nTups;
	int			i,
				j;
	char		formatString[80];

	char	   *tborder = NULL;

	nFields = PQnfields(res);
	nTups = PQntuples(res);

	if (colWidth > 0)
		sprintf(formatString, "%%s %%-%ds", colWidth);
	else
		sprintf(formatString, "%%s %%s");

	if (nFields > 0)
	{							/* only print rows with at least 1 field.  */

		if (!TerseOutput)
		{
			int			width;

			width = nFields * 14;
			tborder = malloc(width + 1);
			for (i = 0; i <= width; i++)
				tborder[i] = '-';
			tborder[i] = '\0';
			fprintf(fout, "%s\n", tborder);
		}

		for (i = 0; i < nFields; i++)
		{
			if (PrintAttNames)
			{
				fprintf(fout, formatString,
						TerseOutput ? "" : "|",
						PQfname(res, i));
			}
		}

		if (PrintAttNames)
		{
			if (TerseOutput)
				fprintf(fout, "\n");
			else
				fprintf(fout, "|\n%s\n", tborder);
		}

		for (i = 0; i < nTups; i++)
		{
			for (j = 0; j < nFields; j++)
			{
				char	   *pval = PQgetvalue(res, i, j);

				fprintf(fout, formatString,
						TerseOutput ? "" : "|",
						pval ? pval : "");
			}
			if (TerseOutput)
				fprintf(fout, "\n");
			else
				fprintf(fout, "|\n%s\n", tborder);
		}
	}
}

#ifdef MULTIBYTE
/*
 * returns the byte length of the word beginning s.
 * Client side encoding is determined by the environment variable
 * "PGCLIENTENCODING".
 * if this variable is not defined, the same encoding as
 * the backend is assumed.
 */
int
PQmblen(unsigned char *s)
{
	char	   *str;
	int			encoding = -1;

	str = getenv("PGCLIENTENCODING");
	if (str && *str != NULL)
		encoding = pg_char_to_encoding(str);
	if (encoding < 0)
		encoding = MULTIBYTE;
	return (pg_encoding_mblen(encoding, s));
}

#else

/* Provide a default definition in case someone calls it anyway */
int
PQmblen(unsigned char *s)
{
	return 1;
}

#endif	/* MULTIBYTE */

static void
do_field(PQprintOpt *po, PGresult *res,
		 const int i, const int j, char *buf, const int fs_len,
		 char **fields,
		 const int nFields, char **fieldNames,
		 unsigned char *fieldNotNum, int *fieldMax,
		 const int fieldMaxLen, FILE *fout)
{

	char	   *pval,
			   *p,
			   *o;
	int			plen;
	bool		skipit;

	plen = PQgetlength(res, i, j);
	pval = PQgetvalue(res, i, j);

	if (plen < 1 || !pval || !*pval)
	{
		if (po->align || po->expanded)
			skipit = true;
		else
		{
			skipit = false;
			goto efield;
		}
	}
	else
		skipit = false;

	if (!skipit)
	{
		char		ch = 0;
#ifdef MULTIBYTE
		int			len;

		for (p = pval, o = buf; *p;
			 len = PQmblen(p), memcpy(o, p, len),
			 o += len, p += len)
#else
		for (p = pval, o = buf; *p; *(o++) = *(p++))
#endif
		{
			ch = *p;
			/*
			 * Consensus on pgsql-interfaces (as of Aug 1998) seems to be that
			 * the print functions ought not insert backslashes.  If you like
			 * them, you can re-enable this next bit.
			 */
#ifdef GRATUITOUS_BACKSLASHES
			if ((fs_len == 1 && (ch == *(po->fieldSep))) ||
				ch == '\\' || ch == '\n')
				*(o++) = '\\';
#endif
			if (po->align &&
				!((ch >= '0' && ch <= '9') ||
				  ch == '.' ||
				  ch == 'E' ||
				  ch == 'e' ||
				  ch == ' ' ||
				  ch == '-'))
				fieldNotNum[j] = 1;
		}
		*o = '\0';
		/*
		 * Above loop will believe E in first column is numeric; also, we
		 * insist on a digit in the last column for a numeric.  This test
		 * is still not bulletproof but it handles most cases.
		 */
		if (po->align &&
			(*pval == 'E' || *pval == 'e' ||
			 !(ch >= '0' && ch <= '9')))
			fieldNotNum[j] = 1;
		if (!po->expanded && (po->align || po->html3))
		{
			int			n = strlen(buf);

			if (n > fieldMax[j])
				fieldMax[j] = n;
			if (!(fields[i * nFields + j] = (char *) malloc(n + 1)))
			{
				perror("malloc");
				exit(1);
			}
			strcpy(fields[i * nFields + j], buf);
		}
		else
		{
			if (po->expanded)
			{
				if (po->html3)
					fprintf(fout,
							"<tr><td align=left><b>%s</b></td>"
							"<td align=%s>%s</td></tr>\n",
							fieldNames[j],
							fieldNotNum[j] ? "left" : "right",
							buf);
				else
				{
					if (po->align)
						fprintf(fout,
								"%-*s%s %s\n",
						fieldMaxLen - fs_len, fieldNames[j], po->fieldSep,
								buf);
					else
						fprintf(fout, "%s%s%s\n", fieldNames[j], po->fieldSep, buf);
				}
			}
			else
			{
				if (!po->html3)
				{
					fputs(buf, fout);
			efield:
					if ((j + 1) < nFields)
						fputs(po->fieldSep, fout);
					else
						fputc('\n', fout);
				}
			}
		}
	}
}


static char *
do_header(FILE *fout, PQprintOpt *po, const int nFields, int *fieldMax,
		  char **fieldNames, unsigned char *fieldNotNum,
		  const int fs_len, PGresult *res)
{

	int			j;				/* for loop index */
	char	   *border = NULL;

	if (po->html3)
		fputs("<tr>", fout);
	else
	{
		int			j;			/* for loop index */
		int			tot = 0;
		int			n = 0;
		char	   *p = NULL;

		for (; n < nFields; n++)
			tot += fieldMax[n] + fs_len + (po->standard ? 2 : 0);
		if (po->standard)
			tot += fs_len * 2 + 2;
		border = malloc(tot + 1);
		if (!border)
		{
			perror("malloc");
			exit(1);
		}
		p = border;
		if (po->standard)
		{
			char	   *fs = po->fieldSep;

			while (*fs++)
				*p++ = '+';
		}
		for (j = 0; j < nFields; j++)
		{
			int			len;

			for (len = fieldMax[j] + (po->standard ? 2 : 0); len--; *p++ = '-');
			if (po->standard || (j + 1) < nFields)
			{
				char	   *fs = po->fieldSep;

				while (*fs++)
					*p++ = '+';
			}
		}
		*p = '\0';
		if (po->standard)
			fprintf(fout, "%s\n", border);
	}
	if (po->standard)
		fputs(po->fieldSep, fout);
	for (j = 0; j < nFields; j++)
	{
		char	   *s = PQfname(res, j);

		if (po->html3)
		{
			fprintf(fout, "<th align=%s>%s</th>",
					fieldNotNum[j] ? "left" : "right", fieldNames[j]);
		}
		else
		{
			int			n = strlen(s);

			if (n > fieldMax[j])
				fieldMax[j] = n;
			if (po->standard)
				fprintf(fout,
						fieldNotNum[j] ? " %-*s " : " %*s ",
						fieldMax[j], s);
			else
				fprintf(fout, fieldNotNum[j] ? "%-*s" : "%*s", fieldMax[j], s);
			if (po->standard || (j + 1) < nFields)
				fputs(po->fieldSep, fout);
		}
	}
	if (po->html3)
		fputs("</tr>\n", fout);
	else
		fprintf(fout, "\n%s\n", border);
	return border;
}


static void
output_row(FILE *fout, PQprintOpt *po, const int nFields, char **fields,
		   unsigned char *fieldNotNum, int *fieldMax, char *border,
		   const int row_index)
{

	int			field_index;	/* for loop index */

	if (po->html3)
		fputs("<tr>", fout);
	else if (po->standard)
		fputs(po->fieldSep, fout);
	for (field_index = 0; field_index < nFields; field_index++)
	{
		char	   *p = fields[row_index * nFields + field_index];

		if (po->html3)
			fprintf(fout, "<td align=%s>%s</td>",
				fieldNotNum[field_index] ? "left" : "right", p ? p : "");
		else
		{
			fprintf(fout,
					fieldNotNum[field_index] ?
					(po->standard ? " %-*s " : "%-*s") :
					(po->standard ? " %*s " : "%*s"),
					fieldMax[field_index],
					p ? p : "");
			if (po->standard || field_index + 1 < nFields)
				fputs(po->fieldSep, fout);
		}
		if (p)
			free(p);
	}
	if (po->html3)
		fputs("</tr>", fout);
	else if (po->standard)
		fprintf(fout, "\n%s", border);
	fputc('\n', fout);
}


/* simply send out max-length number of filler characters to fp */

static void
fill(int length, int max, char filler, FILE *fp)
{
	int			count;

	count = max - length;
	while (count-- >= 0)
		putc(filler, fp);
}
