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
#define	TO	358
#define	TRAILING	359
#define	TRANSACTION	360
#define	TRIM	361
#define	UNION	362
#define	UNIQUE	363
#define	UPDATE	364
#define	USING	365
#define	VALUES	366
#define	VARCHAR	367
#define	VARYING	368
#define	VIEW	369
#define	WHERE	370
#define	WITH	371
#define	WORK	372
#define	YEAR_P	373
#define	ZONE	374
#define	FALSE_P	375
#define	TRIGGER	376
#define	TRUE_P	377
#define	TYPE_P	378
#define	ABORT_TRANS	379
#define	AFTER	380
#define	AGGREGATE	381
#define	ANALYZE	382
#define	BACKWARD	383
#define	BEFORE	384
#define	BINARY	385
#define	CACHE	386
#define	CLUSTER	387
#define	COPY	388
#define	CYCLE	389
#define	DATABASE	390
#define	DELIMITERS	391
#define	DO	392
#define	EACH	393
#define	EXPLAIN	394
#define	EXTEND	395
#define	FORWARD	396
#define	FUNCTION	397
#define	HANDLER	398
#define	INCREMENT	399
#define	INDEX	400
#define	INHERITS	401
#define	INSTEAD	402
#define	ISNULL	403
#define	LANCOMPILER	404
#define	LISTEN	405
#define	LOAD	406
#define	LOCK_P	407
#define	LOCATION	408
#define	MAXVALUE	409
#define	MINVALUE	410
#define	MOVE	411
#define	NEW	412
#define	NONE	413
#define	NOTHING	414
#define	NOTNULL	415
#define	OIDS	416
#define	OPERATOR	417
#define	PROCEDURAL	418
#define	RECIPE	419
#define	RENAME	420
#define	RESET	421
#define	RETURNS	422
#define	ROW	423
#define	RULE	424
#define	SEQUENCE	425
#define	SETOF	426
#define	SHOW	427
#define	START	428
#define	STATEMENT	429
#define	STDIN	430
#define	STDOUT	431
#define	TRUSTED	432
#define	VACUUM	433
#define	VERBOSE	434
#define	VERSION	435
#define	ARCHIVE	436
#define	USER	437
#define	PASSWORD	438
#define	CREATEDB	439
#define	NOCREATEDB	440
#define	CREATEUSER	441
#define	NOCREATEUSER	442
#define	VALID	443
#define	UNTIL	444
#define	IDENT	445
#define	SCONST	446
#define	Op	447
#define	ICONST	448
#define	PARAM	449
#define	FCONST	450
#define	OP	451
#define	UMINUS	452
#define	TYPECAST	453
#define	REDUCE	454


extern YYSTYPE yylval;
