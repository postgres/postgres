%{
/**********************************************************************
 * scan.l		- Scanner for the PL/pgSQL
 *			  procedural language
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/pl/plpgsql/src/Attic/scan.l,v 1.19 2002/08/04 03:59:09 momjian Exp $
 *
 *    This software is copyrighted by Jan Wieck - Hamburg.
 *
 *    The author hereby grants permission  to  use,  copy,  modify,
 *    distribute,  and  license this software and its documentation
 *    for any purpose, provided that existing copyright notices are
 *    retained  in  all  copies  and  that  this notice is included
 *    verbatim in any distributions. No written agreement, license,
 *    or  royalty  fee  is required for any of the authorized uses.
 *    Modifications to this software may be  copyrighted  by  their
 *    author  and  need  not  follow  the licensing terms described
 *    here, provided that the new terms are  clearly  indicated  on
 *    the first page of each file where they apply.
 *
 *    IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY
 *    PARTY  FOR  DIRECT,   INDIRECT,   SPECIAL,   INCIDENTAL,   OR
 *    CONSEQUENTIAL   DAMAGES  ARISING  OUT  OF  THE  USE  OF  THIS
 *    SOFTWARE, ITS DOCUMENTATION, OR ANY DERIVATIVES THEREOF, EVEN
 *    IF  THE  AUTHOR  HAVE BEEN ADVISED OF THE POSSIBILITY OF SUCH
 *    DAMAGE.
 *
 *    THE  AUTHOR  AND  DISTRIBUTORS  SPECIFICALLY   DISCLAIM   ANY
 *    WARRANTIES,  INCLUDING,  BUT  NOT  LIMITED  TO,  THE  IMPLIED
 *    WARRANTIES  OF  MERCHANTABILITY,  FITNESS  FOR  A  PARTICULAR
 *    PURPOSE,  AND NON-INFRINGEMENT.  THIS SOFTWARE IS PROVIDED ON
 *    AN "AS IS" BASIS, AND THE AUTHOR  AND  DISTRIBUTORS  HAVE  NO
 *    OBLIGATION   TO   PROVIDE   MAINTENANCE,   SUPPORT,  UPDATES,
 *    ENHANCEMENTS, OR MODIFICATIONS.
 *
 **********************************************************************/

#include "plpgsql.h"
#include "pl.tab.h"


static char	*plpgsql_source;
static int	plpgsql_bytes_left;
static int	scanner_functype;
static int	scanner_typereported;
static int	pushback_token;
static bool have_pushback_token;

int	plpgsql_SpaceScanned = 0;

static void plpgsql_input(char *buf, int *result, int max);

#define YY_INPUT(buf,res,max)	plpgsql_input(buf, &res, max)
%}

%option 8bit
%option never-interactive
%option nounput
%option noyywrap

%option yylineno

DQUOTE	\"([^\"])*\"
WS    [\200-\377_A-Za-z]
WC    [\200-\377_A-Za-z0-9]

%x	IN_STRING IN_COMMENT

%%
    /* ----------
     * Local variable in scanner to remember where
     * a string or comment started
     * ----------
     */
    int	start_lineno = 0;

    /* ----------
     * Reset the state when entering the scanner
     * ----------
     */
    BEGIN INITIAL;
    plpgsql_SpaceScanned = 0;

    /* ----------
     * On the first call to a new source report the
     * functions type (T_FUNCTION or T_TRIGGER)
     * ----------
     */
	if (!scanner_typereported)
	{
		scanner_typereported = 1;
		return scanner_functype;
	}

    /* ----------
     * The keyword rules
     * ----------
     */
