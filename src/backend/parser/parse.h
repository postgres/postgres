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
#define	FETCH	302
#define	FLOAT	303
#define	FOR	304
#define	FOREIGN	305
#define	FROM	306
#define	FULL	307
#define	GRANT	308
#define	GROUP	309
#define	HAVING	310
#define	HOUR_P	311
#define	IN	312
#define	INNER_P	313
#define	INSENSITIVE	314
#define	INSERT	315
#define	INTERVAL	316
#define	INTO	317
#define	IS	318
#define	JOIN	319
#define	KEY	320
#define	LANGUAGE	321
#define	LEADING	322
#define	LEFT	323
#define	LIKE	324
#define	LOCAL	325
#define	MATCH	326
#define	MINUTE_P	327
#define	MONTH_P	328
#define	NAMES	329
#define	NATIONAL	330
#define	NATURAL	331
#define	NCHAR	332
#define	NEXT	333
#define	NO	334
#define	NOT	335
#define	NOTIFY	336
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
#define	UNION	374
#define	UNIQUE	375
#define	UPDATE	376
#define	USER	377
#define	USING	378
#define	VALUES	379
#define	VARCHAR	380
#define	VARYING	381
#define	VIEW	382
#define	WHERE	383
#define	WITH	384
#define	WORK	385
#define	YEAR_P	386
#define	ZONE	387
#define	FALSE_P	388
#define	TRIGGER	389
#define	TRUE_P	390
#define	TYPE_P	391
#define	ABORT_TRANS	392
#define	AFTER	393
#define	AGGREGATE	394
#define	ANALYZE	395
#define	BACKWARD	396
#define	BEFORE	397
#define	BINARY	398
#define	CACHE	399
#define	CLUSTER	400
#define	COPY	401
#define	CYCLE	402
#define	DATABASE	403
#define	DELIMITERS	404
#define	DO	405
#define	EACH	406
#define	EXPLAIN	407
#define	EXTEND	408
#define	FORWARD	409
#define	FUNCTION	410
#define	HANDLER	411
#define	INCREMENT	412
#define	INDEX	413
#define	INHERITS	414
#define	INSTEAD	415
#define	ISNULL	416
#define	LANCOMPILER	417
#define	LISTEN	418
#define	LOAD	419
#define	LOCK_P	420
#define	LOCATION	421
#define	MAXVALUE	422
#define	MINVALUE	423
#define	MOVE	424
#define	NEW	425
#define	NONE	426
#define	NOTHING	427
#define	NOTNULL	428
#define	OIDS	429
#define	OPERATOR	430
#define	PROCEDURAL	431
#define	RECIPE	432
#define	RENAME	433
#define	RESET	434
#define	RETURNS	435
#define	ROW	436
#define	RULE	437
#define	SEQUENCE	438
#define	SERIAL	439
#define	SETOF	440
#define	SHOW	441
#define	START	442
#define	STATEMENT	443
#define	STDIN	444
#define	STDOUT	445
#define	TRUSTED	446
#define	VACUUM	447
#define	VERBOSE	448
#define	VERSION	449
#define	ENCODING	450
#define	UNLISTEN	451
#define	ARCHIVE	452
#define	PASSWORD	453
#define	CREATEDB	454
#define	NOCREATEDB	455
#define	CREATEUSER	456
#define	NOCREATEUSER	457
#define	VALID	458
#define	UNTIL	459
#define	IDENT	460
#define	SCONST	461
#define	Op	462
#define	ICONST	463
#define	PARAM	464
#define	FCONST	465
#define	OP	466
#define	UMINUS	467
#define	TYPECAST	468


extern YYSTYPE yylval;
