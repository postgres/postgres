%top{
/*
 * Scanner for plan advice
 *
 * Copyright (c) 2000-2026, PostgreSQL Global Development Group
 *
 * contrib/pg_plan_advice/pgpa_scanner.l
 */
#include "postgres.h"

#include "common/string.h"
#include "nodes/miscnodes.h"
#include "parser/scansup.h"

#include "pgpa_ast.h"
#include "pgpa_parser.h"

/*
 * Extra data that we pass around when during scanning.
 *
 * 'litbuf' is used to implement the <xd> exclusive state, which handles
 * double-quoted identifiers.
 */
typedef struct pgpa_yy_extra_type
{
	StringInfoData	litbuf;
} pgpa_yy_extra_type;

}

%{
/* LCOV_EXCL_START */

#define YY_DECL \
	extern int pgpa_yylex(union YYSTYPE *yylval_param, List **result, \
						  char **parse_error_msg_p, yyscan_t yyscanner)

/* No reason to constrain amount of data slurped */
#define YY_READ_BUF_SIZE 16777216

/* Avoid exit() on fatal scanner errors (a bit ugly -- see yy_fatal_error) */
#undef fprintf
#define fprintf(file, fmt, msg)  fprintf_to_ereport(fmt, msg)

static void
fprintf_to_ereport(const char *fmt, const char *msg)
{
	ereport(ERROR, (errmsg_internal("%s", msg)));
}
%}

%option reentrant
%option bison-bridge
%option 8bit
%option never-interactive
%option nodefault
%option noinput
%option nounput
%option noyywrap
%option noyyalloc
%option noyyrealloc
%option noyyfree
%option warn
%option prefix="pgpa_yy"
%option extra-type="pgpa_yy_extra_type *"

/*
 * What follows is a severely stripped-down version of the core scanner. We
 * only care about recognizing identifiers with or without identifier quoting
 * (i.e. double-quoting), decimal integers, and a small handful of other
 * things. Keep these rules in sync with src/backend/parser/scan.l. As in that
 * file, we use an exclusive state called 'xc' for C-style comments, and an
 * exclusive state called 'xd' for double-quoted identifiers.
 */
%x xc
%x xd

ident_start		[A-Za-z\200-\377_]
ident_cont		[A-Za-z\200-\377_0-9\$]

identifier		{ident_start}{ident_cont}*

decdigit		[0-9]
decinteger		{decdigit}(_?{decdigit})*

space			[ \t\n\r\f\v]
whitespace		{space}+

