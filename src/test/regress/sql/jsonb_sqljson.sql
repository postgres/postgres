-- JSON_EXISTS

SELECT JSON_EXISTS(NULL::jsonb, '$');

SELECT JSON_EXISTS(jsonb '[]', '$');
SELECT JSON_EXISTS(JSON_OBJECT(RETURNING jsonb), '$');

SELECT JSON_EXISTS(jsonb '1', '$');
SELECT JSON_EXISTS(jsonb 'null', '$');
SELECT JSON_EXISTS(jsonb '[]', '$');

SELECT JSON_EXISTS(jsonb '1', '$.a');
SELECT JSON_EXISTS(jsonb '1', 'strict $.a');
SELECT JSON_EXISTS(jsonb '1', 'strict $.a' ERROR ON ERROR);
SELECT JSON_EXISTS(jsonb 'null', '$.a');
SELECT JSON_EXISTS(jsonb '[]', '$.a');
SELECT JSON_EXISTS(jsonb '[1, "aaa", {"a": 1}]', 'strict $.a');
SELECT JSON_EXISTS(jsonb '[1, "aaa", {"a": 1}]', 'lax $.a');
SELECT JSON_EXISTS(jsonb '{}', '$.a');
SELECT JSON_EXISTS(jsonb '{"b": 1, "a": 2}', '$.a');

SELECT JSON_EXISTS(jsonb '1', '$.a.b');
SELECT JSON_EXISTS(jsonb '{"a": {"b": 1}}', '$.a.b');
SELECT JSON_EXISTS(jsonb '{"a": 1, "b": 2}', '$.a.b');

SELECT JSON_EXISTS(jsonb '{"a": 1, "b": 2}', '$.* ? (@ > $x)' PASSING 1 AS x);
SELECT JSON_EXISTS(jsonb '{"a": 1, "b": 2}', '$.* ? (@ > $x)' PASSING '1' AS x);
SELECT JSON_EXISTS(jsonb '{"a": 1, "b": 2}', '$.* ? (@ > $x && @ < $y)' PASSING 0 AS x, 2 AS y);
SELECT JSON_EXISTS(jsonb '{"a": 1, "b": 2}', '$.* ? (@ > $x && @ < $y)' PASSING 0 AS x, 1 AS y);

-- extension: boolean expressions
SELECT JSON_EXISTS(jsonb '1', '$ > 2');
SELECT JSON_EXISTS(jsonb '1', '$.a > 2' ERROR ON ERROR);

-- extension: RETURNING clause
SELECT JSON_EXISTS(jsonb '1', '$[0]' RETURNING bool);
SELECT JSON_EXISTS(jsonb '1', '$[1]' RETURNING bool);
SELECT JSON_EXISTS(jsonb '1', '$[0]' RETURNING int);
SELECT JSON_EXISTS(jsonb '1', '$[1]' RETURNING int);
SELECT JSON_EXISTS(jsonb '1', '$[0]' RETURNING text);
SELECT JSON_EXISTS(jsonb '1', '$[1]' RETURNING text);
SELECT JSON_EXISTS(jsonb '1', 'strict $[1]' RETURNING text FALSE ON ERROR);
SELECT JSON_EXISTS(jsonb '1', '$[0]' RETURNING jsonb);
SELECT JSON_EXISTS(jsonb '1', '$[0]' RETURNING float4);


-- JSON_VALUE

SELECT JSON_VALUE(NULL::jsonb, '$');

SELECT JSON_VALUE(jsonb 'null', '$');
SELECT JSON_VALUE(jsonb 'null', '$' RETURNING int);

SELECT JSON_VALUE(jsonb 'true', '$');
SELECT JSON_VALUE(jsonb 'true', '$' RETURNING bool);

SELECT JSON_VALUE(jsonb '123', '$');
SELECT JSON_VALUE(jsonb '123', '$' RETURNING int) + 234;
SELECT JSON_VALUE(jsonb '123', '$' RETURNING text);
/* jsonb bytea ??? */
SELECT JSON_VALUE(jsonb '123', '$' RETURNING bytea ERROR ON ERROR);

SELECT JSON_VALUE(jsonb '1.23', '$');
SELECT JSON_VALUE(jsonb '1.23', '$' RETURNING int);
SELECT JSON_VALUE(jsonb '"1.23"', '$' RETURNING numeric);
SELECT JSON_VALUE(jsonb '"1.23"', '$' RETURNING int ERROR ON ERROR);

SELECT JSON_VALUE(jsonb '"aaa"', '$');
SELECT JSON_VALUE(jsonb '"aaa"', '$' RETURNING text);
SELECT JSON_VALUE(jsonb '"aaa"', '$' RETURNING char(5));
SELECT JSON_VALUE(jsonb '"aaa"', '$' RETURNING char(2));
SELECT JSON_VALUE(jsonb '"aaa"', '$' RETURNING json);
SELECT JSON_VALUE(jsonb '"aaa"', '$' RETURNING jsonb);
SELECT JSON_VALUE(jsonb '"aaa"', '$' RETURNING json ERROR ON ERROR);
SELECT JSON_VALUE(jsonb '"aaa"', '$' RETURNING jsonb ERROR ON ERROR);
SELECT JSON_VALUE(jsonb '"\"aaa\""', '$' RETURNING json);
SELECT JSON_VALUE(jsonb '"\"aaa\""', '$' RETURNING jsonb);
SELECT JSON_VALUE(jsonb '"aaa"', '$' RETURNING int);
SELECT JSON_VALUE(jsonb '"aaa"', '$' RETURNING int ERROR ON ERROR);
SELECT JSON_VALUE(jsonb '"aaa"', '$' RETURNING int DEFAULT 111 ON ERROR);
SELECT JSON_VALUE(jsonb '"123"', '$' RETURNING int) + 234;

SELECT JSON_VALUE(jsonb '"2017-02-20"', '$' RETURNING date) + 9;

