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
#define	JOIN	323
#define	KEY	324
#define	LANGUAGE	325
#define	LEADING	326
#define	LEFT	327
#define	LIKE	328
#define	LOCAL	329
#define	MATCH	330
#define	MINUTE_P	331
#define	MONTH_P	332
#define	NAMES	333
#define	NATIONAL	334
#define	NATURAL	335
#define	NCHAR	336
#define	NEXT	337
#define	NO	338
#define	NOT	339
#define	NULLIF	340
#define	NULL_P	341
#define	NUMERIC	342
#define	OF	343
#define	ON	344
#define	ONLY	345
#define	OPTION	346
#define	OR	347
#define	ORDER	348
#define	OUTER_P	349
#define	PARTIAL	350
#define	POSITION	351
#define	PRECISION	352
#define	PRIMARY	353
#define	PRIOR	354
#define	PRIVILEGES	355
#define	PROCEDURE	356
#define	PUBLIC	357
#define	READ	358
#define	REFERENCES	359
#define	RELATIVE	360
#define	REVOKE	361
#define	RIGHT	362
#define	ROLLBACK	363
#define	SCROLL	364
#define	SECOND_P	365
#define	SELECT	366
#define	SET	367
#define	SUBSTRING	368
#define	TABLE	369
#define	THEN	370
#define	TIME	371
#define	TIMESTAMP	372
#define	TIMEZONE_HOUR	373
#define	TIMEZONE_MINUTE	374
#define	TO	375
#define	TRAILING	376
#define	TRANSACTION	377
#define	TRIM	378
#define	TRUE_P	379
#define	UNION	380
#define	UNIQUE	381
#define	UPDATE	382
#define	USER	383
#define	USING	384
#define	VALUES	385
#define	VARCHAR	386
#define	VARYING	387
#define	VIEW	388
#define	WHEN	389
#define	WHERE	390
#define	WITH	391
#define	WORK	392
#define	YEAR_P	393
#define	ZONE	394
#define	TRIGGER	395
#define	TYPE_P	396
#define	ABORT_TRANS	397
#define	AFTER	398
#define	AGGREGATE	399
#define	ANALYZE	400
#define	BACKWARD	401
#define	BEFORE	402
#define	BINARY	403
#define	CACHE	404
#define	CLUSTER	405
#define	COPY	406
#define	CREATEDB	407
#define	CREATEUSER	408
#define	CYCLE	409
#define	DATABASE	410
#define	DELIMITERS	411
#define	DO	412
#define	EACH	413
#define	ENCODING	414
#define	EXPLAIN	415
#define	EXTEND	416
#define	FORWARD	417
#define	FUNCTION	418
#define	HANDLER	419
#define	INCREMENT	420
#define	INDEX	421
#define	INHERITS	422
#define	INSTEAD	423
#define	ISNULL	424
#define	LANCOMPILER	425
#define	LISTEN	426
#define	LOAD	427
#define	LOCATION	428
#define	LOCK_P	429
#define	MAXVALUE	430
#define	MINVALUE	431
#define	MOVE	432
#define	NEW	433
#define	NOCREATEDB	434
#define	NOCREATEUSER	435
#define	NONE	436
#define	NOTHING	437
#define	NOTIFY	438
#define	NOTNULL	439
#define	OIDS	440
#define	OPERATOR	441
#define	PASSWORD	442
#define	PROCEDURAL	443
#define	RECIPE	444
#define	RENAME	445
#define	RESET	446
#define	RETURNS	447
#define	ROW	448
#define	RULE	449
#define	SEQUENCE	450
#define	SERIAL	451
#define	SETOF	452
#define	SHOW	453
#define	START	454
#define	STATEMENT	455
#define	STDIN	456
#define	STDOUT	457
#define	TRUSTED	458
#define	UNLISTEN	459
#define	UNTIL	460
#define	VACUUM	461
#define	VALID	462
#define	VERBOSE	463
#define	VERSION	464
#define	IDENT	465
#define	SCONST	466
#define	Op	467
#define	ICONST	468
#define	PARAM	469
#define	FCONST	470
#define	OP	471
#define	UMINUS	472
#define	TYPECAST	473


extern YYSTYPE yylval;
