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
#define	NATIONAL	327
#define	NATURAL	328
#define	NCHAR	329
#define	NO	330
#define	NOT	331
#define	NOTIFY	332
#define	NULL_P	333
#define	NUMERIC	334
#define	ON	335
#define	OPTION	336
#define	OR	337
#define	ORDER	338
#define	OUTER_P	339
#define	PARTIAL	340
#define	POSITION	341
#define	PRECISION	342
#define	PRIMARY	343
#define	PRIVILEGES	344
#define	PROCEDURE	345
#define	PUBLIC	346
#define	REFERENCES	347
#define	REVOKE	348
#define	RIGHT	349
#define	ROLLBACK	350
#define	SECOND_P	351
#define	SELECT	352
#define	SET	353
#define	SUBSTRING	354
#define	TABLE	355
#define	TIME	356
#define	TIMESTAMP	357
#define	TIMEZONE_HOUR	358
#define	TIMEZONE_MINUTE	359
#define	TO	360
#define	TRAILING	361
#define	TRANSACTION	362
#define	TRIM	363
#define	UNION	364
#define	UNIQUE	365
#define	UPDATE	366
#define	USER	367
#define	USING	368
#define	VALUES	369
#define	VARCHAR	370
#define	VARYING	371
#define	VIEW	372
#define	WHERE	373
#define	WITH	374
#define	WORK	375
#define	YEAR_P	376
#define	ZONE	377
#define	FALSE_P	378
#define	TRIGGER	379
#define	TRUE_P	380
#define	TYPE_P	381
#define	ABORT_TRANS	382
#define	AFTER	383
#define	AGGREGATE	384
#define	ANALYZE	385
#define	BACKWARD	386
#define	BEFORE	387
#define	BINARY	388
#define	CACHE	389
#define	CLUSTER	390
#define	COPY	391
#define	CYCLE	392
#define	DATABASE	393
#define	DELIMITERS	394
#define	DO	395
#define	EACH	396
#define	EXPLAIN	397
#define	EXTEND	398
#define	FORWARD	399
#define	FUNCTION	400
#define	HANDLER	401
#define	INCREMENT	402
#define	INDEX	403
#define	INHERITS	404
#define	INSTEAD	405
#define	ISNULL	406
#define	LANCOMPILER	407
#define	LISTEN	408
#define	LOAD	409
#define	LOCK_P	410
#define	LOCATION	411
#define	MAXVALUE	412
#define	MINVALUE	413
#define	MOVE	414
#define	NEW	415
#define	NONE	416
#define	NOTHING	417
#define	NOTNULL	418
#define	OIDS	419
#define	OPERATOR	420
#define	PROCEDURAL	421
#define	RECIPE	422
#define	RENAME	423
#define	RESET	424
#define	RETURNS	425
#define	ROW	426
#define	RULE	427
#define	SEQUENCE	428
#define	SETOF	429
#define	SHOW	430
#define	START	431
#define	STATEMENT	432
#define	STDIN	433
#define	STDOUT	434
#define	TRUSTED	435
#define	VACUUM	436
#define	VERBOSE	437
#define	VERSION	438
#define	ARCHIVE	439
#define	PASSWORD	440
#define	CREATEDB	441
#define	NOCREATEDB	442
#define	CREATEUSER	443
#define	NOCREATEUSER	444
#define	VALID	445
#define	UNTIL	446
#define	IDENT	447
#define	SCONST	448
#define	Op	449
#define	ICONST	450
#define	PARAM	451
#define	FCONST	452
#define	OP	453
#define	UMINUS	454
#define	TYPECAST	455
#define	REDUCE	456


extern YYSTYPE yylval;
