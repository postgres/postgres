/* Copyright comment! */
%{
#include "type.h"
#include "y.tab.h"

#include "extern.h"

#define dbg(arg)       if (debugging) fprintf(stderr, "DEBUG, %d: %s\n", yylineno, #arg);
%}
%option yylineno
%s C SQL
ccomment	\/\*([^*]|\*[^/]|\*\*[^/])*\*\/
ws	([ \t\n][ \t\n]*|{ccomment})*
letter	[A-Za-z_]
digit	[0-9]
length	{digit}+
symbol	{letter}({letter}|{digit})*
string	'[^']*'

exec	[eE][xX][eE][cC]
sql	[sS][qQ][lL]
varchar	[vV][aA][rR][cC][hH][aA][rR]
varchar2	[vV][aA][rR][cC][hH][aA][rR]2
into	[iI][nN][tT][oO]
begin	[bB][eE][gG][iI][nN]
end	[eE][nN][dD]
declare [dD][eE][cC][lL][aA][rR][eE]
section	[sS][eE][cC][tT][iI][oO][nN]
include [iI][nN][cC][lL][uU][dD][eE]
connect [cC][oO][nN][nN][eE][cC][tT]
open	[oO][pP][eE][nN]
commit  [cC][oO][mM][mM][iI][tT]
rollback [rR][oO][lL][lL][bB][aA][cC][kK]
%%
<C>{exec}{ws}{sql}	{ BEGIN SQL; dbg(SQL_START); return SQL_START; }
<SQL>";"		{ BEGIN C; dbg(SQL_SEMI); return SQL_SEMI; }
<SQL>{begin}		{ dbg(SQL_BEGIN); return SQL_BEGIN; }
<SQL>{end}		{ dbg(SQL_END); return SQL_END; }
<SQL>{declare}		{ dbg(SQL_DECLARE); return SQL_DECLARE; }
<SQL>{section}		{ dbg(SQL_SECTION); return SQL_SECTION; }
<SQL>{include}		{ dbg(SQL_INCLUDE); return SQL_INCLUDE; }
<SQL>{connect}		{ dbg(SQL_CONNECT); return SQL_CONNECT; }
<SQL>{open}		{ dbg(SQL_OPEN); return SQL_OPEN; }
<SQL>{commit}		{ dbg(SQL_COMMIT); return SQL_COMMIT; }
<SQL>{rollback}		{ dbg(SQL_ROLLBACK); return SQL_ROLLBACK; }

<SQL>{into}		{ dbg(SQL_INTO); return SQL_INTO; }
			  
{length}		{ dbg(S_LENGTH); return S_LENGTH; }
			  
{varchar}		{ dbg(S_VARCHAR); return S_VARCHAR; }
{varchar2}		{ dbg(S_VARCHAR2); return S_VARCHAR2; }
long			{ dbg(S_LONG); return S_LONG; }
short			{ dbg(S_SHORT); return S_SHORT; }
int			{ dbg(S_INT); return S_INT; }
char			{ dbg(S_CHAR); return S_CHAR; }
float			{ dbg(S_FLOAT); return S_FLOAT; }
double			{ dbg(S_DOUBLE); return S_DOUBLE; }
bool                    { dbg(S_BOOL); return S_BOOL; }

static			{ dbg(S_STATIC); return S_STATIC; }
extern			{ dbg(S_EXTERN); return S_EXTERN; }
auto			{ dbg(S_AUTO); return S_AUTO; }
const			{ dbg(S_CONST); return S_CONST; }
register		{ dbg(S_REGISTER); return S_REGISTER; }

struct			{ dbg(S_STRUCT); return S_STRUCT; }

{string}		{ dbg(SQL_STRING); return SQL_STRING; }
<SQL>{ws}		; 
{symbol}		{ dbg(S_SYMBOL); return S_SYMBOL; }

<SQL>"!<"		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"!>"		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"!^"		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"!|"		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"!~"		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"!~*"		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"#<"		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"#<="		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"#<>"		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"#="		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"#>"		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"#>="		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"&&"		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"&<"		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"&>"		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"<<"		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"<="		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"<===>"		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"<>"		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"<?>"		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"===>"		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"===`"		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"=|="		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>">="		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>">>"		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"@@"		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"|/"		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"||/"		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"~*"		{ dbg(S_SYMBOL); return S_SYMBOL; }
<SQL>"~="		{ dbg(S_SYMBOL); return S_SYMBOL; }

"["			{ dbg([); return '['; }
"]"			{ dbg(]); return ']'; }
";"			{ dbg(;); return ';'; }
","			{ dbg(komma); return ','; }
\{			{ dbg(blockstart); return '{'; }
\}			{ dbg(blockend); return'}'; }

<SQL>":"		{ dbg(:); return ':'; }

{ws}			{ ECHO; }
.			{ dbg(.); return S_ANYTHING; }
%%
void
lex_init(void)
{
    braces_open = 0;
    BEGIN C;
}

int yywrap(void) 
{ 
    return 1;
}

