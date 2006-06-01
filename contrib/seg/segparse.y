%{
#define YYPARSE_PARAM result  /* need this to pass a pointer (void *) to yyparse */
  
#include "postgres.h"

#include <math.h>

#include "fmgr.h"
#include "utils/builtins.h"
#include "segdata.h"

  extern int seg_yylex(void);

  extern int significant_digits(char *str);		/* defined in seg.c */
  
  void seg_yyerror(const char *message);
  int seg_yyparse(void *result);

  static float seg_atof(char *value);

  static char strbuf[25] = {
    '0', '0', '0', '0', '0',
    '0', '0', '0', '0', '0',
    '0', '0', '0', '0', '0',
    '0', '0', '0', '0', '0',
    '0', '0', '0', '0', '\0'
  };

%}

/* BISON Declarations */
%name-prefix="seg_yy"

%union {
  struct BND {
    float val;
    char  ext;
    char  sigd;
  } bnd;
  char * text;
}
%token <text> SEGFLOAT
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
	    ((SEG *)result)->upper = HUGE_VAL;
	    ((SEG *)result)->l_sigd = $1.sigd;
	    ((SEG *)result)->u_sigd = 0;
	    ((SEG *)result)->l_ext = ( $1.ext ? $1.ext : '\0' );
	    ((SEG *)result)->u_ext = '-';
          }
      |
          RANGE boundary {
	    ((SEG *)result)->lower = -HUGE_VAL;
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
          SEGFLOAT {
			/* temp variable avoids a gcc 3.3.x bug on Sparc64 */
			float val = seg_atof($1);

			$$.ext = '\0';
			$$.sigd = significant_digits($1);
			$$.val = val;
	  }
      | 
	  EXTENSION SEGFLOAT {
			/* temp variable avoids a gcc 3.3.x bug on Sparc64 */
			float val = seg_atof($2);

			$$.ext = $1[0];
			$$.sigd = significant_digits($2);
			$$.val = val;
	  }
      ;

deviation:
          SEGFLOAT {
			/* temp variable avoids a gcc 3.3.x bug on Sparc64 */
			float val = seg_atof($1);

			$$.ext = '\0';
			$$.sigd = significant_digits($1);
			$$.val = val;
	  }
      ;

%%


static float
seg_atof(char *value)
{
	Datum datum;

	datum = DirectFunctionCall1(float4in, CStringGetDatum(value));
	return DatumGetFloat4(datum);
}


#include "segscan.c"