-- Test NULL checks execution in domain types
CREATE DOMAIN sqljsonb_int_not_null AS int NOT NULL;
SELECT JSON_VALUE(jsonb '1', '$.a' RETURNING sqljsonb_int_not_null);
SELECT JSON_VALUE(jsonb '1', '$.a' RETURNING sqljsonb_int_not_null NULL ON ERROR);
SELECT JSON_VALUE(jsonb '1', '$.a' RETURNING sqljsonb_int_not_null DEFAULT NULL ON ERROR);

SELECT JSON_VALUE(jsonb '[]', '$');
SELECT JSON_VALUE(jsonb '[]', '$' ERROR ON ERROR);
SELECT JSON_VALUE(jsonb '{}', '$');
SELECT JSON_VALUE(jsonb '{}', '$' ERROR ON ERROR);

SELECT JSON_VALUE(jsonb '1', '$.a');
SELECT JSON_VALUE(jsonb '1', 'strict $.a' ERROR ON ERROR);
SELECT JSON_VALUE(jsonb '1', 'strict $.a' DEFAULT 'error' ON ERROR);
SELECT JSON_VALUE(jsonb '1', 'lax $.a' ERROR ON ERROR);
SELECT JSON_VALUE(jsonb '1', 'lax $.a' ERROR ON EMPTY ERROR ON ERROR);
SELECT JSON_VALUE(jsonb '1', 'strict $.a' DEFAULT 2 ON ERROR);
SELECT JSON_VALUE(jsonb '1', 'lax $.a' DEFAULT 2 ON ERROR);
SELECT JSON_VALUE(jsonb '1', 'lax $.a' DEFAULT '2' ON ERROR);
SELECT JSON_VALUE(jsonb '1', 'lax $.a' NULL ON EMPTY DEFAULT '2' ON ERROR);
SELECT JSON_VALUE(jsonb '1', 'lax $.a' DEFAULT '2' ON EMPTY DEFAULT '3' ON ERROR);
SELECT JSON_VALUE(jsonb '1', 'lax $.a' ERROR ON EMPTY DEFAULT '3' ON ERROR);

SELECT JSON_VALUE(jsonb '[1,2]', '$[*]' ERROR ON ERROR);
SELECT JSON_VALUE(jsonb '[1,2]', '$[*]' DEFAULT '0' ON ERROR);
SELECT JSON_VALUE(jsonb '[" "]', '$[*]' RETURNING int ERROR ON ERROR);
SELECT JSON_VALUE(jsonb '[" "]', '$[*]' RETURNING int DEFAULT 2 + 3 ON ERROR);
SELECT JSON_VALUE(jsonb '["1"]', '$[*]' RETURNING int DEFAULT 2 + 3 ON ERROR);

SELECT
	x,
	JSON_VALUE(
		jsonb '{"a": 1, "b": 2}',
		'$.* ? (@ > $x)' PASSING x AS x
		RETURNING int
		DEFAULT -1 ON EMPTY
		DEFAULT -2 ON ERROR
	) y
FROM
	generate_series(0, 2) x;

SELECT JSON_VALUE(jsonb 'null', '$a' PASSING point ' (1, 2 )' AS a);
SELECT JSON_VALUE(jsonb 'null', '$a' PASSING point ' (1, 2 )' AS a RETURNING point);

-- Test timestamptz passing and output
SELECT JSON_VALUE(jsonb 'null', '$ts' PASSING timestamptz '2018-02-21 12:34:56 +10' AS ts);
SELECT JSON_VALUE(jsonb 'null', '$ts' PASSING timestamptz '2018-02-21 12:34:56 +10' AS ts RETURNING timestamptz);
SELECT JSON_VALUE(jsonb 'null', '$ts' PASSING timestamptz '2018-02-21 12:34:56 +10' AS ts RETURNING timestamp);
SELECT JSON_VALUE(jsonb 'null', '$ts' PASSING timestamptz '2018-02-21 12:34:56 +10' AS ts RETURNING json);
SELECT JSON_VALUE(jsonb 'null', '$ts' PASSING timestamptz '2018-02-21 12:34:56 +10' AS ts RETURNING jsonb);

-- JSON_QUERY

SELECT
	JSON_QUERY(js, '$'),
	JSON_QUERY(js, '$' WITHOUT WRAPPER),
	JSON_QUERY(js, '$' WITH CONDITIONAL WRAPPER),
	JSON_QUERY(js, '$' WITH UNCONDITIONAL ARRAY WRAPPER),
	JSON_QUERY(js, '$' WITH ARRAY WRAPPER)
FROM
	(VALUES
		(jsonb 'null'),
		('12.3'),
		('true'),
		('"aaa"'),
		('[1, null, "2"]'),
		('{"a": 1, "b": [2]}')
	) foo(js);

SELECT
	JSON_QUERY(js, 'strict $[*]') AS "unspec",
	JSON_QUERY(js, 'strict $[*]' WITHOUT WRAPPER) AS "without",
	JSON_QUERY(js, 'strict $[*]' WITH CONDITIONAL WRAPPER) AS "with cond",
	JSON_QUERY(js, 'strict $[*]' WITH UNCONDITIONAL ARRAY WRAPPER) AS "with uncond",
	JSON_QUERY(js, 'strict $[*]' WITH ARRAY WRAPPER) AS "with"
FROM
	(VALUES
		(jsonb '1'),
		('[]'),
		('[null]'),
		('[12.3]'),
		('[true]'),
		('["aaa"]'),
		('[[1, 2, 3]]'),
		('[{"a": 1, "b": [2]}]'),
		('[1, "2", null, [3]]')
	) foo(js);

SELECT JSON_QUERY(jsonb '"aaa"', '$' RETURNING text);
SELECT JSON_QUERY(jsonb '"aaa"', '$' RETURNING text KEEP QUOTES);
SELECT JSON_QUERY(jsonb '"aaa"', '$' RETURNING text KEEP QUOTES ON SCALAR STRING);
SELECT JSON_QUERY(jsonb '"aaa"', '$' RETURNING text OMIT QUOTES);
SELECT JSON_QUERY(jsonb '"aaa"', '$' RETURNING text OMIT QUOTES ON SCALAR STRING);
SELECT JSON_QUERY(jsonb '"aaa"', '$' OMIT QUOTES ERROR ON ERROR);
SELECT JSON_QUERY(jsonb '"aaa"', '$' RETURNING json OMIT QUOTES ERROR ON ERROR);
SELECT JSON_QUERY(jsonb '"aaa"', '$' RETURNING bytea FORMAT JSON OMIT QUOTES ERROR ON ERROR);

