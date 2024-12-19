/*
 * contrib/seg/segdata.h
 */
typedef struct SEG
{
	float4		lower;
	float4		upper;
	char		l_sigd;
	char		u_sigd;
	char		l_ext;
	char		u_ext;
} SEG;

/* in seg.c */
extern int	significant_digits(const char *s);

/* for segscan.l and segparse.y */
union YYSTYPE;
#ifndef YY_TYPEDEF_YY_SCANNER_T
#define YY_TYPEDEF_YY_SCANNER_T
typedef void *yyscan_t;
#endif

/* in segscan.l */
extern int	seg_yylex(union YYSTYPE *yylval_param, yyscan_t yyscanner);
extern void seg_yyerror(SEG *result, struct Node *escontext,
						yyscan_t yyscanner,
						const char *message);
extern void seg_scanner_init(const char *str, yyscan_t *yyscannerp);
extern void seg_scanner_finish(yyscan_t yyscanner);

/* in segparse.y */
extern int	seg_yyparse(SEG *result, struct Node *escontext, yyscan_t yyscanner);
