#include <stdarg.h>

#include "postgres.h"
#include "extern.h"

void
output_line_number()
{
    if (input_filename)
       fprintf(yyout, "\n#line %d \"%s\"\n", yylineno, input_filename);
}

void
output_simple_statement(char *cmd)
{
	int i, j = strlen(cmd);;
	
	/* do this char by char as we have to filter '\"' */
	for (i = 0; i < j; i++) {
		if (cmd[i] != '"')
			fputc(cmd[i], yyout);
		else
			fputs("\\\"", yyout);
	}
	output_line_number();
        free(cmd);
}

/*
 * store the whenever action here
 */
struct when when_error, when_nf, when_warn;

static void
print_action(struct when *w)
{
	switch (w->code)
	{
		case W_SQLPRINT: fprintf(yyout, "sqlprint();");
                                 break;
		case W_GOTO:	 fprintf(yyout, "goto %s;", w->command);
				 break;
		case W_DO:	 fprintf(yyout, "%s;", w->command);
				 break;
		case W_STOP:	 fprintf(yyout, "exit (1);");
				 break;
		case W_BREAK:	 fprintf(yyout, "break;");
				 break;
		default:	 fprintf(yyout, "{/* %d not implemented yet */}", w->code);
				 break;
	}
}

void
whenever_action(int mode)
{
	if ((mode&1) ==  1 && when_nf.code != W_NOTHING)
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

	if ((mode&2) == 2)
		fputc('}', yyout);

	output_line_number();
}

char *
hashline_number(void)
{
    if (input_filename)
    {
	char* line = mm_alloc(strlen("\n#line %d \"%s\"\n") + 21 + strlen(input_filename));
	sprintf(line, "\n#line %d \"%s\"\n", yylineno, input_filename);

	return line;
    }

    return EMPTY;
}

void
output_statement(char * stmt, int mode, char *descriptor, char *con)
{
	int i, j = strlen(stmt);

	if (descriptor == NULL)
		fprintf(yyout, "{ ECPGdo(__LINE__, %s, \"", con ? con : "NULL");
	else
	        fprintf(yyout, "{ ECPGdo_descriptor(__LINE__, %s, \"%s\", \"",
	                        con ? con : "NULL", descriptor);

	/* do this char by char as we have to filter '\"' */
	for (i = 0; i < j; i++) {
		if (stmt[i] != '"')
			fputc(stmt[i], yyout);
		else
			fputs("\\\"", yyout);
	}

	if (descriptor == NULL)
	{
		fputs("\", ", yyout);
		
		/* dump variables to C file */
		dump_variables(argsinsert, 1);
		fputs("ECPGt_EOIT, ", yyout);
		dump_variables(argsresult, 1);
		fputs("ECPGt_EORT);", yyout);
		reset_variables();
	}
	else
		fputs("\");", yyout);
	
	mode |= 2;
	whenever_action(mode);
	free(stmt);
	if (descriptor != NULL)
		free(descriptor);
	if (connection != NULL)
		free(connection);
}

