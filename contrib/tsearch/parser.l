%{
#include "postgres.h"

#include "deflex.h"
#include "parser.h"

/* Avoid exit() on fatal scanner errors (a bit ugly -- see yy_fatal_error) */
#define fprintf(file, fmt, msg)  ereport(ERROR, (errmsg_internal("%s", msg)))

/* postgres allocation function */
#define free    pfree
#define malloc  palloc
#define realloc repalloc

#ifdef strdup
#undef strdup
#endif
#define strdup  pstrdup

char *token = NULL;  /* pointer to token */
char *s     = NULL;  /* to return WHOLE hyphenated-word */

YY_BUFFER_STATE buf = NULL; /* buffer to parse; it need for parse from string */

int lrlimit = -1;	/* for limiting read from filehandle ( -1 - unlimited read ) */
int bytestoread = 0;	/* for limiting read from filehandle */

/* redefine macro for read limited length */
#define YY_INPUT(buf,result,max_size) \
	if ( yy_current_buffer->yy_is_interactive ) { \
                int c = '*', n; \
                for ( n = 0; n < max_size && \
                             (c = getc( tsearch_yyin )) != EOF && c != '\n'; ++n ) \
                        buf[n] = (char) c; \
                if ( c == '\n' ) \
                        buf[n++] = (char) c; \
                if ( c == EOF && ferror( tsearch_yyin ) ) \
                        YY_FATAL_ERROR( "input in flex scanner failed" ); \
                result = n; \
        }  else { \
		if ( lrlimit == 0 ) \
			result=YY_NULL; \
		else { \
			if ( lrlimit>0 ) { \
				bytestoread = ( lrlimit > max_size ) ? max_size : lrlimit; \
				lrlimit -= bytestoread; \
			} else \
				bytestoread = max_size; \
        		if ( ((result = fread( buf, 1, bytestoread, tsearch_yyin )) == 0) \
                  		&& ferror( tsearch_yyin ) ) \
                		YY_FATAL_ERROR( "input in flex scanner failed" ); \
		} \
	}

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
	*tsearch_yytext=' '; *(tsearch_yytext+1) = '\0'; 
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return SPACE;
}

"<!--"	{ BEGIN INCOMMENT; }

<INCOMMENT>"-->"	{ 
	BEGIN INITIAL;
	*tsearch_yytext=' '; *(tsearch_yytext+1) = '\0'; 
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return SPACE;
}


"<"[\![:alpha:]]	{ BEGIN INTAG; }

"</"[[:alpha:]]	{ BEGIN INTAG; }

<INTAG>"\""	{ BEGIN QINTAG; }

<QINTAG>"\\\""	;

<QINTAG>"\""	{ BEGIN INTAG; }

<INTAG>">"	{ 
	BEGIN INITIAL;
	token = tsearch_yytext;
	*tsearch_yytext=' '; 
	token = tsearch_yytext;
	tokenlen = 1;
	return TAG;
}

<QINTAG,INTAG,INCOMMENT,INSCRIPT>.|\n 	;

\&(quot|amp|nbsp|lt|gt)\;   {
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return HTMLENTITY;
}

\&\#[0-9][0-9]?[0-9]?\; {
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return HTMLENTITY;
}
 
[-_\.[:alnum:]]+@{HOSTNAME}  /* Emails */ { 
	token = tsearch_yytext; 
	tokenlen = tsearch_yyleng;
	return EMAIL; 
}

[+-]?[0-9]+(\.[0-9]+)?[eEdD][+-]?[0-9]+  /* float */ 	{ 
	token = tsearch_yytext; 
	tokenlen = tsearch_yyleng;
	return SCIENTIFIC; 
}

[0-9]+\.[0-9]+\.[0-9\.]*[0-9] {
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return VERSIONNUMBER;
}

[+-]?[0-9]+\.[0-9]+ {
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return DECIMAL;
}

[+-][0-9]+ { 
	token = tsearch_yytext; 
	tokenlen = tsearch_yyleng;
	return SIGNEDINT; 
}

<DELIM,INITIAL>[0-9]+ { 
	token = tsearch_yytext; 
	tokenlen = tsearch_yyleng;
	return UNSIGNEDINT; 
}

