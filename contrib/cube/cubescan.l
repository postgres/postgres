%{
/* 
** A scanner for EMP-style numeric ranges 
*/

#include "postgres.h"

#include "buffer.h"

/* Avoid exit() on fatal scanner errors (a bit ugly -- see yy_fatal_error) */
#define fprintf(file, fmt, msg)  ereport(ERROR, (errmsg_internal("%s", msg)))


/* flex screws a couple symbols when used with the -P option; fix those */
#define YY_DECL int cube_yylex YY_PROTO(( void )); \
int cube_yylex YY_PROTO(( void ))
#define yylval cube_yylval


/* redefined YY_INPUT reads byte-wise from the memory area defined in buffer.c */
#undef YY_INPUT
#define YY_INPUT(buf,result,max_size) \
{ \
        int c = read_parse_buffer(); \
        result = (c == '\0') ?  YY_NULL : (buf[0] = c, 1); \
}

void cube_flush_scanner_buffer(void); 
%}

%option 8bit
%option never-interactive
%option nounput
%option noyywrap


n            [0-9]+
integer      [+-]?{n}
real         [+-]?({n}\.{n}?|\.{n})
float        ({integer}|{real})([eE]{integer})?

%%

{float}      yylval = yytext; return FLOAT;
\[           yylval = "("; return O_BRACKET;
\]           yylval = ")"; return C_BRACKET;
\(           yylval = "("; return O_PAREN;
\)           yylval = ")"; return C_PAREN;
\,           yylval = ")"; return COMMA;
[ ]+         /* discard spaces */
.            return yytext[0]; /* alert parser of the garbage */

%%

int cube_yylex();

void cube_flush_scanner_buffer(void) {
  YY_FLUSH_BUFFER;
}
