%{
/* 
** A scanner for EMP-style numeric ranges 
*/

#include <string.h>
#include <stdio.h>
#include "segparse.h"
#include "buffer.h"

#define YY_NO_UNPUT 1
#undef  yywrap

/* flex screws a couple symbols when used with the -P otion; fix those */
#define YY_DECL int seg_yylex YY_PROTO(( void )); \
int seg_yylex YY_PROTO(( void ))
#define yylval seg_yylval

/* redefined YY_INPUT reads byte-wise from the memory area defined in buffer.c */
#undef YY_INPUT
#define YY_INPUT(buf,result,max_size) \
{ \
        int c = read_parse_buffer(); \
        result = (c == '\0') ?  YY_NULL : (buf[0] = c, 1); \
}

void seg_flush_scanner_buffer(void); 
%}

range        (\.\.)(\.)?
plumin       (\'\+\-\')|(\(\+\-)\)
integer      [+-]?[0-9]+
real         [+-]?[0-9]+\.[0-9]+
float        ({integer}|{real})([eE]{integer})?

%%

{range}      yylval.text = yytext; return RANGE;
{plumin}     yylval.text = yytext; return PLUMIN;
{float}      yylval.text = yytext; return FLOAT;
\<           yylval.text = "<"; return EXTENSION;
\>           yylval.text = ">"; return EXTENSION;
\~           yylval.text = "~"; return EXTENSION;
[ ]+         /* discard spaces */
.            return yytext[0]; /* alert parser of the garbage */

%%

int seg_yylex();

void seg_flush_scanner_buffer(void) {
  YY_FLUSH_BUFFER;
}
