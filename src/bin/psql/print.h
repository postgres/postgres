/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2016, PostgreSQL Global Development Group
 *
 * src/bin/psql/print.h
 */
#ifndef PRINT_H
#define PRINT_H

#include "libpq-fe.h"


enum printFormat
{
	PRINT_NOTHING = 0,			/* to make sure someone initializes this */
	PRINT_UNALIGNED,
	PRINT_ALIGNED,
	PRINT_WRAPPED,
	PRINT_HTML,
	PRINT_ASCIIDOC,
	PRINT_LATEX,
	PRINT_LATEX_LONGTABLE,
	PRINT_TROFF_MS
	/* add your favourite output format here ... */
};

typedef struct printTextLineFormat
{
	/* Line drawing characters to be used in various contexts */
	const char *hrule;			/* horizontal line character */
	const char *leftvrule;		/* left vertical line (+horizontal) */
	const char *midvrule;		/* intra-column vertical line (+horizontal) */
	const char *rightvrule;		/* right vertical line (+horizontal) */
} printTextLineFormat;

typedef enum printTextRule
{
	/* Additional context for selecting line drawing characters */
	PRINT_RULE_TOP,				/* top horizontal line */
	PRINT_RULE_MIDDLE,			/* intra-data horizontal line */
	PRINT_RULE_BOTTOM,			/* bottom horizontal line */
	PRINT_RULE_DATA				/* data line (hrule is unused here) */
} printTextRule;

typedef enum printTextLineWrap
{
	/* Line wrapping conditions */
	PRINT_LINE_WRAP_NONE,		/* No wrapping */
	PRINT_LINE_WRAP_WRAP,		/* Wraparound due to overlength line */
	PRINT_LINE_WRAP_NEWLINE		/* Newline in data */
} printTextLineWrap;

typedef struct printTextFormat
{
	/* A complete line style */
	const char *name;			/* for display purposes */
	printTextLineFormat lrule[4];		/* indexed by enum printTextRule */
	const char *midvrule_nl;	/* vertical line for continue after newline */
	const char *midvrule_wrap;	/* vertical line for wrapped data */
	const char *midvrule_blank; /* vertical line for blank data */
	const char *header_nl_left; /* left mark after newline */
	const char *header_nl_right;	/* right mark for newline */
	const char *nl_left;		/* left mark after newline */
	const char *nl_right;		/* right mark for newline */
	const char *wrap_left;		/* left mark after wrapped data */
	const char *wrap_right;		/* right mark for wrapped data */
	bool		wrap_right_border;		/* use right-hand border for wrap
										 * marks when border=0? */
} printTextFormat;

typedef enum unicode_linestyle
{
	UNICODE_LINESTYLE_SINGLE = 0,
	UNICODE_LINESTYLE_DOUBLE
} unicode_linestyle;

struct separator
{
	char	   *separator;
	bool		separator_zero;
};

typedef struct printTableOpt
{
	enum printFormat format;	/* see enum above */
	unsigned short int expanded;/* expanded/vertical output (if supported by
								 * output format); 0=no, 1=yes, 2=auto */
	unsigned short int border;	/* Print a border around the table. 0=none,
								 * 1=dividing lines, 2=full */
	unsigned short int pager;	/* use pager for output (if to stdout and
								 * stdout is a tty) 0=off 1=on 2=always */
	int			pager_min_lines;/* don't use pager unless there are at least
								 * this many lines */
	bool		tuples_only;	/* don't output headers, row counts, etc. */
	bool		start_table;	/* print start decoration, eg <table> */
	bool		stop_table;		/* print stop decoration, eg </table> */
	bool		default_footer; /* allow "(xx rows)" default footer */
	unsigned long prior_records;	/* start offset for record counters */
	const printTextFormat *line_style;	/* line style (NULL for default) */
	struct separator fieldSep;	/* field separator for unaligned text mode */
	struct separator recordSep; /* record separator for unaligned text mode */
	bool		numericLocale;	/* locale-aware numeric units separator and
								 * decimal marker */
	char	   *tableAttr;		/* attributes for HTML <table ...> */
	int			encoding;		/* character encoding */
	int			env_columns;	/* $COLUMNS on psql start, 0 is unset */
	int			columns;		/* target width for wrapped format */
	unicode_linestyle unicode_border_linestyle;
	unicode_linestyle unicode_column_linestyle;
	unicode_linestyle unicode_header_linestyle;
} printTableOpt;

