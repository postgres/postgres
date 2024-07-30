%{
/* contrib/seg/segparse.y */

#include "postgres.h"

#include <float.h>
#include <math.h>

#include "fmgr.h"
#include "nodes/miscnodes.h"
#include "utils/builtins.h"
#include "utils/float.h"

#include "segdata.h"

/* silence -Wmissing-variable-declarations */
extern int seg_yychar;
extern int seg_yynerrs;

/*
 * Bison doesn't allocate anything that needs to live across parser calls,
 * so we can easily have it use palloc instead of malloc.  This prevents
 * memory leaks if we error out during parsing.
 */
#define YYMALLOC palloc
#define YYFREE   pfree

static bool seg_atof(char *value, float *result, struct Node *escontext);

static int sig_digits(const char *value);

%}

/* BISON Declarations */
%parse-param {SEG *result}
%parse-param {struct Node *escontext}
%expect 0
%name-prefix="seg_yy"

%union
{
	struct BND
	{
		float		val;
		char		ext;
		char		sigd;
	} bnd;
	char	   *text;
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
		char		strbuf[25];

		result->lower = $1.val - $3.val;
		result->upper = $1.val + $3.val;
		snprintf(strbuf, sizeof(strbuf), "%g", result->lower);
		result->l_sigd = Max(sig_digits(strbuf), Max($1.sigd, $3.sigd));
		snprintf(strbuf, sizeof(strbuf), "%g", result->upper);
		result->u_sigd = Max(sig_digits(strbuf), Max($1.sigd, $3.sigd));
		result->l_ext = '\0';
		result->u_ext = '\0';
	}

	| boundary RANGE boundary
	{
		result->lower = $1.val;
		result->upper = $3.val;
		if ( result->lower > result->upper ) {
			errsave(escontext,
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
		float		val;

		if (!seg_atof($1, &val, escontext))
			YYABORT;

		$$.ext = '\0';
		$$.sigd = sig_digits($1);
		$$.val = val;
	}
	| EXTENSION SEGFLOAT
	{
		/* temp variable avoids a gcc 3.3.x bug on Sparc64 */
		float		val;

		if (!seg_atof($2, &val, escontext))
			YYABORT;

		$$.ext = $1[0];
		$$.sigd = sig_digits($2);
		$$.val = val;
	}
	;

deviation: SEGFLOAT
	{
		/* temp variable avoids a gcc 3.3.x bug on Sparc64 */
		float		val;

		if (!seg_atof($1, &val, escontext))
			YYABORT;

		$$.ext = '\0';
		$$.sigd = sig_digits($1);
		$$.val = val;
	}
	;

%%


static bool
seg_atof(char *value, float *result, struct Node *escontext)
{
	*result = float4in_internal(value, NULL, "seg", value, escontext);
	if (SOFT_ERROR_OCCURRED(escontext))
		return false;
	return true;
}

static int
sig_digits(const char *value)
{
	int			n = significant_digits(value);

	/* Clamp, to ensure value will fit in sigd fields */
	return Min(n, FLT_DIG);
}
