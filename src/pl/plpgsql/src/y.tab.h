typedef union {
	int32			ival;
	char			*str;
	struct {
	    char *name;
	    int  lineno;
	}			varname;
	struct {
	    int  nalloc;
	    int	 nused;
	    int	 *dtnums;
	}			dtlist;
	struct {
	    int  reverse;
	    PLpgSQL_expr *expr;
	}			forilow;
	struct {
	    char *label;
	    int  n_initvars;
	    int  *initvarnos;
	}			declhdr;
	PLpgSQL_type		*dtype;
	PLpgSQL_var		*var;
	PLpgSQL_row		*row;
	PLpgSQL_rec		*rec;
	PLpgSQL_recfield	*recfield;
	PLpgSQL_trigarg		*trigarg;
	PLpgSQL_expr		*expr;
	PLpgSQL_stmt		*stmt;
	PLpgSQL_stmts		*stmts;
	PLpgSQL_stmt_block	*program;
	PLpgSQL_nsitem		*nsitem;
} YYSTYPE;
#define	K_ALIAS	258
#define	K_ASSIGN	259
#define	K_BEGIN	260
#define	K_CONSTANT	261
#define	K_DEBUG	262
#define	K_DECLARE	263
#define	K_DEFAULT	264
#define	K_DOTDOT	265
#define	K_ELSE	266
#define	K_END	267
#define	K_EXCEPTION	268
#define	K_EXIT	269
#define	K_FOR	270
#define	K_FROM	271
#define	K_IF	272
#define	K_IN	273
#define	K_INTO	274
#define	K_LOOP	275
#define	K_NOT	276
#define	K_NOTICE	277
#define	K_NULL	278
#define	K_PERFORM	279
#define	K_RAISE	280
#define	K_RECORD	281
#define	K_RENAME	282
#define	K_RETURN	283
#define	K_REVERSE	284
#define	K_SELECT	285
#define	K_THEN	286
#define	K_TO	287
#define	K_TYPE	288
#define	K_WHEN	289
#define	K_WHILE	290
#define	T_FUNCTION	291
#define	T_TRIGGER	292
#define	T_CHAR	293
#define	T_BPCHAR	294
#define	T_VARCHAR	295
#define	T_LABEL	296
#define	T_STRING	297
#define	T_VARIABLE	298
#define	T_ROW	299
#define	T_ROWTYPE	300
#define	T_RECORD	301
#define	T_RECFIELD	302
#define	T_TGARGV	303
#define	T_DTYPE	304
#define	T_WORD	305
#define	T_NUMBER	306
#define	T_ERROR	307
#define	O_OPTION	308
#define	O_DUMP	309


extern YYSTYPE yylval;