-- QUOTES behavior should not be specified when WITH WRAPPER used:
-- Should fail
SELECT JSON_QUERY(jsonb '[1]', '$' WITH WRAPPER OMIT QUOTES);
SELECT JSON_QUERY(jsonb '[1]', '$' WITH WRAPPER KEEP QUOTES);
SELECT JSON_QUERY(jsonb '[1]', '$' WITH CONDITIONAL WRAPPER KEEP QUOTES);
SELECT JSON_QUERY(jsonb '[1]', '$' WITH CONDITIONAL WRAPPER OMIT QUOTES);
-- Should succeed
SELECT JSON_QUERY(jsonb '[1]', '$' WITHOUT WRAPPER OMIT QUOTES);
SELECT JSON_QUERY(jsonb '[1]', '$' WITHOUT WRAPPER KEEP QUOTES);

SELECT JSON_QUERY(jsonb '[]', '$[*]');
SELECT JSON_QUERY(jsonb '[]', '$[*]' NULL ON EMPTY);
SELECT JSON_QUERY(jsonb '[]', '$[*]' EMPTY ON EMPTY);
SELECT JSON_QUERY(jsonb '[]', '$[*]' EMPTY ARRAY ON EMPTY);
SELECT JSON_QUERY(jsonb '[]', '$[*]' EMPTY OBJECT ON EMPTY);
SELECT JSON_QUERY(jsonb '[]', '$[*]' ERROR ON EMPTY);
SELECT JSON_QUERY(jsonb '[]', '$[*]' DEFAULT '"empty"' ON EMPTY);

SELECT JSON_QUERY(jsonb '[]', '$[*]' ERROR ON EMPTY NULL ON ERROR);
SELECT JSON_QUERY(jsonb '[]', '$[*]' ERROR ON EMPTY EMPTY ARRAY ON ERROR);
SELECT JSON_QUERY(jsonb '[]', '$[*]' ERROR ON EMPTY EMPTY OBJECT ON ERROR);
SELECT JSON_QUERY(jsonb '[]', '$[*]' ERROR ON EMPTY ERROR ON ERROR);
SELECT JSON_QUERY(jsonb '[]', '$[*]' ERROR ON ERROR);

SELECT JSON_QUERY(jsonb '[1,2]', '$[*]' ERROR ON ERROR);
SELECT JSON_QUERY(jsonb '[1,2]', '$[*]' DEFAULT '"empty"' ON ERROR);

SELECT JSON_QUERY(jsonb '[1,2]', '$' RETURNING json);
SELECT JSON_QUERY(jsonb '[1,2]', '$' RETURNING json FORMAT JSON);
SELECT JSON_QUERY(jsonb '[1,2]', '$' RETURNING jsonb);
SELECT JSON_QUERY(jsonb '[1,2]', '$' RETURNING jsonb FORMAT JSON);
SELECT JSON_QUERY(jsonb '[1,2]', '$' RETURNING text);
SELECT JSON_QUERY(jsonb '[1,2]', '$' RETURNING char(10));
SELECT JSON_QUERY(jsonb '[1,2]', '$' RETURNING char(3));
SELECT JSON_QUERY(jsonb '[1,2]', '$' RETURNING text FORMAT JSON);
SELECT JSON_QUERY(jsonb '[1,2]', '$' RETURNING bytea);
SELECT JSON_QUERY(jsonb '[1,2]', '$' RETURNING bytea FORMAT JSON);

SELECT JSON_QUERY(jsonb '[1,2]', '$[*]' RETURNING bytea EMPTY OBJECT ON ERROR);
SELECT JSON_QUERY(jsonb '[1,2]', '$[*]' RETURNING bytea FORMAT JSON EMPTY OBJECT ON ERROR);
SELECT JSON_QUERY(jsonb '[1,2]', '$[*]' RETURNING json EMPTY OBJECT ON ERROR);
SELECT JSON_QUERY(jsonb '[1,2]', '$[*]' RETURNING jsonb EMPTY OBJECT ON ERROR);

SELECT
	x, y,
	JSON_QUERY(
		jsonb '[1,2,3,4,5,null]',
		'$[*] ? (@ >= $x && @ <= $y)'
		PASSING x AS x, y AS y
		WITH CONDITIONAL WRAPPER
		EMPTY ARRAY ON EMPTY
	) list
FROM
	generate_series(0, 4) x,
	generate_series(0, 4) y;

-- Extension: record types returning
CREATE TYPE sqljsonb_rec AS (a int, t text, js json, jb jsonb, jsa json[]);
CREATE TYPE sqljsonb_reca AS (reca sqljsonb_rec[]);

SELECT JSON_QUERY(jsonb '[{"a": 1, "b": "foo", "t": "aaa", "js": [1, "2", {}], "jb": {"x": [1, "2", {}]}},  {"a": 2}]', '$[0]' RETURNING sqljsonb_rec);
SELECT * FROM unnest((JSON_QUERY(jsonb '{"jsa":  [{"a": 1, "b": ["foo"]}, {"a": 2, "c": {}}, 123]}', '$' RETURNING sqljsonb_rec)).jsa);
SELECT * FROM unnest((JSON_QUERY(jsonb '{"reca": [{"a": 1, "t": ["foo", []]}, {"a": 2, "jb": [{}, true]}]}', '$' RETURNING sqljsonb_reca)).reca);

-- Extension: array types returning
SELECT JSON_QUERY(jsonb '[1,2,null,"3"]', '$[*]' RETURNING int[] WITH WRAPPER);
SELECT * FROM unnest(JSON_QUERY(jsonb '[{"a": 1, "t": ["foo", []]}, {"a": 2, "jb": [{}, true]}]', '$' RETURNING sqljsonb_rec[]));

