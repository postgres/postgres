/* $PostgreSQL: pgsql/src/interfaces/ecpg/preproc/output.c,v 1.19 2006/10/04 00:30:12 momjian Exp $ */

#include "postgres_fe.h"

#include "extern.h"

static void output_escaped_str(char *cmd);

void
output_line_number(void)
{
	char	   *line = hashline_number();

	/* output_escaped_str(line); */
	fprintf(yyout, "%s", line);
	free(line);
}

void
output_simple_statement(char *stmt)
{
	output_escaped_str(stmt);
	output_line_number();
	free(stmt);
}

/*
 * store the whenever action here
 */
struct when when_error,
			when_nf,
			when_warn;

static void
print_action(struct when * w)
{
	switch (w->code)
	{
		case W_SQLPRINT:
			fprintf(yyout, "sqlprint();");
			break;
		case W_GOTO:
			fprintf(yyout, "goto %s;", w->command);
			break;
		case W_DO:
			fprintf(yyout, "%s;", w->command);
			break;
		case W_STOP:
			fprintf(yyout, "exit (1);");
			break;
		case W_BREAK:
			fprintf(yyout, "break;");
			break;
		default:
			fprintf(yyout, "{/* %d not implemented yet */}", w->code);
			break;
	}
}

void
whenever_action(int mode)
{
	if ((mode & 1) == 1 && when_nf.code != W_NOTHING)
	{
		output_line_number();
		fprintf(yyout, "\nif (sqlca.sqlcode == ECPG_NOT_FOUND) ");
		print_action(&when_nf);
	}
	if (when_warn.code != W_NOTHING)
	{
		output_line_number();
		fprintf(yyout, "\nif (sqlca.sqlwarn[0] == 'W') ");
		print_action(&when_warn);
	}
	if (when_error.code != W_NOTHING)
	{
		output_line_number();
		fprintf(yyout, "\nif (sqlca.sqlcode < 0) ");
		print_action(&when_error);
	}

	if ((mode & 2) == 2)
		fputc('}', yyout);

	output_line_number();
}

char *
hashline_number(void)
{
	/* do not print line numbers if we are in debug mode */
	if (input_filename
#ifdef YYDEBUG
		&& !yydebug
#endif
		)
	{
		char	   *line = mm_alloc(strlen("\n#line %d \"%s\"\n") + 21 + strlen(input_filename));

		sprintf(line, "\n#line %d \"%s\"\n", yylineno, input_filename);

		return line;
	}

	return EMPTY;
}

void
output_statement(char *stmt, int mode, char *con)
{
	fprintf(yyout, "{ ECPGdo(__LINE__, %d, %d, %s, \"", compat, force_indicator, con ? con : "NULL");
	output_escaped_str(stmt);
	fputs("\", ", yyout);

	/* dump variables to C file */
	dump_variables(argsinsert, 1);
	fputs("ECPGt_EOIT, ", yyout);
	dump_variables(argsresult, 1);
	fputs("ECPGt_EORT);", yyout);
	reset_variables();

	mode |= 2;
	whenever_action(mode);
	free(stmt);
	if (connection != NULL)
		free(connection);
}


static void
output_escaped_str(char *str)
{
	int			i,
				len = strlen(str);

	/* output this char by char as we have to filter " and \n */
	for (i = 0; i < len; i++)
	{
		if (str[i] == '"')
			fputs("\\\"", yyout);
		else if (str[i] == '\n')
			fputs("\\\n", yyout);
		else if (str[i] == '\r' && str[i + 1] == '\n')
		{
			fputs("\\\r\n", yyout);
			i++;
		}
		else
			fputc(str[i], yyout);
	}
}
