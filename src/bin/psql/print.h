#ifndef PRINT_H
#define PRINT_H

#include <config.h>
#include <c.h>

#include <stdio.h>
#include <libpq-fe.h>

enum printFormat {
    PRINT_NOTHING = 0, /* to make sure someone initializes this */
    PRINT_UNALIGNED,
    PRINT_ALIGNED,
    PRINT_HTML,
    PRINT_LATEX
    /* add your favourite output format here ... */
};


typedef struct _printTableOpt {
    enum printFormat format;  /* one of the above */
    bool    expanded;         /* expanded/vertical output (if supported by output format) */
    bool    pager;            /* use pager for output (if to stdout and stdout is a tty) */
    bool    tuples_only;      /* don't output headers, row counts, etc. */
    unsigned short int  border;  /* Print a border around the table. 0=none, 1=dividing lines, 2=full */
    char    *fieldSep;        /* field separator for unaligned text mode */
    char    *tableAttr;       /* attributes for HTML <table ...> */
} printTableOpt;


/*
 * Use this to print just any table in the supported formats.
 * - title is just any string (NULL is fine)
 * - headers is the column headings (NULL ptr terminated). It must be given and
 *   complete since the column count is generated from this.
 * - cells are the data cells to be printed. Now you know why the correct
 *   column count is important
 * - footers are lines to be printed below the table
 * - align is an 'l' or an 'r' for every column, if the output format needs it.
 *   (You must specify this long enough. Otherwise anything could happen.)
*/
void
printTable(const char * title, char ** headers, char ** cells, char ** footers,
	   const char * align,
	   const printTableOpt * opt, FILE * fout);



typedef struct _printQueryOpt {
    printTableOpt topt;       /* the options above */
    char * nullPrint;         /* how to print null entities */
    bool quote;               /* quote all values as much as possible */
    char * title;             /* override title */
    char ** footers;          /* override footer (default is "(xx rows)") */
} printQueryOpt;

/*
 * Use this to print query results
 *
 * It calls the printTable above with all the things set straight.
 */
void
printQuery(PGresult * result, const printQueryOpt * opt, FILE * fout);


#endif /* PRINT_H */