-- Extension: domain types returning
SELECT JSON_QUERY(jsonb '{"a": 1}', '$.a' RETURNING sqljsonb_int_not_null);
SELECT JSON_QUERY(jsonb '{"a": 1}', '$.b' RETURNING sqljsonb_int_not_null);

-- Test timestamptz passing and output
SELECT JSON_QUERY(jsonb 'null', '$ts' PASSING timestamptz '2018-02-21 12:34:56 +10' AS ts);
SELECT JSON_QUERY(jsonb 'null', '$ts' PASSING timestamptz '2018-02-21 12:34:56 +10' AS ts RETURNING json);
SELECT JSON_QUERY(jsonb 'null', '$ts' PASSING timestamptz '2018-02-21 12:34:56 +10' AS ts RETURNING jsonb);

-- Test constraints

CREATE TABLE test_jsonb_constraints (
	js text,
	i int,
	x jsonb DEFAULT JSON_QUERY(jsonb '[1,2]', '$[*]' WITH WRAPPER)
	CONSTRAINT test_jsonb_constraint1
		CHECK (js IS JSON)
	CONSTRAINT test_jsonb_constraint2
		CHECK (JSON_EXISTS(js::jsonb, '$.a' PASSING i + 5 AS int, i::text AS txt, array[1,2,3] as arr))
	CONSTRAINT test_jsonb_constraint3
		CHECK (JSON_VALUE(js::jsonb, '$.a' RETURNING int DEFAULT ('12' || i)::int ON EMPTY ERROR ON ERROR) > i)
	CONSTRAINT test_jsonb_constraint4
		CHECK (JSON_QUERY(js::jsonb, '$.a' WITH CONDITIONAL WRAPPER EMPTY OBJECT ON ERROR) < jsonb '[10]')
	CONSTRAINT test_jsonb_constraint5
		CHECK (JSON_QUERY(js::jsonb, '$.a' RETURNING char(5) OMIT QUOTES EMPTY ARRAY ON EMPTY) >  'a' COLLATE "C")
	CONSTRAINT test_jsonb_constraint6
		CHECK (JSON_EXISTS(js::jsonb, 'strict $.a' RETURNING int TRUE ON ERROR) < 2)
);

\d test_jsonb_constraints

SELECT check_clause
FROM information_schema.check_constraints
WHERE constraint_name LIKE 'test_jsonb_constraint%'
ORDER BY 1;

SELECT pg_get_expr(adbin, adrelid)
FROM pg_attrdef
WHERE adrelid = 'test_jsonb_constraints'::regclass
ORDER BY 1;

INSERT INTO test_jsonb_constraints VALUES ('', 1);
INSERT INTO test_jsonb_constraints VALUES ('1', 1);
INSERT INTO test_jsonb_constraints VALUES ('[]');
INSERT INTO test_jsonb_constraints VALUES ('{"b": 1}', 1);
INSERT INTO test_jsonb_constraints VALUES ('{"a": 1}', 1);
INSERT INTO test_jsonb_constraints VALUES ('{"a": 7}', 1);
INSERT INTO test_jsonb_constraints VALUES ('{"a": 10}', 1);

DROP TABLE test_jsonb_constraints;

-- Test mutabilily od query functions
CREATE TABLE test_jsonb_mutability(js jsonb);
CREATE INDEX ON test_jsonb_mutability (JSON_QUERY(js, '$'));
CREATE INDEX ON test_jsonb_mutability (JSON_QUERY(js, '$.a[0]'));
CREATE INDEX ON test_jsonb_mutability (JSON_QUERY(js, '$.datetime()'));
CREATE INDEX ON test_jsonb_mutability (JSON_QUERY(js, '$.a ? (@ < $.datetime())'));
CREATE INDEX ON test_jsonb_mutability (JSON_QUERY(js, '$.a ? (@.datetime() < $.datetime())'));
CREATE INDEX ON test_jsonb_mutability (JSON_QUERY(js, '$.a ? (@.datetime() < $.datetime("HH:MI TZH"))'));
CREATE INDEX ON test_jsonb_mutability (JSON_QUERY(js, '$.a ? (@.datetime("HH:MI TZH") < $.datetime("HH:MI TZH"))'));
CREATE INDEX ON test_jsonb_mutability (JSON_QUERY(js, '$.a ? (@.datetime("HH:MI") < $.datetime("YY-MM-DD HH:MI"))'));
CREATE INDEX ON test_jsonb_mutability (JSON_QUERY(js, '$.a ? (@.datetime("HH:MI TZH") < $.datetime("YY-MM-DD HH:MI"))'));
CREATE INDEX ON test_jsonb_mutability (JSON_QUERY(js, '$.datetime("HH:MI TZH") < $x' PASSING '12:34'::timetz AS x));
CREATE INDEX ON test_jsonb_mutability (JSON_QUERY(js, '$.datetime("HH:MI TZH") < $y' PASSING '12:34'::timetz AS x));
CREATE INDEX ON test_jsonb_mutability (JSON_QUERY(js, '$.datetime() < $x' PASSING '12:34'::timetz AS x));
CREATE INDEX ON test_jsonb_mutability (JSON_QUERY(js, '$.datetime() < $x' PASSING '1234'::int AS x));
CREATE INDEX ON test_jsonb_mutability (JSON_QUERY(js, '$.datetime() ? (@ == $x)' PASSING '12:34'::time AS x));
CREATE INDEX ON test_jsonb_mutability (JSON_QUERY(js, '$.datetime("YY-MM-DD") ? (@ == $x)' PASSING '2020-07-14'::date AS x));
CREATE INDEX ON test_jsonb_mutability (JSON_QUERY(js, '$[1, $.a ? (@.datetime() == $x)]' PASSING '12:34'::time AS x));
CREATE INDEX ON test_jsonb_mutability (JSON_QUERY(js, '$[1, 0 to $.a ? (@.datetime() == $x)]' PASSING '12:34'::time AS x));
CREATE INDEX ON test_jsonb_mutability (JSON_QUERY(js, '$[1, $.a ? (@.datetime("HH:MI") == $x)]' PASSING '12:34'::time AS x));
DROP TABLE test_jsonb_mutability;

-- JSON_TABLE

