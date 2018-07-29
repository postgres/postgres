%{
/* contrib/cube/cubeparse.y */

/* NdBox = [(lowerleft),(upperright)] */
/* [(xLL(1)...xLL(N)),(xUR(1)...xUR(n))] */

#include "postgres.h"

#include "cubedata.h"
#include "utils/float.h"

/* All grammar constructs return strings */
#define YYSTYPE char *

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

static char *scanbuf;
static int	scanbuflen;

static int item_count(const char *s, char delim);
static NDBOX *write_box(int dim, char *str1, char *str2);
static NDBOX *write_point_as_box(int dim, char *str);

%}

/* BISON Declarations */
%parse-param {NDBOX **result}
%expect 0
%name-prefix="cube_yy"

%token CUBEFLOAT O_PAREN C_PAREN O_BRACKET C_BRACKET COMMA
%start box

/* Grammar follows */
%%

box: O_BRACKET paren_list COMMA paren_list C_BRACKET
	{
		int dim;

		dim = item_count($2, ',');
		if (item_count($4, ',') != dim)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for cube"),
					 errdetail("Different point dimensions in (%s) and (%s).",
							   $2, $4)));
			YYABORT;
		}
		if (dim > CUBE_MAX_DIM)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for cube"),
					 errdetail("A cube cannot have more than %d dimensions.",
							   CUBE_MAX_DIM)));
			YYABORT;
		}

		*result = write_box( dim, $2, $4 );
	}

	| paren_list COMMA paren_list
	{
		int dim;

		dim = item_count($1, ',');
		if (item_count($3, ',') != dim)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for cube"),
					 errdetail("Different point dimensions in (%s) and (%s).",
							   $1, $3)));
			YYABORT;
		}
		if (dim > CUBE_MAX_DIM)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for cube"),
					 errdetail("A cube cannot have more than %d dimensions.",
							   CUBE_MAX_DIM)));
			YYABORT;
		}

		*result = write_box( dim, $1, $3 );
	}

	| paren_list
	{
		int dim;

		dim = item_count($1, ',');
		if (dim > CUBE_MAX_DIM)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for cube"),
					 errdetail("A cube cannot have more than %d dimensions.",
							   CUBE_MAX_DIM)));
			YYABORT;
		}

		*result = write_point_as_box(dim, $1);
	}

	| list
	{
		int dim;

		dim = item_count($1, ',');
		if (dim > CUBE_MAX_DIM)
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for cube"),
					 errdetail("A cube cannot have more than %d dimensions.",
							   CUBE_MAX_DIM)));
			YYABORT;
		}

		*result = write_point_as_box(dim, $1);
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

static NDBOX *
write_box(int dim, char *str1, char *str2)
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
		bp->x[i++] = float8in_internal(s, &endptr, "cube", str1);
	while ((s = strchr(s, ',')) != NULL)
	{
		s++;
		bp->x[i++] = float8in_internal(s, &endptr, "cube", str1);
	}
	Assert(i == dim);

	s = str2;
	if (dim > 0)
	{
		bp->x[i] = float8in_internal(s, &endptr, "cube", str2);
		/* code this way to do right thing with NaN */
		point &= (bp->x[i] == bp->x[0]);
		i++;
	}
	while ((s = strchr(s, ',')) != NULL)
	{
		s++;
		bp->x[i] = float8in_internal(s, &endptr, "cube", str2);
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
		 * smaller, as it's unlikely that the tiny amount of memory freed
		 * that way would be useful, and the output is always short-lived.
		 */
		size = POINT_SIZE(dim);
		SET_VARSIZE(bp, size);
		SET_POINT_BIT(bp);
	}

	return bp;
}

static NDBOX *
write_point_as_box(int dim, char *str)
{
	NDBOX		*bp;
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
		bp->x[i++] = float8in_internal(s, &endptr, "cube", str);
	while ((s = strchr(s, ',')) != NULL)
	{
		s++;
		bp->x[i++] = float8in_internal(s, &endptr, "cube", str);
	}
	Assert(i == dim);

	return bp;
}

#include "cubescan.c"
