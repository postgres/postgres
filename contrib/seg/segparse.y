%{
#define YYPARSE_PARAM result  /* need this to pass a pointer (void *) to yyparse */
  
#include "postgres.h"

#include <errno.h>
#include <math.h>

#include "segdata.h"
  
#ifdef __CYGWIN__
#define HUGE HUGE_VAL
#endif /* __CYGWIN__ */

#undef yylex                  /* falure to redefine yylex will result in calling the */
#define yylex seg_yylex       /* wrong scanner when running inside postgres backend  */

  extern int yylex();           /* defined as seg_yylex in segscan.c */
  extern int significant_digits( char *str );    /* defined in seg.c */
  
  void seg_yyerror(const char *message);
  int seg_yyparse( void *result );

  float seg_atof( char *value );

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
	    ((SEG *)result)->l_sigd = Max(Min(6, significant_digits(strbuf)), Max($1.sigd, $3.sigd));
	    sprintf(strbuf, "%g", ((SEG *)result)->upper);
	    ((SEG *)result)->u_sigd = Max(Min(6, significant_digits(strbuf)), Max($1.sigd, $3.sigd));
	    ((SEG *)result)->l_ext = '\0';
	    ((SEG *)result)->u_ext = '\0';
          }
      |
          boundary RANGE boundary {
	    ((SEG *)result)->lower = $1.val;
	    ((SEG *)result)->upper = $3.val;
	    if ( ((SEG *)result)->lower > ((SEG *)result)->upper ) {
	      ereport(ERROR,
				  (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				   errmsg("swapped boundaries: %g is greater than %g",
						  ((SEG *)result)->lower, ((SEG *)result)->upper)));

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
    snprintf(buf, 256, "numeric value %s unrepresentable", value);
    ereport(ERROR,
		    (errcode(ERRCODE_SYNTAX_ERROR),
		     errmsg("syntax error"),
		     errdetail("%s", buf)));
  }

  return result;
}


#include "segscan.c"
