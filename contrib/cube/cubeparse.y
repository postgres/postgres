%{
/* contrib/cube/cubeparse.y */

/* NdBox = [(lowerleft),(upperright)] */
/* [(xLL(1)...xLL(N)),(xUR(1)...xUR(n))] */

#include "postgres.h"

#include "cubedata.h"
#include "cubeparse.h"	/* must be after cubedata.h for YYSTYPE and NDBOX */
#include "nodes/miscnodes.h"
#include "utils/float.h"
#include "varatt.h"

/*
 * Bison doesn't allocate anything that needs to live across parser calls,
 * so we can easily have it use palloc instead of malloc.  This prevents
 * memory leaks if we error out during parsing.
 */
#define YYMALLOC palloc
#define YYFREE   pfree

static int item_count(const char *s, char delim);
static bool write_box(int dim, char *str1, char *str2,
					  NDBOX **result, struct Node *escontext);
static bool write_point_as_box(int dim, char *str,
							   NDBOX **result, struct Node *escontext);

%}

/* BISON Declarations */
%parse-param {NDBOX **result}
%parse-param {Size scanbuflen}
%parse-param {struct Node *escontext}
%parse-param {yyscan_t yyscanner}
%lex-param   {yyscan_t yyscanner}
%pure-parser
%expect 0
%name-prefix="cube_yy"

%token CUBEFLOAT O_PAREN C_PAREN O_BRACKET C_BRACKET COMMA
%start box

/* Grammar follows */
%%

box: O_BRACKET paren_list COMMA paren_list C_BRACKET
	{
		int			dim;

		dim = item_count($2, ',');
		if (item_count($4, ',') != dim)
		{
			errsave(escontext,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for cube"),
					 errdetail("Different point dimensions in (%s) and (%s).",
							   $2, $4)));
			YYABORT;
		}
		if (dim > CUBE_MAX_DIM)
		{
			errsave(escontext,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for cube"),
					 errdetail("A cube cannot have more than %d dimensions.",
							   CUBE_MAX_DIM)));
			YYABORT;
		}

		if (!write_box(dim, $2, $4, result, escontext))
			YYABORT;

		(void) yynerrs;	/* suppress compiler warning */
	}

	| paren_list COMMA paren_list
	{
		int			dim;

		dim = item_count($1, ',');
		if (item_count($3, ',') != dim)
		{
			errsave(escontext,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for cube"),
					 errdetail("Different point dimensions in (%s) and (%s).",
							   $1, $3)));
			YYABORT;
		}
		if (dim > CUBE_MAX_DIM)
		{
			errsave(escontext,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for cube"),
					 errdetail("A cube cannot have more than %d dimensions.",
							   CUBE_MAX_DIM)));
			YYABORT;
		}

		if (!write_box(dim, $1, $3, result, escontext))
			YYABORT;
	}

	| paren_list
	{
		int			dim;

		dim = item_count($1, ',');
		if (dim > CUBE_MAX_DIM)
		{
			errsave(escontext,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for cube"),
					 errdetail("A cube cannot have more than %d dimensions.",
							   CUBE_MAX_DIM)));
			YYABORT;
		}

		if (!write_point_as_box(dim, $1, result, escontext))
			YYABORT;
	}

	| list
	{
		int			dim;

		dim = item_count($1, ',');
		if (dim > CUBE_MAX_DIM)
		{
			errsave(escontext,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for cube"),
					 errdetail("A cube cannot have more than %d dimensions.",
							   CUBE_MAX_DIM)));
			YYABORT;
		}

		if (!write_point_as_box(dim, $1, result, escontext))
			YYABORT;
	}
	;

paren_list: O_PAREN list C_PAREN
	{
		$$ = $2;
	}
	| O_PAREN C_PAREN
	{
		$$ = pstrdup("");
	}
	;

list: CUBEFLOAT
	{
		/* alloc enough space to be sure whole list will fit */
		$$ = palloc(scanbuflen + 1);
		strcpy($$, $1);
	}
	| list COMMA CUBEFLOAT
	{
		$$ = $1;
		strcat($$, ",");
		strcat($$, $3);
	}
	;

%%

/* This assumes the string has been normalized by productions above */
static int
item_count(const char *s, char delim)
{
	int			nitems = 0;

	if (s[0] != '\0')
	{
		nitems++;
		while ((s = strchr(s, delim)) != NULL)
		{
			nitems++;
			s++;
		}
	}
	return nitems;
}

static bool
write_box(int dim, char *str1, char *str2,
		  NDBOX **result, struct Node *escontext)
{
	NDBOX	   *bp;
	char	   *s;
	char	   *endptr;
	int			i;
	int			size = CUBE_SIZE(dim);
	bool		point = true;

	bp = palloc0(size);
	SET_VARSIZE(bp, size);
	SET_DIM(bp, dim);

	s = str1;
	i = 0;
	if (dim > 0)
	{
		bp->x[i++] = float8in_internal(s, &endptr, "cube", str1, escontext);
		if (SOFT_ERROR_OCCURRED(escontext))
			return false;
	}
	while ((s = strchr(s, ',')) != NULL)
	{
		s++;
		bp->x[i++] = float8in_internal(s, &endptr, "cube", str1, escontext);
		if (SOFT_ERROR_OCCURRED(escontext))
			return false;
	}
	Assert(i == dim);

	s = str2;
	if (dim > 0)
	{
		bp->x[i] = float8in_internal(s, &endptr, "cube", str2, escontext);
		if (SOFT_ERROR_OCCURRED(escontext))
			return false;
		/* code this way to do right thing with NaN */
		point &= (bp->x[i] == bp->x[0]);
		i++;
	}
	while ((s = strchr(s, ',')) != NULL)
	{
		s++;
		bp->x[i] = float8in_internal(s, &endptr, "cube", str2, escontext);
		if (SOFT_ERROR_OCCURRED(escontext))
			return false;
		point &= (bp->x[i] == bp->x[i - dim]);
		i++;
	}
	Assert(i == dim * 2);

	if (point)
	{
		/*
		 * The value turned out to be a point, ie. all the upper-right
		 * coordinates were equal to the lower-left coordinates. Resize the
		 * cube we constructed.  Note: we don't bother to repalloc() it
		 * smaller, as it's unlikely that the tiny amount of memory freed that
		 * way would be useful, and the output is always short-lived.
		 */
		size = POINT_SIZE(dim);
		SET_VARSIZE(bp, size);
		SET_POINT_BIT(bp);
	}

	*result = bp;
	return true;
}

static bool
write_point_as_box(int dim, char *str,
				   NDBOX **result, struct Node *escontext)
{
	NDBOX	   *bp;
	int			i,
				size;
	char	   *s;
	char	   *endptr;

	size = POINT_SIZE(dim);
	bp = palloc0(size);
	SET_VARSIZE(bp, size);
	SET_DIM(bp, dim);
	SET_POINT_BIT(bp);

	s = str;
	i = 0;
	if (dim > 0)
	{
		bp->x[i++] = float8in_internal(s, &endptr, "cube", str, escontext);
		if (SOFT_ERROR_OCCURRED(escontext))
			return false;
	}
	while ((s = strchr(s, ',')) != NULL)
	{
		s++;
		bp->x[i++] = float8in_internal(s, &endptr, "cube", str, escontext);
		if (SOFT_ERROR_OCCURRED(escontext))
			return false;
	}
	Assert(i == dim);

	*result = bp;
	return true;
}
