/*-------------------------------------------------------------------------
 *
 * parser.c
 *		Main entry point/driver for PostgreSQL grammar
 *
 * Note that the grammar is not allowed to perform any table access
 * (since we need to be able to do basic parsing even while inside an
 * aborted transaction).  Therefore, the data structures returned by
 * the grammar are "raw" parsetrees that still need to be analyzed by
 * analyze.c and related files.
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/parser/parser.c,v 1.81 2009/07/14 20:24:10 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "parser/gramparse.h"
#include "parser/parser.h"


/*
 * raw_parser
 *		Given a query in string form, do lexical and grammatical analysis.
 *
 * Returns a list of raw (un-analyzed) parse trees.
 */
List *
raw_parser(const char *str)
{
	base_yyscan_t yyscanner;
	base_yy_extra_type yyextra;
	int			yyresult;

	/* initialize the flex scanner */
	yyscanner = scanner_init(str, &yyextra, ScanKeywords, NumScanKeywords);

	/* filtered_base_yylex() only needs this much initialization */
	yyextra.have_lookahead = false;

	/* initialize the bison parser */
	parser_init(&yyextra);

	/* Parse! */
	yyresult = base_yyparse(yyscanner);

	/* Clean up (release memory) */
	scanner_finish(yyscanner);

	if (yyresult)				/* error */
		return NIL;

	return yyextra.parsetree;
}


/*
 * pg_parse_string_token - get the value represented by a string literal
 *
 * Given the textual form of a SQL string literal, produce the represented
 * value as a palloc'd string.  It is caller's responsibility that the
 * passed string does represent one single string literal.
 *
 * We export this function to avoid having plpgsql depend on internal details
 * of the core grammar (such as the token code assigned to SCONST).
 */
char *
pg_parse_string_token(const char *token)
{
	base_yyscan_t yyscanner;
	base_yy_extra_type yyextra;
	int			ctoken;
	YYSTYPE		yylval;
	YYLTYPE		yylloc;

	yyscanner = scanner_init(token, &yyextra, ScanKeywords, NumScanKeywords);

	ctoken = base_yylex(&yylval, &yylloc, yyscanner);

	if (ctoken != SCONST)		/* caller error */
		elog(ERROR, "expected string constant, got token code %d", ctoken);

	scanner_finish(yyscanner);

	return yylval.str;
}


/*
 * Intermediate filter between parser and base lexer (base_yylex in scan.l).
 *
 * The filter is needed because in some cases the standard SQL grammar
 * requires more than one token lookahead.	We reduce these cases to one-token
 * lookahead by combining tokens here, in order to keep the grammar LALR(1).
 *
 * Using a filter is simpler than trying to recognize multiword tokens
 * directly in scan.l, because we'd have to allow for comments between the
 * words.  Furthermore it's not clear how to do it without re-introducing
 * scanner backtrack, which would cost more performance than this filter
 * layer does.
 */
int
filtered_base_yylex(YYSTYPE *lvalp, YYLTYPE *llocp, base_yyscan_t yyscanner)
{
	base_yy_extra_type *yyextra = pg_yyget_extra(yyscanner);
	int			cur_token;
	int			next_token;
	YYSTYPE		cur_yylval;
	YYLTYPE		cur_yylloc;

	/* Get next token --- we might already have it */
	if (yyextra->have_lookahead)
	{
		cur_token = yyextra->lookahead_token;
		*lvalp = yyextra->lookahead_yylval;
		*llocp = yyextra->lookahead_yylloc;
		yyextra->have_lookahead = false;
	}
	else
		cur_token = base_yylex(lvalp, llocp, yyscanner);

	/* Do we need to look ahead for a possible multiword token? */
	switch (cur_token)
	{
		case NULLS_P:

			/*
			 * NULLS FIRST and NULLS LAST must be reduced to one token
			 */
			cur_yylval = *lvalp;
			cur_yylloc = *llocp;
			next_token = base_yylex(lvalp, llocp, yyscanner);
			switch (next_token)
			{
				case FIRST_P:
					cur_token = NULLS_FIRST;
					break;
				case LAST_P:
					cur_token = NULLS_LAST;
					break;
				default:
					/* save the lookahead token for next time */
					yyextra->lookahead_token = next_token;
					yyextra->lookahead_yylval = *lvalp;
					yyextra->lookahead_yylloc = *llocp;
					yyextra->have_lookahead = true;
					/* and back up the output info to cur_token */
					*lvalp = cur_yylval;
					*llocp = cur_yylloc;
					break;
			}
			break;

		case WITH:

			/*
			 * WITH TIME must be reduced to one token
			 */
			cur_yylval = *lvalp;
			cur_yylloc = *llocp;
			next_token = base_yylex(lvalp, llocp, yyscanner);
			switch (next_token)
			{
				case TIME:
					cur_token = WITH_TIME;
					break;
				default:
					/* save the lookahead token for next time */
					yyextra->lookahead_token = next_token;
					yyextra->lookahead_yylval = *lvalp;
					yyextra->lookahead_yylloc = *llocp;
					yyextra->have_lookahead = true;
					/* and back up the output info to cur_token */
					*lvalp = cur_yylval;
					*llocp = cur_yylloc;
					break;
			}
			break;

		default:
			break;
	}

	return cur_token;
}
