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
#define	EXCEPT	302
#define	EXECUTE	303
#define	EXISTS	304
#define	EXTRACT	305
#define	FALSE_P	306
#define	FETCH	307
#define	FLOAT	308
#define	FOR	309
#define	FOREIGN	310
#define	FROM	311
#define	FULL	312
#define	GRANT	313
#define	GROUP	314
#define	HAVING	315
#define	HOUR_P	316
#define	IN	317
#define	INNER_P	318
#define	INSENSITIVE	319
#define	INSERT	320
#define	INTERSECT	321
#define	INTERVAL	322
#define	INTO	323
#define	IS	324
#define	ISOLATION	325
#define	JOIN	326
#define	KEY	327
#define	LANGUAGE	328
#define	LEADING	329
#define	LEFT	330
#define	LEVEL	331
#define	LIKE	332
#define	LOCAL	333
#define	MATCH	334
#define	MINUTE_P	335
#define	MONTH_P	336
#define	NAMES	337
#define	NATIONAL	338
#define	NATURAL	339
#define	NCHAR	340
#define	NEXT	341
#define	NO	342
#define	NOT	343
#define	NULLIF	344
#define	NULL_P	345
#define	NUMERIC	346
#define	OF	347
#define	ON	348
#define	ONLY	349
#define	OPTION	350
#define	OR	351
#define	ORDER	352
#define	OUTER_P	353
#define	PARTIAL	354
#define	POSITION	355
#define	PRECISION	356
#define	PRIMARY	357
#define	PRIOR	358
#define	PRIVILEGES	359
#define	PROCEDURE	360
#define	PUBLIC	361
#define	READ	362
#define	REFERENCES	363
#define	RELATIVE	364
#define	REVOKE	365
#define	RIGHT	366
#define	ROLLBACK	367
#define	SCROLL	368
#define	SECOND_P	369
#define	SELECT	370
#define	SET	371
#define	SUBSTRING	372
#define	TABLE	373
#define	THEN	374
#define	TIME	375
#define	TIMESTAMP	376
#define	TIMEZONE_HOUR	377
#define	TIMEZONE_MINUTE	378
#define	TO	379
#define	TRAILING	380
#define	TRANSACTION	381
#define	TRIM	382
#define	TRUE_P	383
#define	UNION	384
#define	UNIQUE	385
#define	UPDATE	386
#define	USER	387
#define	USING	388
#define	VALUES	389
#define	VARCHAR	390
#define	VARYING	391
#define	VIEW	392
#define	WHEN	393
#define	WHERE	394
#define	WITH	395
#define	WORK	396
#define	YEAR_P	397
#define	ZONE	398
#define	TRIGGER	399
#define	TYPE_P	400
#define	ABORT_TRANS	401
#define	AFTER	402
#define	AGGREGATE	403
#define	ANALYZE	404
#define	BACKWARD	405
#define	BEFORE	406
#define	BINARY	407
#define	CACHE	408
#define	CLUSTER	409
#define	COPY	410
#define	CREATEDB	411
#define	CREATEUSER	412
#define	CYCLE	413
#define	DATABASE	414
#define	DELIMITERS	415
#define	DO	416
#define	EACH	417
#define	ENCODING	418
#define	EXPLAIN	419
#define	EXTEND	420
#define	FORWARD	421
#define	FUNCTION	422
#define	HANDLER	423
#define	INCREMENT	424
#define	INDEX	425
#define	INHERITS	426
#define	INSTEAD	427
#define	ISNULL	428
#define	LANCOMPILER	429
#define	LISTEN	430
#define	LOAD	431
#define	LOCATION	432
#define	LOCK_P	433
#define	MAXVALUE	434
#define	MINVALUE	435
#define	MOVE	436
#define	NEW	437
#define	NOCREATEDB	438
#define	NOCREATEUSER	439
#define	NONE	440
#define	NOTHING	441
#define	NOTIFY	442
#define	NOTNULL	443
#define	OIDS	444
#define	OPERATOR	445
#define	PASSWORD	446
#define	PROCEDURAL	447
#define	RECIPE	448
#define	RENAME	449
#define	RESET	450
#define	RETURNS	451
#define	ROW	452
#define	RULE	453
#define	SEQUENCE	454
#define	SERIAL	455
#define	SETOF	456
#define	SHOW	457
#define	START	458
#define	STATEMENT	459
#define	STDIN	460
#define	STDOUT	461
#define	TRUSTED	462
#define	UNLISTEN	463
#define	UNTIL	464
#define	VACUUM	465
#define	VALID	466
#define	VERBOSE	467
#define	VERSION	468
#define	IDENT	469
#define	SCONST	470
#define	Op	471
#define	ICONST	472
#define	PARAM	473
#define	FCONST	474
#define	OP	475
#define	UMINUS	476
#define	TYPECAST	477


extern YYSTYPE yylval;