-- Should fail (JSON_TABLE can be used only in FROM clause)
SELECT JSON_TABLE('[]', '$');

-- Should fail (no columns)
SELECT * FROM JSON_TABLE(NULL, '$' COLUMNS ());

-- NULL => empty table
SELECT * FROM JSON_TABLE(NULL::jsonb, '$' COLUMNS (foo int)) bar;

--
SELECT * FROM JSON_TABLE(jsonb '123', '$'
	COLUMNS (item int PATH '$', foo int)) bar;

-- JSON_TABLE: basic functionality
CREATE DOMAIN jsonb_test_domain AS text CHECK (value <> 'foo');

SELECT *
FROM
	(VALUES
		('1'),
		('[]'),
		('{}'),
		('[1, 1.23, "2", "aaaaaaa", "foo", null, false, true, {"aaa": 123}, "[1,2]", "\"str\""]')
	) vals(js)
	LEFT OUTER JOIN
-- JSON_TABLE is implicitly lateral
	JSON_TABLE(
		vals.js::jsonb, 'lax $[*]'
		COLUMNS (
			id FOR ORDINALITY,
			id2 FOR ORDINALITY, -- allowed additional ordinality columns
			"int" int PATH '$',
			"text" text PATH '$',
			"char(4)" char(4) PATH '$',
			"bool" bool PATH '$',
			"numeric" numeric PATH '$',
			"domain" jsonb_test_domain PATH '$',
			js json PATH '$',
			jb jsonb PATH '$',
			jst text    FORMAT JSON  PATH '$',
			jsc char(4) FORMAT JSON  PATH '$',
			jsv varchar(4) FORMAT JSON  PATH '$',
			jsb jsonb FORMAT JSON PATH '$',
			jsbq jsonb FORMAT JSON PATH '$' OMIT QUOTES,
			aaa int, -- implicit path '$."aaa"',
			aaa1 int PATH '$.aaa',
			exists1 bool EXISTS PATH '$.aaa',
			exists2 int EXISTS PATH '$.aaa',
			exists3 int EXISTS PATH 'strict $.aaa' UNKNOWN ON ERROR,
			exists4 text EXISTS PATH 'strict $.aaa' FALSE ON ERROR,

			js2 json PATH '$',
			jsb2w jsonb PATH '$' WITH WRAPPER,
			jsb2q jsonb PATH '$' OMIT QUOTES,
			ia int[] PATH '$',
			ta text[] PATH '$',
			jba jsonb[] PATH '$'
		)
	) jt
	ON true;

-- JSON_TABLE: Test backward parsing

CREATE VIEW jsonb_table_view AS
SELECT * FROM
	JSON_TABLE(
		jsonb 'null', 'lax $[*]' PASSING 1 + 2 AS a, json '"foo"' AS "b c"
		COLUMNS (
			id FOR ORDINALITY,
			id2 FOR ORDINALITY, -- allowed additional ordinality columns
			"int" int PATH '$',
			"text" text PATH '$',
			"char(4)" char(4) PATH '$',
			"bool" bool PATH '$',
			"numeric" numeric PATH '$',
			"domain" jsonb_test_domain PATH '$',
			js json PATH '$',
			jb jsonb PATH '$',
			jst text    FORMAT JSON  PATH '$',
			jsc char(4) FORMAT JSON  PATH '$',
			jsv varchar(4) FORMAT JSON  PATH '$',
			jsb jsonb   FORMAT JSON PATH '$',
			jsbq jsonb FORMAT JSON PATH '$' OMIT QUOTES,
			aaa int, -- implicit path '$."aaa"',
			aaa1 int PATH '$.aaa',
			exists1 bool EXISTS PATH '$.aaa',
			exists2 int EXISTS PATH '$.aaa' TRUE ON ERROR,
			exists3 text EXISTS PATH 'strict $.aaa' UNKNOWN ON ERROR,

			js2 json PATH '$',
			jsb2w jsonb PATH '$' WITH WRAPPER,
			jsb2q jsonb PATH '$' OMIT QUOTES,
			ia int[] PATH '$',
			ta text[] PATH '$',
			jba jsonb[] PATH '$',

			NESTED PATH '$[1]' AS p1 COLUMNS (
				a1 int,
				NESTED PATH '$[*]' AS "p1 1" COLUMNS (
					a11 text
				),
				b1 text
			),
			NESTED PATH '$[2]' AS p2 COLUMNS (
				NESTED PATH '$[*]' AS "p2:1" COLUMNS (
					a21 text
				),
				NESTED PATH '$[*]' AS p22 COLUMNS (
					a22 text
				)
			)
		)
	);

\sv jsonb_table_view

EXPLAIN (COSTS OFF, VERBOSE) SELECT * FROM jsonb_table_view;

DROP VIEW jsonb_table_view;
DROP DOMAIN jsonb_test_domain;

-- JSON_TABLE: ON EMPTY/ON ERROR behavior
SELECT *
FROM
	(VALUES ('1'), ('"err"')) vals(js),
	JSON_TABLE(vals.js::jsonb, '$' COLUMNS (a int PATH '$')) jt;

SELECT *
FROM
	(VALUES ('1'), ('"err"')) vals(js)
		LEFT OUTER JOIN
	JSON_TABLE(vals.js::jsonb, '$' COLUMNS (a int PATH '$') ERROR ON ERROR) jt
		ON true;

SELECT *
FROM
	(VALUES ('1'), ('"err"')) vals(js)
		LEFT OUTER JOIN
	JSON_TABLE(vals.js::jsonb, '$' COLUMNS (a int PATH '$' ERROR ON ERROR)) jt
		ON true;

SELECT * FROM JSON_TABLE(jsonb '1', '$' COLUMNS (a int PATH '$.a' ERROR ON EMPTY)) jt;
SELECT * FROM JSON_TABLE(jsonb '1', '$' COLUMNS (a int PATH 'strict $.a' ERROR ON EMPTY) ERROR ON ERROR) jt;
SELECT * FROM JSON_TABLE(jsonb '1', '$' COLUMNS (a int PATH 'lax $.a' ERROR ON EMPTY) ERROR ON ERROR) jt;

