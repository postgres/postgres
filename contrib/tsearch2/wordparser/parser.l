%{
#include "postgres.h"

#include "deflex.h"
#include "parser.h"
#include "common.h"

/* Avoid exit() on fatal scanner errors */
#define fprintf(file, fmt, msg)  ts_error(ERROR, fmt, msg)

char *token = NULL;  /* pointer to token */
int tokenlen;
char *s     = NULL;  /* to return WHOLE hyphenated-word */

YY_BUFFER_STATE buf = NULL; /* buffer to parse; it need for parse from string */

%}

%option 8bit
%option never-interactive
%option nounput
%option noyywrap

/* parser's state for parsing hyphenated-word */
%x DELIM  
/* parser's state for parsing URL*/
%x URL  
%x SERVER  

/* parser's state for parsing TAGS */
%x INTAG
%x QINTAG
%x INCOMMENT
%x INSCRIPT

/* cyrillic koi8 char */
CYRALNUM	[0-9\200-\377]
CYRALPHA	[\200-\377]
ALPHA		[a-zA-Z\200-\377]
ALNUM		[0-9a-zA-Z\200-\377]


HOSTNAME	([-_[:alnum:]]+\.)+[[:alpha:]]+
URI		[-_[:alnum:]/%,\.;=&?#]+

%%

"<"[Ss][Cc][Rr][Ii][Pp][Tt] { BEGIN INSCRIPT; }

<INSCRIPT>"</"[Ss][Cc][Rr][Ii][Pp][Tt]">" {
	BEGIN INITIAL; 
	*tsearch2_yytext=' '; *(tsearch2_yytext+1) = '\0'; 
	token = tsearch2_yytext;
	tokenlen = tsearch2_yyleng;
	return SPACE;
}

"<!--"	{ BEGIN INCOMMENT; }

<INCOMMENT>"-->"	{ 
	BEGIN INITIAL;
	*tsearch2_yytext=' '; *(tsearch2_yytext+1) = '\0'; 
	token = tsearch2_yytext;
	tokenlen = tsearch2_yyleng;
	return SPACE;
}


"<"[\![:alpha:]]	{ BEGIN INTAG; }

"</"[[:alpha:]]	{ BEGIN INTAG; }

<INTAG>"\""	{ BEGIN QINTAG; }

<QINTAG>"\\\""	;

<QINTAG>"\""	{ BEGIN INTAG; }

<INTAG>">"	{ 
	BEGIN INITIAL;
	token = tsearch2_yytext;
	*tsearch2_yytext=' '; 
	token = tsearch2_yytext;
	tokenlen = 1;
	return TAG;
}

<QINTAG,INTAG,INCOMMENT,INSCRIPT>.|\n 	;

\&(quot|amp|nbsp|lt|gt)\;   {
	token = tsearch2_yytext;
	tokenlen = tsearch2_yyleng;
	return HTMLENTITY;
}

\&\#[0-9][0-9]?[0-9]?\; {
	token = tsearch2_yytext;
	tokenlen = tsearch2_yyleng;
	return HTMLENTITY;
}
 
[-_\.[:alnum:]]+@{HOSTNAME}  /* Emails */ { 
	token = tsearch2_yytext; 
	tokenlen = tsearch2_yyleng;
	return EMAIL; 
}

[+-]?[0-9]+(\.[0-9]+)?[eEdD][+-]?[0-9]+  /* float */ 	{ 
	token = tsearch2_yytext; 
	tokenlen = tsearch2_yyleng;
	return SCIENTIFIC; 
}

[0-9]+\.[0-9]+\.[0-9\.]*[0-9] {
	token = tsearch2_yytext;
	tokenlen = tsearch2_yyleng;
	return VERSIONNUMBER;
}

[+-]?[0-9]+\.[0-9]+ {
	token = tsearch2_yytext;
	tokenlen = tsearch2_yyleng;
	return DECIMAL;
}

[+-][0-9]+ { 
	token = tsearch2_yytext; 
	tokenlen = tsearch2_yyleng;
	return SIGNEDINT; 
}

<DELIM,INITIAL>[0-9]+ { 
	token = tsearch2_yytext; 
	tokenlen = tsearch2_yyleng;
	return UNSIGNEDINT; 
}

http"://"        { 
	BEGIN URL; 
	token = tsearch2_yytext;
	tokenlen = tsearch2_yyleng;
	return HTTP;
}

ftp"://"        { 
	BEGIN URL; 
	token = tsearch2_yytext;
	tokenlen = tsearch2_yyleng;
	return HTTP;
}

