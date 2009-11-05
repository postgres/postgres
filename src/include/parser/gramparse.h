/*-------------------------------------------------------------------------
 *
 * gramparse.h
 *		Shared definitions for the "raw" parser (flex and bison phases only)
 *
 * NOTE: this file is only meant to be included in the core parsing files,
 * ie, parser.c, gram.y, scan.l, and keywords.c.  Definitions that are needed
 * outside the core parser should be in parser.h.
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/parser/gramparse.h,v 1.49 2009/11/05 23:24:26 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef GRAMPARSE_H
#define GRAMPARSE_H

#include "nodes/parsenodes.h"
#include "parser/keywords.h"

/*
 * We track token locations in terms of byte offsets from the start of the
 * source string, not the column number/line number representation that
 * bison uses by default.  Also, to minimize overhead we track only one
 * location (usually the first token location) for each construct, not
 * the beginning and ending locations as bison does by default.  It's
 * therefore sufficient to make YYLTYPE an int.
 */
#define YYLTYPE  int

/*
 * After defining YYLTYPE, it's safe to include gram.h.
 */
#include "parser/gram.h"

/*
 * The YY_EXTRA data that a flex scanner allows us to pass around.  Private
 * state needed for raw parsing/lexing goes here.
 */
typedef struct base_yy_extra_type
{
	/*
	 * The string the lexer is physically scanning.  We keep this mainly so
	 * that we can cheaply compute the offset of the current token (yytext).
	 */
	char	   *scanbuf;
	Size		scanbuflen;

	/*
	 * The keyword list to use.
	 */
	const ScanKeyword *keywords;
	int			num_keywords;

	/*
	 * literalbuf is used to accumulate literal values when multiple rules
	 * are needed to parse a single literal.  Call startlit() to reset buffer
	 * to empty, addlit() to add text.  NOTE: the string in literalbuf is
	 * NOT necessarily null-terminated, but there always IS room to add a
	 * trailing null at offset literallen.  We store a null only when we
	 * need it.
	 */
	char	   *literalbuf;		/* palloc'd expandable buffer */
	int			literallen;		/* actual current string length */
	int			literalalloc;	/* current allocated buffer size */

	int			xcdepth;		/* depth of nesting in slash-star comments */
	char	   *dolqstart;		/* current $foo$ quote start string */

	/* first part of UTF16 surrogate pair for Unicode escapes */
	int32		utf16_first_part;

	/* state variables for literal-lexing warnings */
	bool		warn_on_first_escape;
	bool		saw_non_ascii;

	/*
	 * State variables for filtered_base_yylex().
	 */
	bool		have_lookahead;		/* is lookahead info valid? */
	int			lookahead_token;	/* one-token lookahead */
	YYSTYPE		lookahead_yylval;	/* yylval for lookahead token */
	YYLTYPE		lookahead_yylloc;	/* yylloc for lookahead token */

	/*
	 * State variables that belong to the grammar, not the lexer.  It's
	 * simpler to keep these here than to invent a separate structure.
	 * These fields are unused/undefined if the lexer is invoked on its own.
	 */

	List	   *parsetree;		/* final parse result is delivered here */
} base_yy_extra_type;

/*
 * The type of yyscanner is opaque outside scan.l.
 */
typedef void *base_yyscan_t;

/*
 * In principle we should use yyget_extra() to fetch the yyextra field
 * from a yyscanner struct.  However, flex always puts that field first,
 * and this is sufficiently performance-critical to make it seem worth
 * cheating a bit to use an inline macro.
 */
#define pg_yyget_extra(yyscanner) (*((base_yy_extra_type **) (yyscanner)))


/* from parser.c */
extern int	filtered_base_yylex(YYSTYPE *lvalp, YYLTYPE *llocp,
								base_yyscan_t yyscanner);

/* from scan.l */
extern base_yyscan_t scanner_init(const char *str,
								  base_yy_extra_type *yyext,
								  const ScanKeyword *keywords,
								  int num_keywords);
extern void scanner_finish(base_yyscan_t yyscanner);
extern int	base_yylex(YYSTYPE *lvalp, YYLTYPE *llocp,
					   base_yyscan_t yyscanner);
extern int	scanner_errposition(int location, base_yyscan_t yyscanner);
extern void scanner_yyerror(const char *message, base_yyscan_t yyscanner);

/* from gram.y */
extern void parser_init(base_yy_extra_type *yyext);
extern int	base_yyparse(base_yyscan_t yyscanner);

#endif   /* GRAMPARSE_H */