SELECT * FROM JSON_TABLE(jsonb '"a"', '$' COLUMNS (a int PATH '$'   DEFAULT 1 ON EMPTY DEFAULT 2 ON ERROR)) jt;
SELECT * FROM JSON_TABLE(jsonb '"a"', '$' COLUMNS (a int PATH 'strict $.a' DEFAULT 1 ON EMPTY DEFAULT 2 ON ERROR)) jt;
SELECT * FROM JSON_TABLE(jsonb '"a"', '$' COLUMNS (a int PATH 'lax $.a' DEFAULT 1 ON EMPTY DEFAULT 2 ON ERROR)) jt;

-- JSON_TABLE: EXISTS PATH types
SELECT * FROM JSON_TABLE(jsonb '"a"', '$' COLUMNS (a int4 EXISTS PATH '$.a'));
SELECT * FROM JSON_TABLE(jsonb '"a"', '$' COLUMNS (a int2 EXISTS PATH '$.a'));
SELECT * FROM JSON_TABLE(jsonb '"a"', '$' COLUMNS (a int8 EXISTS PATH '$.a'));
SELECT * FROM JSON_TABLE(jsonb '"a"', '$' COLUMNS (a float4 EXISTS PATH '$.a'));
SELECT * FROM JSON_TABLE(jsonb '"a"', '$' COLUMNS (a char(3) EXISTS PATH '$.a'));
SELECT * FROM JSON_TABLE(jsonb '"a"', '$' COLUMNS (a json EXISTS PATH '$.a'));
SELECT * FROM JSON_TABLE(jsonb '"a"', '$' COLUMNS (a jsonb EXISTS PATH '$.a'));

-- JSON_TABLE: nested paths and plans

-- Should fail (JSON_TABLE columns must contain explicit AS path
-- specifications if explicit PLAN clause is used)
SELECT * FROM JSON_TABLE(
	jsonb '[]', '$' -- AS <path name> required here
	COLUMNS (
		foo int PATH '$'
	)
	PLAN DEFAULT (UNION)
) jt;

SELECT * FROM JSON_TABLE(
	jsonb '[]', '$' AS path1
	COLUMNS (
		NESTED PATH '$' COLUMNS ( -- AS <path name> required here
			foo int PATH '$'
		)
	)
	PLAN DEFAULT (UNION)
) jt;

-- Should fail (column names must be distinct)
SELECT * FROM JSON_TABLE(
	jsonb '[]', '$' AS a
	COLUMNS (
		a int
	)
) jt;

SELECT * FROM JSON_TABLE(
	jsonb '[]', '$' AS a
	COLUMNS (
		b int,
		NESTED PATH '$' AS a
		COLUMNS (
			c int
		)
	)
) jt;

SELECT * FROM JSON_TABLE(
	jsonb '[]', '$'
	COLUMNS (
		b int,
		NESTED PATH '$' AS b
		COLUMNS (
			c int
		)
	)
) jt;

SELECT * FROM JSON_TABLE(
	jsonb '[]', '$'
	COLUMNS (
		NESTED PATH '$' AS a
		COLUMNS (
			b int
		),
		NESTED PATH '$'
		COLUMNS (
			NESTED PATH '$' AS a
			COLUMNS (
				c int
			)
		)
	)
) jt;

-- JSON_TABLE: plan validation

SELECT * FROM JSON_TABLE(
	jsonb 'null', '$[*]' AS p0
	COLUMNS (
		NESTED PATH '$' AS p1 COLUMNS (
			NESTED PATH '$' AS p11 COLUMNS ( foo int ),
			NESTED PATH '$' AS p12 COLUMNS ( bar int )
		),
		NESTED PATH '$' AS p2 COLUMNS (
			NESTED PATH '$' AS p21 COLUMNS ( baz int )
		)
	)
	PLAN (p1)
) jt;

SELECT * FROM JSON_TABLE(
	jsonb 'null', '$[*]' AS p0
	COLUMNS (
		NESTED PATH '$' AS p1 COLUMNS (
			NESTED PATH '$' AS p11 COLUMNS ( foo int ),
			NESTED PATH '$' AS p12 COLUMNS ( bar int )
		),
		NESTED PATH '$' AS p2 COLUMNS (
			NESTED PATH '$' AS p21 COLUMNS ( baz int )
		)
	)
	PLAN (p0)
) jt;

SELECT * FROM JSON_TABLE(
	jsonb 'null', '$[*]' AS p0
	COLUMNS (
		NESTED PATH '$' AS p1 COLUMNS (
			NESTED PATH '$' AS p11 COLUMNS ( foo int ),
			NESTED PATH '$' AS p12 COLUMNS ( bar int )
		),
		NESTED PATH '$' AS p2 COLUMNS (
			NESTED PATH '$' AS p21 COLUMNS ( baz int )
		)
	)
	PLAN (p0 OUTER p3)
) jt;

SELECT * FROM JSON_TABLE(
	jsonb 'null', '$[*]' AS p0
	COLUMNS (
		NESTED PATH '$' AS p1 COLUMNS (
			NESTED PATH '$' AS p11 COLUMNS ( foo int ),
			NESTED PATH '$' AS p12 COLUMNS ( bar int )
		),
		NESTED PATH '$' AS p2 COLUMNS (
			NESTED PATH '$' AS p21 COLUMNS ( baz int )
		)
	)
	PLAN (p0 UNION p1 UNION p11)
) jt;

SELECT * FROM JSON_TABLE(
	jsonb 'null', '$[*]' AS p0
	COLUMNS (
		NESTED PATH '$' AS p1 COLUMNS (
			NESTED PATH '$' AS p11 COLUMNS ( foo int ),
			NESTED PATH '$' AS p12 COLUMNS ( bar int )
		),
		NESTED PATH '$' AS p2 COLUMNS (
			NESTED PATH '$' AS p21 COLUMNS ( baz int )
		)
	)
	PLAN (p0 OUTER (p1 CROSS p13))
) jt;