http"://"        { 
	BEGIN URL; 
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return HTTP;
}

ftp"://"        { 
	BEGIN URL; 
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return HTTP;
}

<URL,INITIAL>{HOSTNAME}[/:]{URI} { 
	BEGIN SERVER;
	if (s) { free(s); s=NULL; } 
	s = strdup( tsearch_yytext ); 
	tokenlen = tsearch_yyleng;
	yyless( 0 ); 
	token = s;
	return FURL;
}

<SERVER,URL,INITIAL>{HOSTNAME} {
	token = tsearch_yytext; 
	tokenlen = tsearch_yyleng;
	return HOST;
}

<SERVER>[/:]{URI} 	{
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return URI;
}

[[:alnum:]\./_-]+"/"[[:alnum:]\./_-]+ {
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return FILEPATH;
}

({CYRALPHA}+-)+{CYRALPHA}+ /* composite-word */	{
	BEGIN DELIM;
	if (s) { free(s); s=NULL; } 
	s = strdup( tsearch_yytext );
	tokenlen = tsearch_yyleng;
	yyless( 0 );
	token = s;
	return CYRHYPHENWORD;
}

([[:alpha:]]+-)+[[:alpha:]]+ /* composite-word */	{
	 BEGIN DELIM;
	if (s) { free(s); s=NULL; } 
	s = strdup( tsearch_yytext );
	tokenlen = tsearch_yyleng;
	yyless( 0 );
	token = s;
	return LATHYPHENWORD;
}

({ALNUM}+-)+{ALNUM}+ /* composite-word */	{
	BEGIN DELIM;
	if (s) { free(s); s=NULL; } 
	s = strdup( tsearch_yytext );
	tokenlen = tsearch_yyleng;
	yyless( 0 );
	token = s;
	return HYPHENWORD;
}

<DELIM>\+?[0-9]+\.[0-9]+ {
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return DECIMAL;
}

<DELIM>{CYRALPHA}+  /* one word in composite-word */	{ 
	token = tsearch_yytext; 
	tokenlen = tsearch_yyleng;
	return CYRPARTHYPHENWORD; 
}

<DELIM>[[:alpha:]]+  /* one word in composite-word */	{ 
	token = tsearch_yytext; 
	tokenlen = tsearch_yyleng;
	return LATPARTHYPHENWORD; 
}

<DELIM>{ALNUM}+  /* one word in composite-word */	{ 
	token = tsearch_yytext; 
	tokenlen = tsearch_yyleng;
	return PARTHYPHENWORD; 
}

<DELIM>-  { 
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return SPACE;
}

<DELIM,SERVER,URL>.|\n /* return in basic state */	{
	BEGIN INITIAL;
	yyless( 0 );
}

{CYRALPHA}+ /* normal word */	{ 
	token = tsearch_yytext; 
	tokenlen = tsearch_yyleng;
	return CYRWORD; 
}

[[:alpha:]]+ /* normal word */	{ 
	token = tsearch_yytext; 
	tokenlen = tsearch_yyleng;
	return LATWORD; 
}

{ALNUM}+ /* normal word */	{ 
	token = tsearch_yytext; 
	tokenlen = tsearch_yyleng;
	return UWORD; 
}

[ \r\n\t]+ {
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return SPACE;
}

. {
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return SPACE;
} 

%%

/* clearing after parsing from string */
void end_parse() {
	if (s) { free(s); s=NULL; } 
	tsearch_yy_delete_buffer( buf );
	buf = NULL;
} 

/* start parse from string */
void start_parse_str(char* str, int limit) {
	if (buf) end_parse();
	buf = tsearch_yy_scan_bytes( str, limit );
	tsearch_yy_switch_to_buffer( buf );
	BEGIN INITIAL;
}

/* start parse from filehandle */
void start_parse_fh( FILE* fh, int limit ) {
	if (buf) end_parse();
	lrlimit = ( limit ) ? limit : -1;
	buf = tsearch_yy_create_buffer( fh, YY_BUF_SIZE );
	tsearch_yy_switch_to_buffer( buf );
	BEGIN INITIAL;
}


