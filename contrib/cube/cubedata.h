/* contrib/cube/cubedata.h */

/*
 * This limit is pretty arbitrary, but don't make it so large that you
 * risk overflow in sizing calculations.
 */
#define CUBE_MAX_DIM (100)

typedef struct NDBOX
{
	/* varlena header (do not touch directly!) */
	int32		vl_len_;

	/*----------
	 * Header contains info about NDBOX. For binary compatibility with old
	 * versions, it is defined as "unsigned int".
	 *
	 * Following information is stored:
	 *
	 *	bits 0-7  : number of cube dimensions;
	 *	bits 8-30 : unused, initialize to zero;
	 *	bit  31   : point flag. If set, the upper right coordinates are not
	 *				stored, and are implicitly the same as the lower left
	 *				coordinates.
	 *----------
	 */
	unsigned int header;

	/*
	 * The lower left coordinates for each dimension come first, followed by
	 * upper right coordinates unless the point flag is set.
	 */
	double		x[FLEXIBLE_ARRAY_MEMBER];
} NDBOX;

/* NDBOX access macros */
#define POINT_BIT			0x80000000
#define DIM_MASK			0x7fffffff

#define IS_POINT(cube)		( ((cube)->header & POINT_BIT) != 0 )
#define SET_POINT_BIT(cube) ( (cube)->header |= POINT_BIT )
#define DIM(cube)			( (cube)->header & DIM_MASK )
#define SET_DIM(cube, _dim) ( (cube)->header = ((cube)->header & ~DIM_MASK) | (_dim) )

#define LL_COORD(cube, i) ( (cube)->x[i] )
#define UR_COORD(cube, i) ( IS_POINT(cube) ? (cube)->x[i] : (cube)->x[(i) + DIM(cube)] )

#define POINT_SIZE(_dim)	(offsetof(NDBOX, x) + sizeof(double)*(_dim))
#define CUBE_SIZE(_dim)		(offsetof(NDBOX, x) + sizeof(double)*(_dim)*2)

/* fmgr interface macros */
#define DatumGetNDBOXP(x)	((NDBOX *) PG_DETOAST_DATUM(x))
#define PG_GETARG_NDBOX_P(x)	DatumGetNDBOXP(PG_GETARG_DATUM(x))
#define PG_RETURN_NDBOX_P(x)	PG_RETURN_POINTER(x)

/* GiST operator strategy numbers */
#define CubeKNNDistanceCoord			15	/* ~> */
#define CubeKNNDistanceTaxicab			16	/* <#> */
#define CubeKNNDistanceEuclid			17	/* <-> */
#define CubeKNNDistanceChebyshev		18	/* <=> */

/* for cubescan.l and cubeparse.y */
/* All grammar constructs return strings */
#define YYSTYPE char *
#ifndef YY_TYPEDEF_YY_SCANNER_T
#define YY_TYPEDEF_YY_SCANNER_T
typedef void *yyscan_t;
#endif

/* in cubescan.l */
extern int	cube_yylex(YYSTYPE *yylval_param, yyscan_t yyscanner);
extern void cube_yyerror(NDBOX **result, Size scanbuflen,
						 struct Node *escontext,
						 yyscan_t yyscanner,
						 const char *message);
extern void cube_scanner_init(const char *str, Size *scanbuflen, yyscan_t *yyscannerp);
extern void cube_scanner_finish(yyscan_t yyscanner);

/* in cubeparse.y */
extern int	cube_yyparse(NDBOX **result, Size scanbuflen,
						 struct Node *escontext,
						 yyscan_t yyscanner);
