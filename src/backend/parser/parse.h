typedef union
{
	double				dval;
	int					ival;
	char				chr;
	char				*str;
	bool				boolean;
	bool*				pboolean;	/* for pg_shadow privileges */
	List				*list;
	Node				*node;
	Value				*value;

	Attr				*attr;

	TypeName			*typnam;
	DefElem				*defelt;
	ParamString			*param;
	SortGroupBy			*sortgroupby;
	JoinUsing			*joinusing;
	IndexElem			*ielem;
	RangeVar			*range;
	RelExpr				*relexp;
	A_Indices			*aind;
	ResTarget			*target;
	ParamNo				*paramno;

	VersionStmt			*vstmt;
	DefineStmt			*dstmt;
	RuleStmt			*rstmt;
	InsertStmt			*astmt;
} YYSTYPE;
#define	ABSOLUTE	258
#define	ACTION	259
#define	ADD	260
#define	ALL	261
#define	ALTER	262
#define	AND	263
#define	ANY	264
#define	AS	265
#define	ASC	266
#define	BEGIN_TRANS	267
#define	BETWEEN	268
#define	BOTH	269
#define	BY	270
#define	CASCADE	271
#define	CASE	272
#define	CAST	273
#define	CHAR	274
#define	CHARACTER	275
#define	CHECK	276
#define	CLOSE	277
#define	COALESCE	278
#define	COLLATE	279
#define	COLUMN	280
#define	COMMIT	281
#define	CONSTRAINT	282
#define	CREATE	283
#define	CROSS	284
#define	CURRENT	285
#define	CURRENT_DATE	286
#define	CURRENT_TIME	287
#define	CURRENT_TIMESTAMP	288
#define	CURRENT_USER	289
#define	CURSOR	290
#define	DAY_P	291
#define	DECIMAL	292
#define	DECLARE	293
#define	DEFAULT	294
#define	DELETE	295
#define	DESC	296
#define	DISTINCT	297
#define	DOUBLE	298
#define	DROP	299
#define	ELSE	300
#define	END_TRANS	301
#define	EXECUTE	302
#define	EXISTS	303
#define	EXTRACT	304
#define	FALSE_P	305
#define	FETCH	306
#define	FLOAT	307
#define	FOR	308
#define	FOREIGN	309
#define	FROM	310
#define	FULL	311
#define	GRANT	312
#define	GROUP	313
#define	HAVING	314
#define	HOUR_P	315
#define	IN	316
#define	INNER_P	317
#define	INSENSITIVE	318
#define	INSERT	319
#define	INTERVAL	320
#define	INTO	321
#define	IS	322
#define	ISOLATION	323
#define	JOIN	324
#define	KEY	325
#define	LANGUAGE	326
#define	LEADING	327
#define	LEFT	328
#define	LEVEL	329
#define	LIKE	330
#define	LOCAL	331
#define	MATCH	332
#define	MINUTE_P	333
#define	MONTH_P	334
#define	NAMES	335
#define	NATIONAL	336
#define	NATURAL	337
#define	NCHAR	338
#define	NEXT	339
#define	NO	340
#define	NOT	341
#define	NULLIF	342
#define	NULL_P	343
#define	NUMERIC	344
#define	OF	345
#define	ON	346
#define	ONLY	347
#define	OPTION	348
#define	OR	349
#define	ORDER	350
#define	OUTER_P	351
#define	PARTIAL	352
#define	POSITION	353
#define	PRECISION	354
#define	PRIMARY	355
#define	PRIOR	356
#define	PRIVILEGES	357
#define	PROCEDURE	358
#define	PUBLIC	359
#define	READ	360
#define	REFERENCES	361
#define	RELATIVE	362
#define	REVOKE	363
#define	RIGHT	364
#define	ROLLBACK	365
#define	SCROLL	366
#define	SECOND_P	367
#define	SELECT	368
#define	SET	369
#define	SUBSTRING	370
#define	TABLE	371
#define	THEN	372
#define	TIME	373
#define	TIMESTAMP	374
#define	TIMEZONE_HOUR	375
#define	TIMEZONE_MINUTE	376
#define	TO	377
#define	TRAILING	378
#define	TRANSACTION	379
#define	TRIM	380
#define	TRUE_P	381
#define	UNION	382
#define	UNIQUE	383
#define	UPDATE	384
#define	USER	385
#define	USING	386
#define	VALUES	387
#define	VARCHAR	388
#define	VARYING	389
#define	VIEW	390
#define	WHEN	391
#define	WHERE	392
#define	WITH	393
#define	WORK	394
#define	YEAR_P	395
#define	ZONE	396
#define	TRIGGER	397
#define	TYPE_P	398
#define	ABORT_TRANS	399
#define	AFTER	400
#define	AGGREGATE	401
#define	ANALYZE	402
#define	BACKWARD	403
#define	BEFORE	404
#define	BINARY	405
#define	CACHE	406
#define	CLUSTER	407
#define	COPY	408
#define	CREATEDB	409
#define	CREATEUSER	410
#define	CYCLE	411
#define	DATABASE	412
#define	DELIMITERS	413
#define	DO	414
#define	EACH	415
#define	ENCODING	416
#define	EXPLAIN	417
#define	EXTEND	418
#define	FORWARD	419
#define	FUNCTION	420
#define	HANDLER	421
#define	INCREMENT	422
#define	INDEX	423
#define	INHERITS	424
#define	INSTEAD	425
#define	ISNULL	426
#define	LANCOMPILER	427
#define	LISTEN	428
#define	LOAD	429
#define	LOCATION	430
#define	LOCK_P	431
#define	MAXVALUE	432
#define	MINVALUE	433
#define	MOVE	434
#define	NEW	435
#define	NOCREATEDB	436
#define	NOCREATEUSER	437
#define	NONE	438
#define	NOTHING	439
#define	NOTIFY	440
#define	NOTNULL	441
#define	OIDS	442
#define	OPERATOR	443
#define	PASSWORD	444
#define	PROCEDURAL	445
#define	RECIPE	446
#define	RENAME	447
#define	RESET	448
#define	RETURNS	449
#define	ROW	450
#define	RULE	451
#define	SEQUENCE	452
#define	SERIAL	453
#define	SETOF	454
#define	SHOW	455
#define	START	456
#define	STATEMENT	457
#define	STDIN	458
#define	STDOUT	459
#define	TRUSTED	460
#define	UNLISTEN	461
#define	UNTIL	462
#define	VACUUM	463
#define	VALID	464
#define	VERBOSE	465
#define	VERSION	466
#define	IDENT	467
#define	SCONST	468
#define	Op	469
#define	ICONST	470
#define	PARAM	471
#define	FCONST	472
#define	OP	473
#define	UMINUS	474
#define	TYPECAST	475


extern YYSTYPE yylval;
