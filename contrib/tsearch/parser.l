%{
#include <string.h>
#include "deflex.h"
#include "parser.h"

/* postgres allocation function */
#include "postgres.h"
#define free	pfree
#define malloc	palloc
#define realloc repalloc

#ifdef strdup
#undef strdup
#endif
#define strdup	pstrdup


char *token = NULL;  /* pointer to token */
char *s     = NULL;  /* for returning full defis-word */

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

#define YY_NO_UNPUT
%}

/* parser's state for parsing defis-word */
%x DELIM  
/* parser's state for parsing URL*/
%x URL  
%x SERVER  

/* parser's state for parsing filepath */

%x INTAG
%x QINTAG

/* NONLATIN char */
NONLATINALNUM	[0-9\200-\377]
NONLATINALPHA	[\200-\377]
ALPHA		[a-zA-Z\200-\377]
ALNUM		[0-9a-zA-Z\200-\377]


HOSTNAME	([-_[:alnum:]]+\.)+[[:alpha:]]+
URI		[-_[:alnum:]/%,\.;=&?#]+

%%

"<"[[:alpha:]]	{ BEGIN INTAG;
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return SYMTAG;
 }

"</"[[:alpha:]]	{ BEGIN INTAG;
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return SYMTAG;
 }

"<>" {
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return SYMTAG;
}

"<"[^>[:alpha:]] { 
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return SPACE;
}

<INTAG>"\""	{ BEGIN QINTAG;
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return SYMTAG;
 }

<QINTAG>"\\\""	{
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return SYMTAG;
}

<QINTAG>"\""	{ BEGIN INTAG;
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return SYMTAG;
 }

<QINTAG>.|\n	{
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return SYMTAG;
}

<INTAG>">"	{ BEGIN INITIAL;
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return SYMTAG;
 }

<INTAG>.|\n    {
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return SYMTAG;
}

 
[-_\.[:alnum:]]+@{HOSTNAME}  /* Emails */ { 
	token = tsearch_yytext; 
	tokenlen = tsearch_yyleng;
	return EMAIL; 
}

<DELIM,INITIAL>[0-9] 	/* digit's and point (might be a version) */ { 
	token = tsearch_yytext; 
	tokenlen = tsearch_yyleng;
	return FINT; 
}

<DELIM,INITIAL>[0-9]+[0-9\.]*[0-9] 	/* digit's and point (might be a version) */ { 
	token = tsearch_yytext; 
	tokenlen = tsearch_yyleng;
	return FINT; 
}

[+-]?[0-9\.]+[eE][+-]?[0-9]+  /* float */ 	{ 
	token = tsearch_yytext; 
	tokenlen = tsearch_yyleng;
	return FLOAT; 
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

({NONLATINALNUM}+-)+{NONLATINALPHA}+ /* composite-word */	{
	BEGIN DELIM;
	if (s) { free(s); s=NULL; } 
	s = strdup( tsearch_yytext );
	tokenlen = tsearch_yyleng;
	yyless( 0 );
	token = s;
	return DEFISNONLATINWORD;
}

([[:alnum:]]+-)+[[:alpha:]]+ /* composite-word */	{
	 BEGIN DELIM;
	if (s) { free(s); s=NULL; } 
	tokenlen = tsearch_yyleng;
	s = strdup( tsearch_yytext );
	yyless( 0 );
	token = s;
	return DEFISLATWORD;
}

({ALNUM}+-)+{ALPHA}+ /* composite-word */	{
	BEGIN DELIM;
	if (s) { free(s); s=NULL; } 
	s = strdup( tsearch_yytext );
	tokenlen = tsearch_yyleng;
	yyless( 0 );
	token = s;
	return DEFISWORD;
}

<DELIM>{NONLATINALNUM}+  /* one word in composite-word */	{ 
	token = tsearch_yytext; 
	tokenlen = tsearch_yyleng;
	return NONLATINPARTWORD; 
}

<DELIM>[[:alnum:]]+  /* one word in composite-word */	{ 
	token = tsearch_yytext; 
	tokenlen = tsearch_yyleng;
	return LATPARTWORD; 
}

<DELIM>{ALNUM}+  /* one word in composite-word */	{ 
	token = tsearch_yytext; 
	tokenlen = tsearch_yyleng;
	return PARTWORD; 
}

<DELIM>-  { 
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return SPACE;
}

<DELIM,SERVER,URL>.|\n /* return in basic state */	{
	BEGIN INITIAL;
	tokenlen = tsearch_yyleng;
	yyless( 0 );
}

{NONLATINALNUM}+ /* normal word */	{ 
	token = tsearch_yytext; 
	tokenlen = tsearch_yyleng;
	return NONLATINWORD; 
}

[[:alnum:]]+ /* normal word */	{ 
	token = tsearch_yytext; 
	tokenlen = tsearch_yyleng;
	return LATWORD; 
}

{ALNUM}+ /* normal word */	{ 
	token = tsearch_yytext; 
	tokenlen = tsearch_yyleng;
	return UWORD; 
}

.|\n {
	token = tsearch_yytext;
	tokenlen = tsearch_yyleng;
	return SPACE;
} 

%%

int tsearch_yywrap(void) {
	return 1;
}

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