:=				{ return K_ASSIGN;			}
=				{ return K_ASSIGN;			}
\.\.			{ return K_DOTDOT;			}
alias			{ return K_ALIAS;			}
begin			{ return K_BEGIN;			}
close			{ return K_CLOSE;			}
constant		{ return K_CONSTANT;		}
cursor			{ return K_CURSOR;			}
debug			{ return K_DEBUG;			}
declare			{ return K_DECLARE;			}
default			{ return K_DEFAULT;			}
diagnostics		{ return K_DIAGNOSTICS;		}
else			{ return K_ELSE;			}
elsif           { return K_ELSIF;           }
end				{ return K_END;				}
exception		{ return K_EXCEPTION;		}
execute			{ return K_EXECUTE;			}
exit			{ return K_EXIT;			}
fetch			{ return K_FETCH;			}
for				{ return K_FOR;				}
from			{ return K_FROM;			}
get				{ return K_GET;				}
if				{ return K_IF;				}
in				{ return K_IN;				}
info			{ return K_INFO;			}
into			{ return K_INTO;			}
is				{ return K_IS;				}
log				{ return K_LOG;				}
loop			{ return K_LOOP;			}
not				{ return K_NOT;				}
notice			{ return K_NOTICE;			}
null			{ return K_NULL;			}
open			{ return K_OPEN;			}
perform			{ return K_PERFORM;			}
raise			{ return K_RAISE;			}
record			{ return K_RECORD;			}
rename			{ return K_RENAME;			}
result_oid		{ return K_RESULT_OID;		}
return			{ return K_RETURN;			}
reverse			{ return K_REVERSE;			}
row_count		{ return K_ROW_COUNT;		}
select			{ return K_SELECT;			}
then			{ return K_THEN;			}
to				{ return K_TO;				}
type			{ return K_TYPE;			}
warning			{ return K_WARNING;			}
when			{ return K_WHEN;			}
while			{ return K_WHILE;			}

^#option		{ return O_OPTION;			}
dump			{ return O_DUMP;			}


    /* ----------
     * Special word rules
     * ----------
     */
{WS}{WC}*		{ return plpgsql_parse_word(yytext);	}
{DQUOTE}*		{ return plpgsql_parse_word(yytext);	}

{WS}{WC}*\.{WS}{WC}*	{ return plpgsql_parse_dblword(yytext);	}
{WS}{WC}*\.{DQUOTE}*	{ return plpgsql_parse_dblword(yytext);	}
{DQUOTE}*\.{WS}{WC}*	{ return plpgsql_parse_dblword(yytext);	}
{DQUOTE}*\.{DQUOTE}*	{ return plpgsql_parse_dblword(yytext);	}

{WS}{WC}*\.{WS}{WC}*\.{WS}{WC}*	{ return plpgsql_parse_tripword(yytext); }
{WS}{WC}*\.{WS}{WC}*\.{DQUOTE}*	{ return plpgsql_parse_tripword(yytext); }
{WS}{WC}*\.{DQUOTE}*\.{WS}{WC}*	{ return plpgsql_parse_tripword(yytext); }
{WS}{WC}*\.{DQUOTE}*\.{DQUOTE}*	{ return plpgsql_parse_tripword(yytext); }
{DQUOTE}*\.{WS}{WC}*\.{WS}{WC}*	{ return plpgsql_parse_tripword(yytext); }
{DQUOTE}*\.{WS}{WC}*\.{DQUOTE}*	{ return plpgsql_parse_tripword(yytext); }
{DQUOTE}*\.{DQUOTE}*\.{WS}{WC}*	{ return plpgsql_parse_tripword(yytext); }
{DQUOTE}*\.{DQUOTE}*\.{DQUOTE}*	{ return plpgsql_parse_tripword(yytext); }

{WS}{WC}*%TYPE		{ return plpgsql_parse_wordtype(yytext);	}
{DQUOTE}*%TYPE		{ return plpgsql_parse_wordtype(yytext);	}

{WS}{WC}*\.{WS}{WC}*%TYPE	{ return plpgsql_parse_dblwordtype(yytext); }
{WS}{WC}*\.{DQUOTE}*%TYPE	{ return plpgsql_parse_dblwordtype(yytext); }
{DQUOTE}*\.{WS}{WC}*%TYPE	{ return plpgsql_parse_dblwordtype(yytext); }
{DQUOTE}*\.{DQUOTE}*%TYPE	{ return plpgsql_parse_dblwordtype(yytext); }

{WS}{WC}*%ROWTYPE	{ return plpgsql_parse_wordrowtype(yytext);	}
{DQUOTE}*%ROWTYPE	{ return plpgsql_parse_wordrowtype(yytext);	}

\$[0-9]+		{ return plpgsql_parse_word(yytext);	}

\$[0-9]+\.{WS}{WC}*	{ return plpgsql_parse_dblword(yytext);	}
\$[0-9]+\.{DQUOTE}*	{ return plpgsql_parse_dblword(yytext);	}

