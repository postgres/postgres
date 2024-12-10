/* src/interfaces/ecpg/preproc/output.c */

#include "postgres_fe.h"

#include "preproc_extern.h"

static void output_escaped_str(const char *str, bool quoted);

void
output_line_number(void)
{
	char	   *line = hashline_number();

	fprintf(base_yyout, "%s", line);
}

void
output_simple_statement(const char *stmt, int whenever_mode)
{
	output_escaped_str(stmt, false);
	if (whenever_mode)
		whenever_action(whenever_mode);
	output_line_number();
}


/*
 * store the whenever action here
 */
struct when when_error,
			when_nf,
			when_warn;

static void
print_action(struct when *w)
{
	switch (w->code)
	{
		case W_SQLPRINT:
			fprintf(base_yyout, "sqlprint();");
			break;
		case W_GOTO:
			fprintf(base_yyout, "goto %s;", w->command);
			break;
		case W_DO:
			fprintf(base_yyout, "%s;", w->command);
			break;
		case W_STOP:
			fprintf(base_yyout, "exit (1);");
			break;
		case W_BREAK:
			fprintf(base_yyout, "break;");
			break;
		case W_CONTINUE:
			fprintf(base_yyout, "continue;");
			break;
		default:
			fprintf(base_yyout, "{/* %d not implemented yet */}", w->code);
			break;
	}
}

void
whenever_action(int mode)
{
	if ((mode & 1) == 1 && when_nf.code != W_NOTHING)
	{
		output_line_number();
		fprintf(base_yyout, "\nif (sqlca.sqlcode == ECPG_NOT_FOUND) ");
		print_action(&when_nf);
	}
	if (when_warn.code != W_NOTHING)
	{
		output_line_number();
		fprintf(base_yyout, "\nif (sqlca.sqlwarn[0] == 'W') ");
		print_action(&when_warn);
	}
	if (when_error.code != W_NOTHING)
	{
		output_line_number();
		fprintf(base_yyout, "\nif (sqlca.sqlcode < 0) ");
		print_action(&when_error);
	}

	if ((mode & 2) == 2)
		fputc('}', base_yyout);

	output_line_number();
}

char *
hashline_number(void)
{
	/* do not print line numbers if we are in debug mode */
	if (input_filename
#ifdef YYDEBUG
		&& !base_yydebug
#endif
		)
	{
		/* "* 2" here is for escaping '\' and '"' below */
		char	   *line = loc_alloc(strlen("\n#line %d \"%s\"\n") + sizeof(int) * CHAR_BIT * 10 / 3 + strlen(input_filename) * 2);
		char	   *src,
				   *dest;

		sprintf(line, "\n#line %d \"", base_yylineno);
		src = input_filename;
		dest = line + strlen(line);
		while (*src)
		{
			if (*src == '\\' || *src == '"')
				*dest++ = '\\';
			*dest++ = *src++;
		}
		*dest = '\0';
		strcat(dest, "\"\n");

		return line;
	}

	return "";
}

static char *ecpg_statement_type_name[] = {
	"ECPGst_normal",
	"ECPGst_execute",
	"ECPGst_exec_immediate",
	"ECPGst_prepnormal",
	"ECPGst_prepare",
	"ECPGst_exec_with_exprlist"
};

void
output_statement(const char *stmt, int whenever_mode, enum ECPG_statement_type st)
{
	fprintf(base_yyout, "{ ECPGdo(__LINE__, %d, %d, %s, %d, ", compat, force_indicator, connection ? connection : "NULL", questionmarks);

	if (st == ECPGst_prepnormal && !auto_prepare)
		st = ECPGst_normal;

	/*
	 * In following cases, stmt is CSTRING or char_variable. They must be
	 * output directly. - prepared_name of EXECUTE without exprlist -
	 * execstring of EXECUTE IMMEDIATE
	 */
	fprintf(base_yyout, "%s, ", ecpg_statement_type_name[st]);
	if (st == ECPGst_execute || st == ECPGst_exec_immediate)
		fprintf(base_yyout, "%s, ", stmt);
	else
	{
		fputs("\"", base_yyout);
		output_escaped_str(stmt, false);
		fputs("\", ", base_yyout);
	}

	/* dump variables to C file */
	dump_variables(argsinsert, 1);
	argsinsert = NULL;
	fputs("ECPGt_EOIT, ", base_yyout);
	dump_variables(argsresult, 1);
	argsresult = NULL;
	fputs("ECPGt_EORT);", base_yyout);

	whenever_action(whenever_mode | 2);
}

void
output_prepare_statement(const char *name, const char *stmt)
{
	fprintf(base_yyout, "{ ECPGprepare(__LINE__, %s, %d, ", connection ? connection : "NULL", questionmarks);
	output_escaped_str(name, true);
	fputs(", ", base_yyout);
	output_escaped_str(stmt, true);
	fputs(");", base_yyout);
	whenever_action(2);
}

void
output_deallocate_prepare_statement(const char *name)
{
	const char *con = connection ? connection : "NULL";

	if (strcmp(name, "all") != 0)
	{
		fprintf(base_yyout, "{ ECPGdeallocate(__LINE__, %d, %s, ", compat, con);
		output_escaped_str(name, true);
		fputs(");", base_yyout);
	}
	else
		fprintf(base_yyout, "{ ECPGdeallocate_all(__LINE__, %d, %s);", compat, con);

	whenever_action(2);
}

static void
output_escaped_str(const char *str, bool quoted)
{
	int			i = 0;
	int			len = strlen(str);

	if (quoted && str[0] == '"' && str[len - 1] == '"') /* do not escape quotes
														 * at beginning and end
														 * if quoted string */
	{
		i = 1;
		len--;
		fputs("\"", base_yyout);
	}

	/* output this char by char as we have to filter " and \n */
	for (; i < len; i++)
	{
		if (str[i] == '"')
			fputs("\\\"", base_yyout);
		else if (str[i] == '\n')
			fputs("\\\n", base_yyout);
		else if (str[i] == '\\')
		{
			int			j = i;

			/*
			 * check whether this is a continuation line if it is, do not
			 * output anything because newlines are escaped anyway
			 */

			/* accept blanks after the '\' as some other compilers do too */
			do
			{
				j++;
			} while (str[j] == ' ' || str[j] == '\t');

			if ((str[j] != '\n') && (str[j] != '\r' || str[j + 1] != '\n')) /* not followed by a
																			 * newline */
				fputs("\\\\", base_yyout);
		}
		else if (str[i] == '\r' && str[i + 1] == '\n')
		{
			fputs("\\\r\n", base_yyout);
			i++;
		}
		else
			fputc(str[i], base_yyout);
	}

	if (quoted && str[0] == '"' && str[len] == '"')
		fputs("\"", base_yyout);
}
