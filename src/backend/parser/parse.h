typedef union
{
	double		dval;
	int			ival;
	char		chr;
	char	   *str;
	bool		boolean;
	bool	   *pboolean;		/* for pg_user privileges */
	List	   *list;
	Node	   *node;
	Value	   *value;

	Attr	   *attr;

	TypeName   *typnam;
	DefElem    *defelt;
	ParamString *param;
	SortGroupBy *sortgroupby;
	IndexElem  *ielem;
	RangeVar   *range;
	RelExpr    *relexp;
	A_Indices  *aind;
	ResTarget  *target;
	ParamNo    *paramno;

	VersionStmt *vstmt;
	DefineStmt *dstmt;
	RuleStmt   *rstmt;
	InsertStmt *astmt;
} YYSTYPE;

#define ACTION	258
#define ADD 259
#define ALL 260
#define ALTER	261
#define AND 262
#define ANY 263
#define AS	264
#define ASC 265
#define BEGIN_TRANS 266
#define BETWEEN 267
#define BOTH	268
#define BY	269
#define CASCADE 270
#define CAST	271
#define CHAR	272
#define CHARACTER	273
#define CHECK	274
#define CLOSE	275
#define COLLATE 276
#define COLUMN	277
#define COMMIT	278
#define CONSTRAINT	279
#define CREATE	280
#define CROSS	281
#define CURRENT 282
#define CURRENT_DATE	283
#define CURRENT_TIME	284
#define CURRENT_TIMESTAMP	285
#define CURRENT_USER	286
#define CURSOR	287
#define DAY_P	288
#define DECIMAL 289
#define DECLARE 290
#define DEFAULT 291
#define DELETE	292
#define DESC	293
#define DISTINCT	294
#define DOUBLE	295
#define DROP	296
#define END_TRANS	297
#define EXECUTE 298
#define EXISTS	299
#define EXTRACT 300
#define FETCH	301
#define FLOAT	302
#define FOR 303
#define FOREIGN 304
#define FROM	305
#define FULL	306
#define GRANT	307
#define GROUP	308
#define HAVING	309
#define HOUR_P	310
#define IN	311
#define INNER_P 312
#define INSERT	313
#define INTERVAL	314
#define INTO	315
#define IS	316
#define JOIN	317
#define KEY 318
#define LANGUAGE	319
#define LEADING 320
#define LEFT	321
#define LIKE	322
#define LOCAL	323
#define MATCH	324
#define MINUTE_P	325
#define MONTH_P 326
#define NATIONAL	327
#define NATURAL 328
#define NCHAR	329
#define NO	330
#define NOT 331
#define NOTIFY	332
#define NULL_P	333
#define NUMERIC 334
#define ON	335
#define OPTION	336
#define OR	337
#define ORDER	338
#define OUTER_P 339
#define PARTIAL 340
#define POSITION	341
#define PRECISION	342
#define PRIMARY 343
#define PRIVILEGES	344
#define PROCEDURE	345
#define PUBLIC	346
#define REFERENCES	347
#define REVOKE	348
#define RIGHT	349
#define ROLLBACK	350
#define SECOND_P	351
#define SELECT	352
#define SET 353
#define SUBSTRING	354
#define TABLE	355
#define TIME	356
#define TIMESTAMP	357
#define TO	358
#define TRAILING	359
#define TRANSACTION 360
#define TRIM	361
#define UNION	362
#define UNIQUE	363
#define UPDATE	364
#define USING	365
#define VALUES	366
#define VARCHAR 367
#define VARYING 368
#define VIEW	369
#define WHERE	370
#define WITH	371
#define WORK	372
#define YEAR_P	373
#define ZONE	374
#define FALSE_P 375
#define TRIGGER 376
#define TRUE_P	377
#define TYPE_P	378
#define ABORT_TRANS 379
#define AFTER	380
#define AGGREGATE	381
#define ANALYZE 382
#define BACKWARD	383
#define BEFORE	384
#define BINARY	385
#define CLUSTER 386
#define COPY	387
#define DATABASE	388
#define DELIMITERS	389
#define DO	390
#define EACH	391
#define EXPLAIN 392
#define EXTEND	393
#define FORWARD 394
#define FUNCTION	395
#define HANDLER 396
#define INDEX	397
#define INHERITS	398
#define INSTEAD 399
#define ISNULL	400
#define LANCOMPILER 401
#define LISTEN	402
#define LOAD	403
#define LOCK_P	404
#define LOCATION	405
#define MOVE	406
#define NEW 407
#define NONE	408
#define NOTHING 409
#define NOTNULL 410
#define OIDS	411
#define OPERATOR	412
#define PROCEDURAL	413
#define RECIPE	414
#define RENAME	415
#define RESET	416
#define RETURNS 417
#define ROW 418
#define RULE	419
#define SEQUENCE	420
#define SETOF	421
#define SHOW	422
#define STATEMENT	423
#define STDIN	424
#define STDOUT	425
#define TRUSTED 426
#define VACUUM	427
#define VERBOSE 428
#define VERSION 429
#define ARCHIVE 430
#define USER	431
#define PASSWORD	432
#define CREATEDB	433
#define NOCREATEDB	434
#define CREATEUSER	435
#define NOCREATEUSER	436
#define VALID	437
#define UNTIL	438
#define IDENT	439
#define SCONST	440
#define Op	441
#define ICONST	442
#define PARAM	443
#define FCONST	444
#define OP	445
#define UMINUS	446
#define TYPECAST	447
#define REDUCE	448


extern YYSTYPE yylval;