<URL,INITIAL>{HOSTNAME}[/:]{URI} { 
	BEGIN SERVER;
	if (s) { free(s); s=NULL; } 
	s = strdup( tsearch2_yytext ); 
	tokenlen = tsearch2_yyleng;
	yyless( 0 ); 
	token = s;
	return FURL;
}

<SERVER,URL,INITIAL>{HOSTNAME} {
	token = tsearch2_yytext; 
	tokenlen = tsearch2_yyleng;
	return HOST;
}

<SERVER>[/:]{URI} 	{
	token = tsearch2_yytext;
	tokenlen = tsearch2_yyleng;
	return URI;
}

[[:alnum:]\./_-]+"/"[[:alnum:]\./_-]+ {
	token = tsearch2_yytext;
	tokenlen = tsearch2_yyleng;
	return FILEPATH;
}

({CYRALPHA}+-)+{CYRALPHA}+ /* composite-word */	{
	BEGIN DELIM;
	if (s) { free(s); s=NULL; } 
	s = strdup( tsearch2_yytext );
	tokenlen = tsearch2_yyleng;
	yyless( 0 );
	token = s;
	return CYRHYPHENWORD;
}

([[:alpha:]]+-)+[[:alpha:]]+ /* composite-word */	{
	 BEGIN DELIM;
	if (s) { free(s); s=NULL; } 
	s = strdup( tsearch2_yytext );
	tokenlen = tsearch2_yyleng;
	yyless( 0 );
	token = s;
	return LATHYPHENWORD;
}

({ALNUM}+-)+{ALNUM}+ /* composite-word */	{
	BEGIN DELIM;
	if (s) { free(s); s=NULL; } 
	s = strdup( tsearch2_yytext );
	tokenlen = tsearch2_yyleng;
	yyless( 0 );
	token = s;
	return HYPHENWORD;
}

<DELIM>[0-9]+\.[0-9]+\.[0-9\.]*[0-9] {
	token = tsearch2_yytext;
	tokenlen = tsearch2_yyleng;
	return VERSIONNUMBER;
}

<DELIM>\+?[0-9]+\.[0-9]+ {
	token = tsearch2_yytext;
	tokenlen = tsearch2_yyleng;
	return DECIMAL;
}

<DELIM>{CYRALPHA}+  /* one word in composite-word */	{ 
	token = tsearch2_yytext; 
	tokenlen = tsearch2_yyleng;
	return CYRPARTHYPHENWORD; 
}

<DELIM>[[:alpha:]]+  /* one word in composite-word */	{ 
	token = tsearch2_yytext; 
	tokenlen = tsearch2_yyleng;
	return LATPARTHYPHENWORD; 
}

<DELIM>{ALNUM}+  /* one word in composite-word */	{ 
	token = tsearch2_yytext; 
	tokenlen = tsearch2_yyleng;
	return PARTHYPHENWORD; 
}

<DELIM>-  { 
	token = tsearch2_yytext;
	tokenlen = tsearch2_yyleng;
	return SPACE;
}

<DELIM,SERVER,URL>.|\n /* return in basic state */	{
	BEGIN INITIAL;
	yyless( 0 );
}

{CYRALPHA}+ /* normal word */	{ 
	token = tsearch2_yytext; 
	tokenlen = tsearch2_yyleng;
	return CYRWORD; 
}

[[:alpha:]]+ /* normal word */	{ 
	token = tsearch2_yytext; 
	tokenlen = tsearch2_yyleng;
	return LATWORD; 
}

{ALNUM}+ /* normal word */	{ 
	token = tsearch2_yytext; 
	tokenlen = tsearch2_yyleng;
	return UWORD; 
}

[ \r\n\t]+ {
	token = tsearch2_yytext;
	tokenlen = tsearch2_yyleng;
	return SPACE;
}

. {
	token = tsearch2_yytext;
	tokenlen = tsearch2_yyleng;
	return SPACE;
} 

%%

/* clearing after parsing from string */
void tsearch2_end_parse() {
	if (s) { free(s); s=NULL; } 
	tsearch2_yy_delete_buffer( buf );
	buf = NULL;
} 

/* start parse from string */
void tsearch2_start_parse_str(char* str, int limit) {
	if (buf) tsearch2_end_parse();
	buf = tsearch2_yy_scan_bytes( str, limit );
	tsearch2_yy_switch_to_buffer( buf );
	BEGIN INITIAL;
}