SELECT * FROM JSON_TABLE(
	jsonb 'null', '$[*]' AS p0
	COLUMNS (
		NESTED PATH '$' AS p1 COLUMNS (
			NESTED PATH '$' AS p11 COLUMNS ( foo int ),
			NESTED PATH '$' AS p12 COLUMNS ( bar int )
		),
		NESTED PATH '$' AS p2 COLUMNS (
			NESTED PATH '$' AS p21 COLUMNS ( baz int )
		)
	)
	PLAN (p0 OUTER (p1 CROSS p2))
) jt;

SELECT * FROM JSON_TABLE(
	jsonb 'null', '$[*]' AS p0
	COLUMNS (
		NESTED PATH '$' AS p1 COLUMNS (
			NESTED PATH '$' AS p11 COLUMNS ( foo int ),
			NESTED PATH '$' AS p12 COLUMNS ( bar int )
		),
		NESTED PATH '$' AS p2 COLUMNS (
			NESTED PATH '$' AS p21 COLUMNS ( baz int )
		)
	)
	PLAN (p0 OUTER ((p1 UNION p11) CROSS p2))
) jt;

SELECT * FROM JSON_TABLE(
	jsonb 'null', '$[*]' AS p0
	COLUMNS (
		NESTED PATH '$' AS p1 COLUMNS (
			NESTED PATH '$' AS p11 COLUMNS ( foo int ),
			NESTED PATH '$' AS p12 COLUMNS ( bar int )
		),
		NESTED PATH '$' AS p2 COLUMNS (
			NESTED PATH '$' AS p21 COLUMNS ( baz int )
		)
	)
	PLAN (p0 OUTER ((p1 INNER p11) CROSS p2))
) jt;

SELECT * FROM JSON_TABLE(
	jsonb 'null', '$[*]' AS p0
	COLUMNS (
		NESTED PATH '$' AS p1 COLUMNS (
			NESTED PATH '$' AS p11 COLUMNS ( foo int ),
			NESTED PATH '$' AS p12 COLUMNS ( bar int )
		),
		NESTED PATH '$' AS p2 COLUMNS (
			NESTED PATH '$' AS p21 COLUMNS ( baz int )
		)
	)
	PLAN (p0 OUTER ((p1 INNER (p12 CROSS p11)) CROSS p2))
) jt;

SELECT * FROM JSON_TABLE(
	jsonb 'null', 'strict $[*]' AS p0
	COLUMNS (
		NESTED PATH '$' AS p1 COLUMNS (
			NESTED PATH '$' AS p11 COLUMNS ( foo int ),
			NESTED PATH '$' AS p12 COLUMNS ( bar int )
		),
		NESTED PATH '$' AS p2 COLUMNS (
			NESTED PATH '$' AS p21 COLUMNS ( baz int )
		)
	)
	PLAN (p0 OUTER ((p1 INNER (p12 CROSS p11)) CROSS (p2 INNER p21)))
) jt;

SELECT * FROM JSON_TABLE(
	jsonb 'null', 'strict $[*]' -- without root path name
	COLUMNS (
		NESTED PATH '$' AS p1 COLUMNS (
			NESTED PATH '$' AS p11 COLUMNS ( foo int ),
			NESTED PATH '$' AS p12 COLUMNS ( bar int )
		),
		NESTED PATH '$' AS p2 COLUMNS (
			NESTED PATH '$' AS p21 COLUMNS ( baz int )
		)
	)
	PLAN ((p1 INNER (p12 CROSS p11)) CROSS (p2 INNER p21))
) jt;

-- JSON_TABLE: plan execution

CREATE TEMP TABLE jsonb_table_test (js jsonb);

INSERT INTO jsonb_table_test
VALUES (
	'[
		{"a":  1,  "b": [], "c": []},
		{"a":  2,  "b": [1, 2, 3], "c": [10, null, 20]},
		{"a":  3,  "b": [1, 2], "c": []},
		{"x": "4", "b": [1, 2], "c": 123}
	 ]'
);

-- unspecified plan (outer, union)
select
	jt.*
from
	jsonb_table_test jtt,
	json_table (
		jtt.js,'strict $[*]' as p
		columns (
			n for ordinality,
			a int path 'lax $.a' default -1 on empty,
			nested path 'strict $.b[*]' as pb columns ( b int path '$' ),
			nested path 'strict $.c[*]' as pc columns ( c int path '$' )
		)
	) jt;

-- default plan (outer, union)
select
	jt.*
from
	jsonb_table_test jtt,
	json_table (
		jtt.js,'strict $[*]' as p
		columns (
			n for ordinality,
			a int path 'lax $.a' default -1 on empty,
			nested path 'strict $.b[*]' as pb columns ( b int path '$' ),
			nested path 'strict $.c[*]' as pc columns ( c int path '$' )
		)
		plan default (outer, union)
	) jt;

-- specific plan (p outer (pb union pc))
select
	jt.*
from
	jsonb_table_test jtt,
	json_table (
		jtt.js,'strict $[*]' as p
		columns (
			n for ordinality,
			a int path 'lax $.a' default -1 on empty,
			nested path 'strict $.b[*]' as pb columns ( b int path '$' ),
			nested path 'strict $.c[*]' as pc columns ( c int path '$' )
		)
		plan (p outer (pb union pc))
	) jt;

-- specific plan (p outer (pc union pb))
select
	jt.*
from
	jsonb_table_test jtt,
	json_table (
		jtt.js,'strict $[*]' as p
		columns (
			n for ordinality,
			a int path 'lax $.a' default -1 on empty,
			nested path 'strict $.b[*]' as pb columns ( b int path '$' ),
			nested path 'strict $.c[*]' as pc columns ( c int path '$' )
		)
		plan (p outer (pc union pb))
	) jt;

-- default plan (inner, union)
select
	jt.*
from
	jsonb_table_test jtt,
	json_table (
		jtt.js,'strict $[*]' as p
		columns (
			n for ordinality,
			a int path 'lax $.a' default -1 on empty,
			nested path 'strict $.b[*]' as pb columns ( b int path '$' ),
			nested path 'strict $.c[*]' as pc columns ( c int path '$' )
		)
		plan default (inner)
	) jt;

-- specific plan (p inner (pb union pc))
select
	jt.*
