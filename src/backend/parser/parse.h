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
#define	ACTION	258
#define	ADD	259
#define	ALL	260
#define	ALTER	261
#define	AND	262
#define	ANY	263
#define	AS	264
#define	ASC	265
#define	BEGIN_TRANS	266
#define	BETWEEN	267
#define	BOTH	268
#define	BY	269
#define	CASCADE	270
#define	CAST	271
#define	CHAR	272
#define	CHARACTER	273
#define	CHECK	274
#define	CLOSE	275
#define	COLLATE	276
#define	COLUMN	277
#define	COMMIT	278
#define	CONSTRAINT	279
#define	CREATE	280
#define	CROSS	281
#define	CURRENT	282
#define	CURRENT_DATE	283
#define	CURRENT_TIME	284
#define	CURRENT_TIMESTAMP	285
#define	CURRENT_USER	286
#define	CURSOR	287
#define	DAY_P	288
#define	DECIMAL	289
#define	DECLARE	290
#define	DEFAULT	291
#define	DELETE	292
#define	DESC	293
#define	DISTINCT	294
#define	DOUBLE	295
#define	DROP	296
#define	END_TRANS	297
#define	EXECUTE	298
#define	EXISTS	299
#define	EXTRACT	300
#define	FETCH	301
#define	FLOAT	302
#define	FOR	303
#define	FOREIGN	304
#define	FROM	305
#define	FULL	306
#define	GRANT	307
#define	GROUP	308
#define	HAVING	309
#define	HOUR_P	310
#define	IN	311
#define	INNER_P	312
#define	INSERT	313
#define	INTERVAL	314
#define	INTO	315
#define	IS	316
#define	JOIN	317
#define	KEY	318
#define	LANGUAGE	319
#define	LEADING	320
#define	LEFT	321
#define	LIKE	322
#define	LOCAL	323
#define	MATCH	324
#define	MINUTE_P	325
#define	MONTH_P	326
#define	NAMES	327
#define	NATIONAL	328
#define	NATURAL	329
#define	NCHAR	330
#define	NO	331
#define	NOT	332
#define	NOTIFY	333
#define	NULL_P	334
#define	NUMERIC	335
#define	ON	336
#define	OPTION	337
#define	OR	338
#define	ORDER	339
#define	OUTER_P	340
#define	PARTIAL	341
#define	POSITION	342
#define	PRECISION	343
#define	PRIMARY	344
#define	PRIVILEGES	345
#define	PROCEDURE	346
#define	PUBLIC	347
#define	REFERENCES	348
#define	REVOKE	349
#define	RIGHT	350
#define	ROLLBACK	351
#define	SECOND_P	352
#define	SELECT	353
#define	SET	354
#define	SUBSTRING	355
#define	TABLE	356
#define	TIME	357
#define	TIMESTAMP	358
#define	TIMEZONE_HOUR	359
#define	TIMEZONE_MINUTE	360
#define	TO	361
#define	TRAILING	362
#define	TRANSACTION	363
#define	TRIM	364
#define	UNION	365
#define	UNIQUE	366
#define	UPDATE	367
#define	USER	368
#define	USING	369
#define	VALUES	370
#define	VARCHAR	371
#define	VARYING	372
#define	VIEW	373
#define	WHERE	374
#define	WITH	375
#define	WORK	376
#define	YEAR_P	377
#define	ZONE	378
#define	FALSE_P	379
#define	TRIGGER	380
#define	TRUE_P	381
#define	TYPE_P	382
#define	ABORT_TRANS	383
#define	AFTER	384
#define	AGGREGATE	385
#define	ANALYZE	386
#define	BACKWARD	387
#define	BEFORE	388
#define	BINARY	389
#define	CACHE	390
#define	CLUSTER	391
#define	COPY	392
#define	CYCLE	393
#define	DATABASE	394
#define	DELIMITERS	395
#define	DO	396
#define	EACH	397
#define	EXPLAIN	398
#define	EXTEND	399
#define	FORWARD	400
#define	FUNCTION	401
#define	HANDLER	402
#define	INCREMENT	403
#define	INDEX	404
#define	INHERITS	405
#define	INSTEAD	406
#define	ISNULL	407
#define	LANCOMPILER	408
#define	LISTEN	409
#define	LOAD	410
#define	LOCK_P	411
#define	LOCATION	412
#define	MAXVALUE	413
#define	MINVALUE	414
#define	MOVE	415
#define	NEW	416
#define	NONE	417
#define	NOTHING	418
#define	NOTNULL	419
#define	OIDS	420
#define	OPERATOR	421
#define	PROCEDURAL	422
#define	RECIPE	423
#define	RENAME	424
#define	RESET	425
#define	RETURNS	426
#define	ROW	427
#define	RULE	428
#define	SEQUENCE	429
#define	SETOF	430
#define	SHOW	431
#define	START	432
#define	STATEMENT	433
#define	STDIN	434
#define	STDOUT	435
#define	TRUSTED	436
#define	VACUUM	437
#define	VERBOSE	438
#define	VERSION	439
#define	ENCODING	440
#define	ARCHIVE	441
#define	PASSWORD	442
#define	CREATEDB	443
#define	NOCREATEDB	444
#define	CREATEUSER	445
#define	NOCREATEUSER	446
#define	VALID	447
#define	UNTIL	448
#define	IDENT	449
#define	SCONST	450
#define	Op	451
#define	ICONST	452
#define	PARAM	453
#define	FCONST	454
#define	OP	455
#define	UMINUS	456
#define	TYPECAST	457


extern YYSTYPE yylval;