dquote			\"
xdstart			{dquote}
xdstop			{dquote}
xddouble		{dquote}{dquote}
xdinside		[^"]+

xcstart			\/\*
xcstop			\*+\/
xcinside		[^*/]+

%%

{whitespace}	{ /* ignore */ }

{identifier}	{
					char   *str;
					bool	fail;
					pgpa_advice_tag_type	tag;

					/*
					 * Unlike the core scanner, we don't truncate identifiers
					 * here. There is no obvious reason to do so.
					 */
					str = downcase_identifier(yytext, yyleng, false, false);
					yylval->str = str;

					/*
					 * If it's not a tag, just return TOK_IDENT; else, return
					 * a token type based on how further parsing should
					 * proceed.
					 */
					tag = pgpa_parse_advice_tag(str, &fail);
					if (fail)
						return TOK_IDENT;
					else if (tag == PGPA_TAG_JOIN_ORDER)
						return TOK_TAG_JOIN_ORDER;
					else if (tag == PGPA_TAG_INDEX_SCAN ||
							 tag == PGPA_TAG_INDEX_ONLY_SCAN)
						return TOK_TAG_INDEX;
					else if (tag == PGPA_TAG_SEQ_SCAN ||
							 tag == PGPA_TAG_TID_SCAN ||
							 tag == PGPA_TAG_BITMAP_HEAP_SCAN ||
							 tag == PGPA_TAG_NO_GATHER)
						return TOK_TAG_SIMPLE;
					else
						return TOK_TAG_GENERIC;
				}

{decinteger}	{
					char   *endptr;

					errno = 0;
					yylval->integer = strtoint(yytext, &endptr, 10);
					if (*endptr != '\0' || errno == ERANGE)
						pgpa_yyerror(result, parse_error_msg_p, yyscanner,
									 "integer out of range");
					return TOK_INTEGER;
				}

{xcstart}		{
					BEGIN(xc);
				}

{xdstart}		{
					BEGIN(xd);
					resetStringInfo(&yyextra->litbuf);
				}

.				{ return yytext[0]; }

<xc>{xcstop}	{
					BEGIN(INITIAL);
				}

<xc>{xcinside}	{
					/* discard multiple characters without slash or asterisk */
				}

<xc>.			{
					/*
					 * Discard any single character. flex prefers longer
					 * matches, so this rule will never be picked when we could
					 * have matched xcstop.
					 *
					 * NB: At present, we don't bother to support nested
					 * C-style comments here, but this logic could be extended
					 * if that restriction poses a problem.
					 */
				}

<xc><<EOF>>		{
					BEGIN(INITIAL);
					pgpa_yyerror(result, parse_error_msg_p, yyscanner,
								 "unterminated comment");
				}

<xd>{xdstop}	{
					BEGIN(INITIAL);
					if (yyextra->litbuf.len == 0)
						pgpa_yyerror(result, parse_error_msg_p, yyscanner,
									 "zero-length delimited identifier");
					yylval->str = pstrdup(yyextra->litbuf.data);
					return TOK_IDENT;
				}

<xd>{xddouble}	{
					appendStringInfoChar(&yyextra->litbuf, '"');
				}

<xd>{xdinside}	{
					appendBinaryStringInfo(&yyextra->litbuf, yytext, yyleng);
				}

<xd><<EOF>>		{
					BEGIN(INITIAL);
					pgpa_yyerror(result, parse_error_msg_p, yyscanner,
								 "unterminated quoted identifier");
				}

%%

/* LCOV_EXCL_STOP */

/*
 * Handler for errors while scanning or parsing advice.
 *
 * bison passes the error message to us via 'message', and the context is
 * available via the 'yytext' macro. We assemble those values into a final
 * error text and then arrange to pass it back to the caller of pgpa_yyparse()
 * by storing it into *parse_error_msg_p.
 */
void
pgpa_yyerror(List **result, char **parse_error_msg_p, yyscan_t yyscanner,
			 const char *message)
{
	struct yyguts_t *yyg = (struct yyguts_t *) yyscanner;	/* needed for yytext
															 * macro */


	/* report only the first error in a parse operation */
	if (*parse_error_msg_p)
		return;

	if (yytext[0])
		*parse_error_msg_p = psprintf("%s at or near \"%s\"", message, yytext);
	else
		*parse_error_msg_p = psprintf("%s at end of input", message);
}

/*
 * Initialize the advice scanner.
 *
 * This should be called before parsing begins.
 */
void
pgpa_scanner_init(const char *str, yyscan_t *yyscannerp)
{
	yyscan_t	yyscanner;
	pgpa_yy_extra_type	*yyext = palloc0_object(pgpa_yy_extra_type);

	if (yylex_init(yyscannerp) != 0)
		elog(ERROR, "yylex_init() failed: %m");

	yyscanner = *yyscannerp;

	initStringInfo(&yyext->litbuf);
	pgpa_yyset_extra(yyext, yyscanner);

	yy_scan_string(str, yyscanner);
}


/*
 * Shut down the advice scanner.
 *
 * This should be called after parsing is complete.
 */
void
pgpa_scanner_finish(yyscan_t yyscanner)
{
	yylex_destroy(yyscanner);
}

/*
 * Interface functions to make flex use palloc() instead of malloc().
 * It'd be better to make these static, but flex insists otherwise.
 */

void *
yyalloc(yy_size_t size, yyscan_t yyscanner)
{
	return palloc(size);
}

void *
yyrealloc(void *ptr, yy_size_t size, yyscan_t yyscanner)
{
	if (ptr)
		return repalloc(ptr, size);
	else
		return palloc(size);
}

void
yyfree(void *ptr, yyscan_t yyscanner)
{
	if (ptr)
		pfree(ptr);
}