\$[0-9]+\.{WS}{WC}*\.{WS}{WC}*	{ return plpgsql_parse_tripword(yytext); }
\$[0-9]+\.{WS}{WC}*\.{DQUOTE}*	{ return plpgsql_parse_tripword(yytext); }
\$[0-9]+\.{DQUOTE}*\.{WS}{WC}*	{ return plpgsql_parse_tripword(yytext); }
\$[0-9]+\.{DQUOTE}*\.{DQUOTE}*	{ return plpgsql_parse_tripword(yytext); }

\$[0-9]+%TYPE		{ return plpgsql_parse_wordtype(yytext);	}

\$[0-9]+\.{WS}{WC}*%TYPE	{ return plpgsql_parse_dblwordtype(yytext); }
\$[0-9]+\.{DQUOTE}*%TYPE	{ return plpgsql_parse_dblwordtype(yytext); }

\$[0-9]+%ROWTYPE	{ return plpgsql_parse_wordrowtype(yytext);	}

[0-9]+			{ return T_NUMBER;			}

    /* ----------
     * Ignore whitespaces but remember this happened
     * ----------
     */
[ \t\r\n]+		{ plpgsql_SpaceScanned = 1;		}

    /* ----------
     * Eat up comments
     * ----------
     */
--[^\r\n]*		;
\/\*			{ start_lineno = yylineno;
			  BEGIN IN_COMMENT;
			}
<IN_COMMENT>\*\/	{ BEGIN INITIAL;			}
<IN_COMMENT>\n		;
<IN_COMMENT>.		;
<IN_COMMENT><<EOF>>	{
				plpgsql_error_lineno = start_lineno;
				elog(ERROR, "unterminated comment");
			}

    /* ----------
     * Collect anything inside of ''s and return one STRING
     * ----------
     */
'			{ start_lineno = yylineno;
			  BEGIN IN_STRING;
			  yymore();
			}
<IN_STRING>\\.		|
<IN_STRING>''		{ yymore();				}
<IN_STRING>'		{ BEGIN INITIAL;
			  return T_STRING;
			}
<IN_STRING><<EOF>>	{
				plpgsql_error_lineno = start_lineno;
				elog(ERROR, "unterminated string");
			}
<IN_STRING>[^'\\]*	{ yymore();				}

    /* ----------
     * Any unmatched character is returned as is
     * ----------
     */
.			{ return yytext[0];			}

%%


static void
plpgsql_input(char *buf, int *result, int max)
{
    int		n = max;

    if (n > plpgsql_bytes_left)
        n = plpgsql_bytes_left;

    if (n == 0)
	{
        *result = YY_NULL;
		return;
    }

    *result = n;
    memcpy(buf, plpgsql_source, n);
    plpgsql_source += n;
    plpgsql_bytes_left -= n;
}

/*
 * This is the yylex routine called from outside.  It exists to provide
 * a token pushback facility.
 */
int
plpgsql_yylex(void)
{
	if (have_pushback_token)
	{
		have_pushback_token = false;
		return pushback_token;
	}
	return yylex();
}

/*
 * Push back a single token to be re-read by next plpgsql_yylex() call.
 */
void
plpgsql_push_back_token(int token)
{
	if (have_pushback_token)
		elog(ERROR, "plpgsql_push_back_token: can't push back multiple tokens");
	pushback_token = token;
	have_pushback_token = true;
}


/*
 * Initialize the scanner for new input.
 */
void
plpgsql_setinput(char *source, int functype)
{
    yyrestart(NULL);
    yylineno = 1;

    plpgsql_source = source;

	/*----------
	 * Hack: skip any initial newline, so that in the common coding layout
	 *		CREATE FUNCTION ... AS '
	 *			code body
	 *		' LANGUAGE 'plpgsql';
	 * we will think "line 1" is what the programmer thinks of as line 1.
	 *----------
	 */
    if (*plpgsql_source == '\r')
        plpgsql_source++;
    if (*plpgsql_source == '\n')
        plpgsql_source++;

    plpgsql_bytes_left = strlen(plpgsql_source);

    scanner_functype     = functype;
    scanner_typereported = 0;

	have_pushback_token = false;
}
