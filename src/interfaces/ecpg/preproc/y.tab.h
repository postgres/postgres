typedef union {
    int				tagname;
    struct ECPGtemp_type	type;
    char *			symbolname;
    int				indexsize;
    enum ECPGttype		type_enum;
} YYSTYPE;
#define	SQL_START	258
#define	SQL_SEMI	259
#define	SQL_STRING	260
#define	SQL_INTO	261
#define	SQL_BEGIN	262
#define	SQL_END	263
#define	SQL_DECLARE	264
#define	SQL_SECTION	265
#define	SQL_INCLUDE	266
#define	SQL_CONNECT	267
#define	SQL_OPEN	268
#define	SQL_COMMIT	269
#define	SQL_ROLLBACK	270
#define	S_SYMBOL	271
#define	S_LENGTH	272
#define	S_ANYTHING	273
#define	S_VARCHAR	274
#define	S_VARCHAR2	275
#define	S_EXTERN	276
#define	S_STATIC	277
#define	S_UNSIGNED	278
#define	S_SIGNED	279
#define	S_LONG	280
#define	S_SHORT	281
#define	S_INT	282
#define	S_CHAR	283
#define	S_FLOAT	284
#define	S_DOUBLE	285
#define	S_BOOL	286


extern YYSTYPE yylval;
