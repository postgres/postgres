/* New main for ecpg, the PostgreSQL embedded SQL precompiler. */
/* (C) Michael Meskes <meskes@debian.org> Feb 5th, 1998 */
/* Placed under the same copyright as PostgresSQL */

#include <stdio.h>

extern void lex_init(void);
int yyparse (void);

int main(int argc, char *argv[])
{
    lex_init();
    fprintf(stdout, "/* These two include files are added by the preprocessor */\n#include <ecpgtype.h>\n#include <ecpglib.h>\n");
    yyparse();
    return(0);
}
