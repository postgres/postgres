%{
/* contrib/seg/segparse.y */

#include "postgres.h"

#include <math.h>

#include "fmgr.h"
#include "utils/builtins.h"
#include "segdata.h"

/*
 * Bison doesn't allocate anything that needs to live across parser calls,
 * so we can easily have it use palloc instead of malloc.  This prevents
 * memory leaks if we error out during parsing.  Note this only works with
 * bison >= 2.0.  However, in bison 1.875 the default is to use alloca()
 * if possible, so there's not really much problem anyhow, at least if
 * you're building with gcc.
 */
#define YYMALLOC palloc
#define YYFREE   pfree

extern int seg_yylex(void);

extern int significant_digits(char *str);		/* defined in seg.c */

extern int	seg_yyparse(SEG *result);
extern void seg_yyerror(SEG *result, const char *message);

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
%parse-param {SEG *result}
%expect 0
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


range: boundary PLUMIN deviation
	{
		result->lower = $1.val - $3.val;
		result->upper = $1.val + $3.val;
		sprintf(strbuf, "%g", result->lower);
		result->l_sigd = Max(Min(6, significant_digits(strbuf)), Max($1.sigd, $3.sigd));
		sprintf(strbuf, "%g", result->upper);
		result->u_sigd = Max(Min(6, significant_digits(strbuf)), Max($1.sigd, $3.sigd));
		result->l_ext = '\0';
		result->u_ext = '\0';
	}

	| boundary RANGE boundary
	{
		result->lower = $1.val;
		result->upper = $3.val;
		if ( result->lower > result->upper ) {
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("swapped boundaries: %g is greater than %g",
							result->lower, result->upper)));

			YYERROR;
		}
		result->l_sigd = $1.sigd;
		result->u_sigd = $3.sigd;
		result->l_ext = ( $1.ext ? $1.ext : '\0' );
		result->u_ext = ( $3.ext ? $3.ext : '\0' );
	}

	| boundary RANGE
	{
		result->lower = $1.val;
		result->upper = HUGE_VAL;
		result->l_sigd = $1.sigd;
		result->u_sigd = 0;
		result->l_ext = ( $1.ext ? $1.ext : '\0' );
		result->u_ext = '-';
	}

	| RANGE boundary
	{
		result->lower = -HUGE_VAL;
		result->upper = $2.val;
		result->l_sigd = 0;
		result->u_sigd = $2.sigd;
		result->l_ext = '-';
		result->u_ext = ( $2.ext ? $2.ext : '\0' );
	}

	| boundary
	{
		result->lower = result->upper = $1.val;
		result->l_sigd = result->u_sigd = $1.sigd;
		result->l_ext = result->u_ext = ( $1.ext ? $1.ext : '\0' );
	}
	;

boundary: SEGFLOAT
	{
		/* temp variable avoids a gcc 3.3.x bug on Sparc64 */
		float val = seg_atof($1);

		$$.ext = '\0';
		$$.sigd = significant_digits($1);
		$$.val = val;
	}
	| EXTENSION SEGFLOAT
	{
		/* temp variable avoids a gcc 3.3.x bug on Sparc64 */
		float val = seg_atof($2);

		$$.ext = $1[0];
		$$.sigd = significant_digits($2);
		$$.val = val;
	}
	;

deviation: SEGFLOAT
	{
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
