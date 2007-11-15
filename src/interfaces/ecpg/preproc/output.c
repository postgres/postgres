/* $PostgreSQL: pgsql/src/interfaces/ecpg/preproc/output.c,v 1.23 2007/11/15 21:14:45 momjian Exp $ */

#include "postgres_fe.h"

#include "extern.h"

static void output_escaped_str(char *cmd, bool quoted);

void
output_line_number(void)
{
	char	   *line = hashline_number();

	fprintf(yyout, "%s", line);
	free(line);
}

void
output_simple_statement(char *stmt)
{
	output_escaped_str(stmt, false);
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
		char	   *line = mm_alloc(strlen("\n#line %d \"%s\"\n") + sizeof(int) * CHAR_BIT * 10 / 3 + strlen(input_filename));

		sprintf(line, "\n#line %d \"%s\"\n", yylineno, input_filename);

		return line;
	}

	return EMPTY;
}

void
output_statement(char *stmt, int whenever_mode, enum ECPG_statement_type st)
{

	fprintf(yyout, "{ ECPGdo(__LINE__, %d, %d, %s, %d, ", compat, force_indicator, connection ? connection : "NULL", questionmarks);
	if (st == ECPGst_normal)
	{
		if (auto_prepare)
			fputs("ECPGst_prepnormal, \"", yyout);
		else
			fputs("ECPGst_normal, \"", yyout);

		output_escaped_str(stmt, false);
		fputs("\", ", yyout);
	}
	else
		fprintf(yyout, "%d, %s, ", st, stmt);

	/* dump variables to C file */
	dump_variables(argsinsert, 1);
	fputs("ECPGt_EOIT, ", yyout);
	dump_variables(argsresult, 1);
	fputs("ECPGt_EORT);", yyout);
	reset_variables();

	whenever_action(whenever_mode | 2);
	free(stmt);
	if (connection != NULL)
		free(connection);
}

void
output_prepare_statement(char *name, char *stmt)
{
	fprintf(yyout, "{ ECPGprepare(__LINE__, %s, %d, ", connection ? connection : "NULL", questionmarks);
	output_escaped_str(name, true);
	fputs(", ", yyout);
	output_escaped_str(stmt, true);
	fputs(");", yyout);
	whenever_action(2);
	free(name);
	if (connection != NULL)
		free(connection);
}

void
output_deallocate_prepare_statement(char *name)
{
	const char *con = connection ? connection : "NULL";

	if (strcmp(name, "all"))
	{
		fprintf(yyout, "{ ECPGdeallocate(__LINE__, %d, %s, ", compat, con);
		output_escaped_str(name, true);
		fputs(");", yyout);
	}
	else
		fprintf(yyout, "{ ECPGdeallocate_all(__LINE__, %d, %s);", compat, con);

	whenever_action(2);
	free(name);
	if (connection != NULL)
		free(connection);
}

static void
output_escaped_str(char *str, bool quoted)
{
	int			i = 0;
	int			len = strlen(str);

	if (quoted && str[0] == '\"' && str[len - 1] == '\"')		/* do not escape quotes
																 * at beginning and end
																 * if quoted string */
	{
		i = 1;
		len--;
		fputs("\"", yyout);
	}

	/* output this char by char as we have to filter " and \n */
	for (; i < len; i++)
	{
		if (str[i] == '"')
			fputs("\\\"", yyout);
		else if (str[i] == '\n')
			fputs("\\\n", yyout);
		else if (str[i] == '\\')
			fputs("\\\\", yyout);
		else if (str[i] == '\r' && str[i + 1] == '\n')
		{
			fputs("\\\r\n", yyout);
			i++;
		}
		else
			fputc(str[i], yyout);
	}

	if (quoted && str[0] == '\"' && str[len] == '\"')
		fputs("\"", yyout);
}
