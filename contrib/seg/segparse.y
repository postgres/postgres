%{
#define YYERROR_VERBOSE   
#define YYPARSE_PARAM result  /* need this to pass a pointer (void *) to yyparse */
  
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "segdata.h"
#include "buffer.h"
  
#include "postgres.h"
#include "utils/elog.h"
  
#undef yylex                  /* falure to redefine yylex will result in calling the */
#define yylex seg_yylex       /* wrong scanner when running inside postgres backend  */

  extern int errno;
  extern int yylex();           /* defined as seg_yylex in segscan.c */
  extern int significant_digits( char *str );    /* defined in seg.c */
  
  int seg_yyerror( char *msg );
  int seg_yyparse( void *result );

  float seg_atof( char *value );

#define MAX(X,Y) ((X) > (Y) ? (X) : (Y))
#define MIN(X,Y) ((X) < (Y) ? (X) : (Y))
#define ABS(X) ((X) < 0 ? (-X) : (X))

  long threshold;
  char strbuf[25] = {
    '0', '0', '0', '0', '0',
    '0', '0', '0', '0', '0',
    '0', '0', '0', '0', '0',
    '0', '0', '0', '0', '0',
    '0', '0', '0', '0', '\0'
  };

%}

/* BISON Declarations */
%union {
  struct BND {
    float val;
    char  ext;
    char  sigd;
  } bnd;
  char * text;
}
%token <text> FLOAT
%token <text> RANGE
%token <text> PLUMIN
%token <text> EXTENSION
%type  <bnd>  boundary
%type  <bnd>  deviation
%start range

/* Grammar follows */
%%


range:
          boundary PLUMIN deviation {
	    ((SEG *)result)->lower = $1.val - $3.val;
	    ((SEG *)result)->upper = $1.val + $3.val;
	    sprintf(strbuf, "%g", ((SEG *)result)->lower);
	    ((SEG *)result)->l_sigd = MAX(MIN(6, significant_digits(strbuf)), MAX($1.sigd, $3.sigd));
	    sprintf(strbuf, "%g", ((SEG *)result)->upper);
	    ((SEG *)result)->u_sigd = MAX(MIN(6, significant_digits(strbuf)), MAX($1.sigd, $3.sigd));
	    ((SEG *)result)->l_ext = '\0';
	    ((SEG *)result)->u_ext = '\0';
          }
      |
          boundary RANGE boundary {
	    ((SEG *)result)->lower = $1.val;
	    ((SEG *)result)->upper = $3.val;
	    if ( ((SEG *)result)->lower > ((SEG *)result)->upper ) {
	      reset_parse_buffer();     
	      elog(ERROR, "swapped boundaries: %g is greater than %g", ((SEG *)result)->lower, ((SEG *)result)->upper );
	      YYERROR;
	    }
	    ((SEG *)result)->l_sigd = $1.sigd;
	    ((SEG *)result)->u_sigd = $3.sigd;
	    ((SEG *)result)->l_ext = ( $1.ext ? $1.ext : '\0' );
	    ((SEG *)result)->u_ext = ( $3.ext ? $3.ext : '\0' );
          }
      |
          boundary RANGE {
	    ((SEG *)result)->lower = $1.val;
	    ((SEG *)result)->upper = HUGE;
	    ((SEG *)result)->l_sigd = $1.sigd;
	    ((SEG *)result)->u_sigd = 0;
	    ((SEG *)result)->l_ext = ( $1.ext ? $1.ext : '\0' );
	    ((SEG *)result)->u_ext = '-';
          }
      ;
      |
          RANGE boundary {
	    ((SEG *)result)->lower = -HUGE;
	    ((SEG *)result)->upper = $2.val;
	    ((SEG *)result)->l_sigd = 0;
	    ((SEG *)result)->u_sigd = $2.sigd;
	    ((SEG *)result)->l_ext = '-';
	    ((SEG *)result)->u_ext = ( $2.ext ? $2.ext : '\0' );
          }
      |
          boundary {
	    ((SEG *)result)->lower = ((SEG *)result)->upper = $1.val;
	    ((SEG *)result)->l_sigd = ((SEG *)result)->u_sigd = $1.sigd;
	    ((SEG *)result)->l_ext = ((SEG *)result)->u_ext = ( $1.ext ? $1.ext : '\0' );
          }
      ;

boundary:
          FLOAT {
             $$.ext = '\0';
	     $$.sigd = significant_digits($1);
             $$.val = seg_atof($1);
	  }
      | 
	  EXTENSION FLOAT {
             $$.ext = $1[0];
	     $$.sigd = significant_digits($2);
             $$.val = seg_atof($2);
	  }
      ;

deviation:
          FLOAT {
             $$.ext = '\0';
	     $$.sigd = significant_digits($1);
             $$.val = seg_atof($1);
	  }
      ;

%%


float seg_atof ( char *value ) {
  float result;
  char *buf = (char *) palloc(256);

  errno = 0;
  sscanf(value, "%f", &result);

  if ( errno ) {
    sprintf(buf, "numeric value %s unrepresentable", value);
    reset_parse_buffer();     
    elog(ERROR, buf);
  }

  return result;
}


int seg_yyerror ( char *msg ) {
  char *buf = (char *) palloc(256);
  int position;

  yyclearin;

  if ( !strcmp(msg, "parse error, expecting `$'") ) {
    msg = "expecting end of input";
  }

  position = parse_buffer_pos() > parse_buffer_size() ? parse_buffer_pos() - 1 : parse_buffer_pos();

  sprintf(
	  buf, 
	  "%s at or near position %d, character ('%c', \\%03o), input: '%s'\n", 
	  msg,
	  position,
	  parse_buffer()[position - 1],
	  parse_buffer()[position - 1],
	  parse_buffer()
	  );

  reset_parse_buffer();     
  elog(ERROR, buf);
  return 0;
}


