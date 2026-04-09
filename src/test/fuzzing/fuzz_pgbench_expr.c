/*-------------------------------------------------------------------------
 *
 * fuzz_pgbench_expr.c
 *    Fuzzing harness for the pgbench expression parser
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *    src/test/fuzzing/fuzz_pgbench_expr.c
 *
 * This harness feeds arbitrary byte sequences to the pgbench expression
 * parser (expr_yyparse).  The parser exercises a Bison grammar and Flex
 * lexer that handle arithmetic expressions, function calls, boolean
 * operators, and CASE expressions.
 *
 * The pgbench expression parser normally calls syntax_error() on any
 * parse error, which calls exit(1).  This harness provides replacement
 * definitions of syntax_error(), strtoint64(), and strtodouble() so
 * that the generated parser and lexer objects can link without pulling
 * in pgbench.c.  Our syntax_error() uses longjmp to recover rather
 * than exiting.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <setjmp.h>
#include <stdio.h>

#include "pgbench.h"
#include "fe_utils/psqlscan.h"

static sigjmp_buf fuzz_jmp_buf;

static void free_pgbench_expr(PgBenchExpr *expr);

static const PsqlScanCallbacks fuzz_callbacks = {
	NULL,						/* no variable lookup needed */
};

int			LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/*
 * Replacement for pgbench.c's syntax_error().  Instead of calling exit(),
 * we longjmp back to the fuzzer's recovery point.
 */
void
syntax_error(const char *source, int lineno, const char *line,
			 const char *command, const char *msg,
			 const char *more, int column)
{
	siglongjmp(fuzz_jmp_buf, 1);
}

/*
 * Replacement for pgbench.c's strtoint64().
 */
bool
strtoint64(const char *str, bool errorOK, int64 *result)
{
	char	   *end;

	errno = 0;
	*result = strtoi64(str, &end, 10);

	if (errno == ERANGE || errno != 0 || end == str || *end != '\0')
		return false;
	return true;
}

/*
 * Replacement for pgbench.c's strtodouble().
 */
bool
strtodouble(const char *str, bool errorOK, double *dv)
{
	char	   *end;

	errno = 0;
	*dv = strtod(str, &end);

	if (errno == ERANGE || errno != 0 || end == str || *end != '\0')
		return false;
	return true;
}

/*
 * Recursively free a PgBenchExpr tree.
 */
static void
free_pgbench_expr(PgBenchExpr *expr)
{
	PgBenchExprLink *link;
	PgBenchExprLink *next;

	if (expr == NULL)
		return;

	switch (expr->etype)
	{
		case ENODE_CONSTANT:
			break;
		case ENODE_VARIABLE:
			pg_free(expr->u.variable.varname);
			break;
		case ENODE_FUNCTION:
			for (link = expr->u.function.args; link != NULL; link = next)
			{
				next = link->next;
				free_pgbench_expr(link->expr);
				pg_free(link);
			}
			break;
	}

	pg_free(expr);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char	   *str;
	PsqlScanState sstate;
	yyscan_t	yyscanner;
	PgBenchExpr *result = NULL;

	if (size == 0)
		return 0;

	/* expr_yyparse needs a NUL-terminated string */
	str = malloc(size + 1);
	if (!str)
		return 0;
	memcpy(str, data, size);
	str[size] = '\0';

	sstate = psql_scan_create(&fuzz_callbacks);
	psql_scan_setup(sstate, str, (int) size, 0, true);

	yyscanner = expr_scanner_init(sstate, "fuzz", 1, 0, "\\set");

	if (sigsetjmp(fuzz_jmp_buf, 0) == 0)
	{
		(void) expr_yyparse(&result, yyscanner);
	}

	/* Clean up regardless of success or longjmp */
	expr_scanner_finish(yyscanner);
	psql_scan_finish(sstate);
	psql_scan_destroy(sstate);

	if (result)
		free_pgbench_expr(result);

	free(str);
	return 0;
}

#ifdef STANDALONE_FUZZ_TARGET
int
main(int argc, char **argv)
{
	int			i;
	int			ret = 0;

	for (i = 1; i < argc; i++)
	{
		FILE	   *f = fopen(argv[i], "rb");
		long		len;
		uint8_t    *buf;
		size_t		n_read;

		if (!f)
		{
			fprintf(stderr, "%s: could not open %s: %m\n", argv[0], argv[i]);
			ret = 1;
			continue;
		}

		fseek(f, 0, SEEK_END);
		len = ftell(f);
		fseek(f, 0, SEEK_SET);

		if (len < 0)
		{
			fprintf(stderr, "%s: could not determine size of %s\n",
					argv[0], argv[i]);
			fclose(f);
			ret = 1;
			continue;
		}

		buf = malloc(len);
		if (!buf)
		{
			fprintf(stderr, "%s: out of memory\n", argv[0]);
			fclose(f);
			return 1;
		}

		n_read = fread(buf, 1, len, f);
		fclose(f);

		LLVMFuzzerTestOneInput(buf, n_read);
		free(buf);
	}

	return ret;
}
#endif							/* STANDALONE_FUZZ_TARGET */
