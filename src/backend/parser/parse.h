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
#define	CAST	272
#define	CHAR	273
#define	CHARACTER	274
#define	CHECK	275
#define	CLOSE	276
#define	COLLATE	277
#define	COLUMN	278
#define	COMMIT	279
#define	CONSTRAINT	280
#define	CREATE	281
#define	CROSS	282
#define	CURRENT	283
#define	CURRENT_DATE	284
#define	CURRENT_TIME	285
#define	CURRENT_TIMESTAMP	286
#define	CURRENT_USER	287
#define	CURSOR	288
#define	DAY_P	289
#define	DECIMAL	290
#define	DECLARE	291
#define	DEFAULT	292
#define	DELETE	293
#define	DESC	294
#define	DISTINCT	295
#define	DOUBLE	296
#define	DROP	297
#define	END_TRANS	298
#define	EXECUTE	299
#define	EXISTS	300
#define	EXTRACT	301
#define	FALSE_P	302
#define	FETCH	303
#define	FLOAT	304
#define	FOR	305
#define	FOREIGN	306
#define	FROM	307
#define	FULL	308
#define	GRANT	309
#define	GROUP	310
#define	HAVING	311
#define	HOUR_P	312
#define	IN	313
#define	INNER_P	314
#define	INSENSITIVE	315
#define	INSERT	316
#define	INTERVAL	317
#define	INTO	318
#define	IS	319
#define	JOIN	320
#define	KEY	321
#define	LANGUAGE	322
#define	LEADING	323
#define	LEFT	324
#define	LIKE	325
#define	LOCAL	326
#define	MATCH	327
#define	MINUTE_P	328
#define	MONTH_P	329
#define	NAMES	330
#define	NATIONAL	331
#define	NATURAL	332
#define	NCHAR	333
#define	NEXT	334
#define	NO	335
#define	NOT	336
#define	NULL_P	337
#define	NUMERIC	338
#define	OF	339
#define	ON	340
#define	ONLY	341
#define	OPTION	342
#define	OR	343
#define	ORDER	344
#define	OUTER_P	345
#define	PARTIAL	346
#define	POSITION	347
#define	PRECISION	348
#define	PRIMARY	349
#define	PRIOR	350
#define	PRIVILEGES	351
#define	PROCEDURE	352
#define	PUBLIC	353
#define	READ	354
#define	REFERENCES	355
#define	RELATIVE	356
#define	REVOKE	357
#define	RIGHT	358
#define	ROLLBACK	359
#define	SCROLL	360
#define	SECOND_P	361
#define	SELECT	362
#define	SET	363
#define	SUBSTRING	364
#define	TABLE	365
#define	TIME	366
#define	TIMESTAMP	367
#define	TIMEZONE_HOUR	368
#define	TIMEZONE_MINUTE	369
#define	TO	370
#define	TRAILING	371
#define	TRANSACTION	372
#define	TRIM	373
#define	TRUE_P	374
#define	UNION	375
#define	UNIQUE	376
#define	UPDATE	377
#define	USER	378
#define	USING	379
#define	VALUES	380
#define	VARCHAR	381
#define	VARYING	382
#define	VIEW	383
#define	WHERE	384
#define	WITH	385
#define	WORK	386
#define	YEAR_P	387
#define	ZONE	388
#define	TRIGGER	389
#define	TYPE_P	390
#define	ABORT_TRANS	391
#define	AFTER	392
#define	AGGREGATE	393
#define	ANALYZE	394
#define	BACKWARD	395
#define	BEFORE	396
#define	BINARY	397
#define	CACHE	398
#define	CLUSTER	399
#define	COPY	400
#define	CREATEDB	401
#define	CREATEUSER	402
#define	CYCLE	403
#define	DATABASE	404
#define	DELIMITERS	405
#define	DO	406
#define	EACH	407
#define	ENCODING	408
#define	EXPLAIN	409
#define	EXTEND	410
#define	FORWARD	411
#define	FUNCTION	412
#define	HANDLER	413
#define	INCREMENT	414
#define	INDEX	415
#define	INHERITS	416
#define	INSTEAD	417
#define	ISNULL	418
#define	LANCOMPILER	419
#define	LISTEN	420
#define	LOAD	421
#define	LOCATION	422
#define	LOCK_P	423
#define	MAXVALUE	424
#define	MINVALUE	425
#define	MOVE	426
#define	NEW	427
#define	NOCREATEDB	428
#define	NOCREATEUSER	429
#define	NONE	430
#define	NOTHING	431
#define	NOTIFY	432
#define	NOTNULL	433
#define	OIDS	434
#define	OPERATOR	435
#define	PASSWORD	436
#define	PROCEDURAL	437
#define	RECIPE	438
#define	RENAME	439
#define	RESET	440
#define	RETURNS	441
#define	ROW	442
#define	RULE	443
#define	SEQUENCE	444
#define	SERIAL	445
#define	SETOF	446
#define	SHOW	447
#define	START	448
#define	STATEMENT	449
#define	STDIN	450
#define	STDOUT	451
#define	TRUSTED	452
#define	UNLISTEN	453
#define	UNTIL	454
#define	VACUUM	455
#define	VALID	456
#define	VERBOSE	457
#define	VERSION	458
#define	IDENT	459
#define	SCONST	460
#define	Op	461
#define	ICONST	462
#define	PARAM	463
#define	FCONST	464
#define	OP	465
#define	UMINUS	466
#define	TYPECAST	467


extern YYSTYPE yylval;