/*
 * Table footers are implemented as a singly-linked list.
 *
 * This is so that you don't need to know the number of footers in order to
 * initialise the printTableContent struct, which is very convenient when
 * preparing complex footers (as in describeOneTableDetails).
 */
typedef struct printTableFooter
{
	char	   *data;
	struct printTableFooter *next;
} printTableFooter;

/*
 * The table content struct holds all the information which will be displayed
 * by printTable().
 */
typedef struct printTableContent
{
	const printTableOpt *opt;
	const char *title;			/* May be NULL */
	int			ncolumns;		/* Specified in Init() */
	int			nrows;			/* Specified in Init() */
	const char **headers;		/* NULL-terminated array of header strings */
	const char **header;		/* Pointer to the last added header */
	const char **cells;			/* NULL-terminated array of cell content
								 * strings */
	const char **cell;			/* Pointer to the last added cell */
	long		cellsadded;		/* Number of cells added this far */
	bool	   *cellmustfree;	/* true for cells that need to be free()d */
	printTableFooter *footers;	/* Pointer to the first footer */
	printTableFooter *footer;	/* Pointer to the last added footer */
	char	   *aligns;			/* Array of alignment specifiers; 'l' or 'r',
								 * one per column */
	char	   *align;			/* Pointer to the last added alignment */
} printTableContent;

typedef struct printQueryOpt
{
	printTableOpt topt;			/* the options above */
	char	   *nullPrint;		/* how to print null entities */
	char	   *title;			/* override title */
	char	  **footers;		/* override footer (default is "(xx rows)") */
	bool		translate_header;		/* do gettext on column headers */
	const bool *translate_columns;		/* translate_columns[i-1] => do
										 * gettext on col i */
	int			n_translate_columns;	/* length of translate_columns[] */
} printQueryOpt;


extern const printTextFormat pg_asciiformat;
extern const printTextFormat pg_asciiformat_old;
extern printTextFormat pg_utf8format;	/* ideally would be const, but... */


extern void disable_sigpipe_trap(void);
extern void restore_sigpipe_trap(void);
extern void set_sigpipe_trap_state(bool ignore);

extern FILE *PageOutput(int lines, const printTableOpt *topt);
extern void ClosePager(FILE *pagerpipe);

extern void html_escaped_print(const char *in, FILE *fout);

extern void printTableInit(printTableContent *const content,
			   const printTableOpt *opt, const char *title,
			   const int ncolumns, const int nrows);
extern void printTableAddHeader(printTableContent *const content,
					char *header, const bool translate, const char align);
extern void printTableAddCell(printTableContent *const content,
				  char *cell, const bool translate, const bool mustfree);
extern void printTableAddFooter(printTableContent *const content,
					const char *footer);
extern void printTableSetFooter(printTableContent *const content,
					const char *footer);
extern void printTableCleanup(printTableContent *const content);
extern void printTable(const printTableContent *cont,
		   FILE *fout, bool is_pager, FILE *flog);
extern void printQuery(const PGresult *result, const printQueryOpt *opt,
		   FILE *fout, bool is_pager, FILE *flog);

extern void setDecimalLocale(void);
extern const printTextFormat *get_line_style(const printTableOpt *opt);
extern void refresh_utf8format(const printTableOpt *opt);

#ifndef __CYGWIN__
#define DEFAULT_PAGER "more"
#else
#define DEFAULT_PAGER "less"
#endif

#endif   /* PRINT_H */