from
	jsonb_table_test jtt,
	json_table (
		jtt.js,'strict $[*]' as p
		columns (
			n for ordinality,
			a int path 'lax $.a' default -1 on empty,
			nested path 'strict $.b[*]' as pb columns ( b int path '$' ),
			nested path 'strict $.c[*]' as pc columns ( c int path '$' )
		)
		plan (p inner (pb union pc))
	) jt;

-- default plan (inner, cross)
select
	jt.*
from
	jsonb_table_test jtt,
	json_table (
		jtt.js,'strict $[*]' as p
		columns (
			n for ordinality,
			a int path 'lax $.a' default -1 on empty,
			nested path 'strict $.b[*]' as pb columns ( b int path '$' ),
			nested path 'strict $.c[*]' as pc columns ( c int path '$' )
		)
		plan default (cross, inner)
	) jt;

-- specific plan (p inner (pb cross pc))
select
	jt.*
from
	jsonb_table_test jtt,
	json_table (
		jtt.js,'strict $[*]' as p
		columns (
			n for ordinality,
			a int path 'lax $.a' default -1 on empty,
			nested path 'strict $.b[*]' as pb columns ( b int path '$' ),
			nested path 'strict $.c[*]' as pc columns ( c int path '$' )
		)
		plan (p inner (pb cross pc))
	) jt;

-- default plan (outer, cross)
select
	jt.*
from
	jsonb_table_test jtt,
	json_table (
		jtt.js,'strict $[*]' as p
		columns (
			n for ordinality,
			a int path 'lax $.a' default -1 on empty,
			nested path 'strict $.b[*]' as pb columns ( b int path '$' ),
			nested path 'strict $.c[*]' as pc columns ( c int path '$' )
		)
		plan default (outer, cross)
	) jt;

-- specific plan (p outer (pb cross pc))
select
	jt.*
from
	jsonb_table_test jtt,
	json_table (
		jtt.js,'strict $[*]' as p
		columns (
			n for ordinality,
			a int path 'lax $.a' default -1 on empty,
			nested path 'strict $.b[*]' as pb columns ( b int path '$' ),
			nested path 'strict $.c[*]' as pc columns ( c int path '$' )
		)
		plan (p outer (pb cross pc))
	) jt;


select
	jt.*, b1 + 100 as b
from
	json_table (jsonb
		'[
			{"a":  1,  "b": [[1, 10], [2], [3, 30, 300]], "c": [1, null, 2]},
			{"a":  2,  "b": [10, 20], "c": [1, null, 2]},
			{"x": "3", "b": [11, 22, 33, 44]}
		 ]',
		'$[*]' as p
		columns (
			n for ordinality,
			a int path 'lax $.a' default -1 on error,
			nested path 'strict $.b[*]' as pb columns (
				b text format json path '$',
				nested path 'strict $[*]' as pb1 columns (
					b1 int path '$'
				)
			),
			nested path 'strict $.c[*]' as pc columns (
				c text format json path '$',
				nested path 'strict $[*]' as pc1 columns (
					c1 int path '$'
				)
			)
		)
		--plan default(outer, cross)
		plan(p outer ((pb inner pb1) cross (pc outer pc1)))
	) jt;

-- Should succeed (JSON arguments are passed to root and nested paths)
SELECT *
FROM
	generate_series(1, 4) x,
	generate_series(1, 3) y,
	JSON_TABLE(jsonb
		'[[1,2,3],[2,3,4,5],[3,4,5,6]]',
		'strict $[*] ? (@[*] < $x)'
		PASSING x AS x, y AS y
		COLUMNS (
			y text FORMAT JSON PATH '$',
			NESTED PATH 'strict $[*] ? (@ >= $y)'
			COLUMNS (
				z int PATH '$'
			)
		)
	) jt;

-- Should fail (JSON arguments are not passed to column paths)
SELECT *
FROM JSON_TABLE(
	jsonb '[1,2,3]',
	'$[*] ? (@ < $x)'
		PASSING 10 AS x
		COLUMNS (y text FORMAT JSON PATH '$ ? (@ < $x)')
	) jt;

-- Extension: non-constant JSON path
SELECT JSON_EXISTS(jsonb '{"a": 123}', '$' || '.' || 'a');
SELECT JSON_VALUE(jsonb '{"a": 123}', '$' || '.' || 'a');
SELECT JSON_VALUE(jsonb '{"a": 123}', '$' || '.' || 'b' DEFAULT 'foo' ON EMPTY);
SELECT JSON_QUERY(jsonb '{"a": 123}', '$' || '.' || 'a');
SELECT JSON_QUERY(jsonb '{"a": 123}', '$' || '.' || 'a' WITH WRAPPER);
-- Should fail (invalid path)
SELECT JSON_QUERY(jsonb '{"a": 123}', 'error' || ' ' || 'error');
-- Should fail (not supported)
SELECT * FROM JSON_TABLE(jsonb '{"a": 123}', '$' || '.' || 'a' COLUMNS (foo int));

-- Test parallel JSON_VALUE()


CREATE UNLOGGED TABLE test_parallel_jsonb_value AS
SELECT i::text::jsonb AS js
FROM generate_series(1, 50000) i;


-- encourage use of parallel plans
set parallel_setup_cost=0;
set parallel_tuple_cost=0;
set min_parallel_table_scan_size=0;
set max_parallel_workers_per_gather=4;
set parallel_leader_participation = off;

-- Should be non-parallel due to subtransactions
EXPLAIN (COSTS OFF)
SELECT sum(JSON_VALUE(js, '$' RETURNING numeric)) FROM test_parallel_jsonb_value;
SELECT sum(JSON_VALUE(js, '$' RETURNING numeric)) FROM test_parallel_jsonb_value;

-- Should be parallel
EXPLAIN (COSTS OFF)
SELECT sum(JSON_VALUE(js, '$' RETURNING numeric ERROR ON ERROR)) FROM test_parallel_jsonb_value;
SELECT sum(JSON_VALUE(js, '$' RETURNING numeric ERROR ON ERROR)) FROM test_parallel_jsonb_value;

DROP TABLE test_parallel_jsonb_value;
