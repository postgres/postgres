/* Copyright comment! */
%{
#include <sys/types.h>
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
label	({letter}|{digit})*
string	'[^']*'

begin	[bB][eE][gG][iI][nN]
commit  [cC][oO][mM][mM][iI][tT]
connect [cC][oO][nN][nN][eE][cC][tT]
continue [cC][oO][nN][tT][iI][nN][uU][eE]
declare [dD][eE][cC][lL][aA][rR][eE]
do      [dD][oO]
end	[eE][nN][dD]
exec	[eE][xX][eE][cC]
execute	[eE][xX][eE][cC][uU][tT][eE]
fetch   [fF][eE][tT][cC][hH]
found	[fF][oO][uU][nN][dD]
from    [fF][rR][oO][mM]
go	[gG][oO]
goto    [gG][oO][tT][oO]
immediate [iI][mM][mM][eE][dD][iI][aA][tT][eE]
include [iI][nN][cC][lL][uU][dD][eE]
in	[iI][nN]
into	[iI][nN][tT][oO]
not	[nN][oO][tT]
open	[oO][pP][eE][nN]
release [rR][eE][lL][eE][aA][sS][eE]
rollback [rR][oO][lL][lL][bB][aA][cC][kK]
section	[sS][eE][cC][tT][iI][oO][nN]
sql	[sS][qQ][lL]
sqlerror [sS][qQ][lL][eE][rR][rR][oO][rR]
sqlprint [sS][qQ][lL][pP][rR][iI][nN][tT]
stop	[sS][tT][oO][pP]
to    	[tT][oO]
varchar	[vV][aA][rR][cC][hH][aA][rR]
varchar2	[vV][aA][rR][cC][hH][aA][rR]2
whenever [wW][hH][eE][nN][eE][vV][eE][rR]
work    [wW][oO][rR][kK]
%%
<C>{exec}{ws}{sql}	{ BEGIN SQL; dbg(SQL_START); return SQL_START; }
<SQL>";"		{ BEGIN C; dbg(SQL_SEMI); return SQL_SEMI; }
<SQL>{begin}		{ dbg(SQL_BEGIN); return SQL_BEGIN; }
<SQL>{end}		{ dbg(SQL_END); return SQL_END; }
<SQL>{declare}		{ dbg(SQL_DECLARE); return SQL_DECLARE; }
<SQL>{execute}		{ dbg(SQL_EXECUTE); return SQL_EXECUTE; }
<SQL>{immediate}	{ dbg(SQL_IMMEDIATE); return SQL_IMMEDIATE; }
<SQL>{section}		{ dbg(SQL_SECTION); return SQL_SECTION; }
<SQL>{include}		{ dbg(SQL_INCLUDE); return SQL_INCLUDE; }
<SQL>{connect}		{ dbg(SQL_CONNECT); return SQL_CONNECT; }
<SQL>{open}		{ dbg(SQL_OPEN); return SQL_OPEN; }
<SQL>{commit}		{ dbg(SQL_COMMIT); return SQL_COMMIT; }
<SQL>{release}		{ dbg(SQL_RELEASE); return SQL_RELEASE; }
<SQL>{work}		{ dbg(SQL_WORK); return SQL_WORK; }
<SQL>{fetch}		{ dbg(SQL_FETCH); return SQL_FETCH; }
<SQL>{rollback}		{ dbg(SQL_ROLLBACK); return SQL_ROLLBACK; }
<SQL>{whenever}		{ dbg(SQL_WHENEVER); return SQL_WHENEVER; }
<SQL>{sqlerror}		{ dbg(SQL_SQLERROR); return SQL_SQLERROR; }
<SQL>{sqlprint}		{ dbg(SQL_SQLPRINT); return SQL_SQLPRINT; }
<SQL>{not}{ws}{found}	{ dbg(SQL_NOT_FOUND); return SQL_NOT_FOUND; }
<SQL>{continue}		{ dbg(SQL_CONTINUE); return SQL_CONTINUE; }
<SQL>{into}		{ dbg(SQL_INTO); return SQL_INTO; }
<SQL>{in}		{ dbg(SQL_IN); return SQL_IN; }
<SQL>{goto}		{ dbg(SQL_GOTO); return SQL_GOTO; }
<SQL>{go}{ws}{to}	{ dbg(SQL_GOTO); return SQL_GOTO; }
<SQL>{stop}		{ dbg(SQL_STOP); return SQL_STOP; }
<SQL>{do}		{ dbg(SQL_DO); return SQL_DO; }
<SQL>{from}		{ dbg(SQL_FROM); return SQL_FROM; }
			  
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
signed			{ dbg(S_SIGNED); return S_SIGNED; }
extern			{ dbg(S_EXTERN); return S_EXTERN; }
auto			{ dbg(S_AUTO); return S_AUTO; }
const			{ dbg(S_CONST); return S_CONST; }
register		{ dbg(S_REGISTER); return S_REGISTER; }

struct			{ dbg(S_STRUCT); return S_STRUCT; }

{string}		{ dbg(SQL_STRING); return SQL_STRING; }
<SQL>{ws}		; 
{symbol}		{ dbg(S_SYMBOL); return S_SYMBOL; }
{label}			{ dbg(S_LABEL); return S_LABEL; }

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
"="			{ dbg(=); return '='; }
","			{ dbg(komma); return ','; }
\(			{ dbg(braceopen); return '('; }
\)			{ dbg(braceclose); return ')'; }
\{			{ dbg(blockstart); return '{'; }
\}			{ dbg(blockend); return '}'; }
\*			{ dbg(*); return('*'); }

<SQL>":"		{ dbg(:); return ':'; }
<SQL>"::"		{ dbg(SQL_CONV); return SQL_CONV; }

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

