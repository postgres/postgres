-- JSON_TABLE

-- Should fail (JSON_TABLE can be used only in FROM clause)
SELECT JSON_TABLE('[]', '$');

-- Only allow EMPTY and ERROR for ON ERROR
SELECT * FROM JSON_TABLE('[]', 'strict $.a' COLUMNS (js2 int PATH '$') DEFAULT 1 ON ERROR);
SELECT * FROM JSON_TABLE('[]', 'strict $.a' COLUMNS (js2 int PATH '$') NULL ON ERROR);
SELECT * FROM JSON_TABLE('[]', 'strict $.a' COLUMNS (js2 int PATH '$') EMPTY ON ERROR);
SELECT * FROM JSON_TABLE('[]', 'strict $.a' COLUMNS (js2 int PATH '$') ERROR ON ERROR);

-- Column and path names must be distinct
SELECT * FROM JSON_TABLE(jsonb'"1.23"', '$.a' as js2 COLUMNS (js2 int path '$'));

-- Should fail (no columns)
SELECT * FROM JSON_TABLE(NULL, '$' COLUMNS ());

SELECT * FROM JSON_TABLE (NULL::jsonb, '$' COLUMNS (v1 timestamp)) AS f (v1, v2);

--duplicated column name
SELECT * FROM JSON_TABLE(jsonb'"1.23"', '$.a' COLUMNS (js2 int path '$', js2 int path '$'));

--return composite data type.
create type comp as (a int, b int);
SELECT * FROM JSON_TABLE(jsonb '{"rec": "(1,2)"}', '$' COLUMNS (id FOR ORDINALITY, comp comp path '$.rec' omit quotes)) jt;
drop type comp;

-- NULL => empty table
SELECT * FROM JSON_TABLE(NULL::jsonb, '$' COLUMNS (foo int)) bar;
SELECT * FROM JSON_TABLE(jsonb'"1.23"', 'strict $.a' COLUMNS (js2 int PATH '$'));

--
SELECT * FROM JSON_TABLE(jsonb '123', '$'
	COLUMNS (item int PATH '$', foo int)) bar;

-- JSON_TABLE: basic functionality
CREATE DOMAIN jsonb_test_domain AS text CHECK (value <> 'foo');
CREATE TEMP TABLE json_table_test (js) AS
	(VALUES
		('1'),
		('[]'),
		('{}'),
		('[1, 1.23, "2", "aaaaaaa", "foo", null, false, true, {"aaa": 123}, "[1,2]", "\"str\""]')
	);

-- Regular "unformatted" columns
SELECT *
FROM json_table_test vals
	LEFT OUTER JOIN
	JSON_TABLE(
		vals.js::jsonb, 'lax $[*]'
		COLUMNS (
			id FOR ORDINALITY,
			"int" int PATH '$',
			"text" text PATH '$',
			"char(4)" char(4) PATH '$',
			"bool" bool PATH '$',
			"numeric" numeric PATH '$',
			"domain" jsonb_test_domain PATH '$',
			js json PATH '$',
			jb jsonb PATH '$'
		)
	) jt
	ON true;

-- "formatted" columns
SELECT *
FROM json_table_test vals
	LEFT OUTER JOIN
	JSON_TABLE(
		vals.js::jsonb, 'lax $[*]'
		COLUMNS (
			id FOR ORDINALITY,
			jst text    FORMAT JSON  PATH '$',
			jsc char(4) FORMAT JSON  PATH '$',
			jsv varchar(4) FORMAT JSON  PATH '$',
			jsb jsonb FORMAT JSON PATH '$',
			jsbq jsonb FORMAT JSON PATH '$' OMIT QUOTES
		)
	) jt
	ON true;

-- EXISTS columns
SELECT *
FROM json_table_test vals
	LEFT OUTER JOIN
	JSON_TABLE(
		vals.js::jsonb, 'lax $[*]'
		COLUMNS (
			id FOR ORDINALITY,
			exists1 bool EXISTS PATH '$.aaa',
			exists2 int EXISTS PATH '$.aaa',
			exists3 int EXISTS PATH 'strict $.aaa' UNKNOWN ON ERROR,
			exists4 text EXISTS PATH 'strict $.aaa' FALSE ON ERROR
		)
	) jt
	ON true;

-- Other miscellaneous checks
SELECT *
FROM json_table_test vals
	LEFT OUTER JOIN
	JSON_TABLE(
		vals.js::jsonb, 'lax $[*]'
		COLUMNS (
			id FOR ORDINALITY,
			aaa int, -- "aaa" has implicit path '$."aaa"'
			aaa1 int PATH '$.aaa',
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

CREATE VIEW jsonb_table_view2 AS
SELECT * FROM
	JSON_TABLE(
		jsonb 'null', 'lax $[*]' PASSING 1 + 2 AS a, json '"foo"' AS "b c"
		COLUMNS (
			"int" int PATH '$',
			"text" text PATH '$',
			"char(4)" char(4) PATH '$',
			"bool" bool PATH '$',
			"numeric" numeric PATH '$',
			"domain" jsonb_test_domain PATH '$'));

CREATE VIEW jsonb_table_view3 AS
SELECT * FROM
	JSON_TABLE(
		jsonb 'null', 'lax $[*]' PASSING 1 + 2 AS a, json '"foo"' AS "b c"
		COLUMNS (
			js json PATH '$',
			jb jsonb PATH '$',
			jst text    FORMAT JSON  PATH '$',
			jsc char(4) FORMAT JSON  PATH '$',
			jsv varchar(4) FORMAT JSON  PATH '$'));

CREATE VIEW jsonb_table_view4 AS
SELECT * FROM
	JSON_TABLE(
		jsonb 'null', 'lax $[*]' PASSING 1 + 2 AS a, json '"foo"' AS "b c"
		COLUMNS (
            jsb jsonb   FORMAT JSON PATH '$',
            jsbq jsonb FORMAT JSON PATH '$' OMIT QUOTES,
            aaa int, -- implicit path '$."aaa"',
            aaa1 int PATH '$.aaa'));

CREATE VIEW jsonb_table_view5 AS
SELECT * FROM
	JSON_TABLE(
		jsonb 'null', 'lax $[*]' PASSING 1 + 2 AS a, json '"foo"' AS "b c"
		COLUMNS (
			exists1 bool EXISTS PATH '$.aaa',
			exists2 int EXISTS PATH '$.aaa' TRUE ON ERROR,
			exists3 text EXISTS PATH 'strict $.aaa' UNKNOWN ON ERROR));

CREATE VIEW jsonb_table_view6 AS
SELECT * FROM
	JSON_TABLE(
		jsonb 'null', 'lax $[*]' PASSING 1 + 2 AS a, json '"foo"' AS "b c"
		COLUMNS (
			js2 json PATH '$',
			jsb2w jsonb PATH '$' WITH WRAPPER,
			jsb2q jsonb PATH '$' OMIT QUOTES,
			ia int[] PATH '$',
			ta text[] PATH '$',
			jba jsonb[] PATH '$'));

\sv jsonb_table_view2
\sv jsonb_table_view3
\sv jsonb_table_view4
\sv jsonb_table_view5
\sv jsonb_table_view6

EXPLAIN (COSTS OFF, VERBOSE) SELECT * FROM jsonb_table_view2;
EXPLAIN (COSTS OFF, VERBOSE) SELECT * FROM jsonb_table_view3;
EXPLAIN (COSTS OFF, VERBOSE) SELECT * FROM jsonb_table_view4;
EXPLAIN (COSTS OFF, VERBOSE) SELECT * FROM jsonb_table_view5;
EXPLAIN (COSTS OFF, VERBOSE) SELECT * FROM jsonb_table_view6;

-- JSON_TABLE() with alias
EXPLAIN (COSTS OFF, VERBOSE)
SELECT * FROM
	JSON_TABLE(
		jsonb 'null', 'lax $[*]' PASSING 1 + 2 AS a, json '"foo"' AS "b c"
		COLUMNS (
			id FOR ORDINALITY,
			"int" int PATH '$',
			"text" text PATH '$'
	)) json_table_func;

EXPLAIN (COSTS OFF, FORMAT JSON, VERBOSE)
SELECT * FROM
	JSON_TABLE(
		jsonb 'null', 'lax $[*]' PASSING 1 + 2 AS a, json '"foo"' AS "b c"
		COLUMNS (
			id FOR ORDINALITY,
			"int" int PATH '$',
			"text" text PATH '$'
	)) json_table_func;

DROP VIEW jsonb_table_view2;
DROP VIEW jsonb_table_view3;
DROP VIEW jsonb_table_view4;
DROP VIEW jsonb_table_view5;
DROP VIEW jsonb_table_view6;
DROP DOMAIN jsonb_test_domain;

-- JSON_TABLE: only one FOR ORDINALITY columns allowed
SELECT * FROM JSON_TABLE(jsonb '1', '$' COLUMNS (id FOR ORDINALITY, id2 FOR ORDINALITY, a int PATH '$.a' ERROR ON EMPTY)) jt;
SELECT * FROM JSON_TABLE(jsonb '1', '$' COLUMNS (id FOR ORDINALITY, a int PATH '$' ERROR ON EMPTY)) jt;

-- JSON_TABLE: ON EMPTY/ON ERROR behavior
SELECT *
FROM
	(VALUES ('1'), ('"err"')) vals(js),
	JSON_TABLE(vals.js::jsonb, '$' COLUMNS (a int PATH '$')) jt;

SELECT *
FROM
	(VALUES ('1'), ('"err"')) vals(js)
		LEFT OUTER JOIN
	JSON_TABLE(vals.js::jsonb, '$' COLUMNS (a int PATH '$' ERROR ON ERROR)) jt
		ON true;

-- TABLE-level ERROR ON ERROR is not propagated to columns
SELECT *
FROM
	(VALUES ('1'), ('"err"')) vals(js)
		LEFT OUTER JOIN
	JSON_TABLE(vals.js::jsonb, '$' COLUMNS (a int PATH '$' ERROR ON ERROR)) jt
		ON true;

SELECT * FROM JSON_TABLE(jsonb '1', '$' COLUMNS (a int PATH '$.a' ERROR ON EMPTY)) jt;
SELECT * FROM JSON_TABLE(jsonb '1', '$' COLUMNS (a int PATH 'strict $.a' ERROR ON ERROR) ERROR ON ERROR) jt;
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

-- JSON_TABLE: WRAPPER/QUOTES clauses on scalar columns
SELECT * FROM JSON_TABLE(jsonb '"world"', '$' COLUMNS (item text PATH '$' KEEP QUOTES ON SCALAR STRING));
SELECT * FROM JSON_TABLE(jsonb '"world"', '$' COLUMNS (item text PATH '$' OMIT QUOTES ON SCALAR STRING));
SELECT * FROM JSON_TABLE(jsonb '"world"', '$' COLUMNS (item text FORMAT JSON PATH '$' KEEP QUOTES));
SELECT * FROM JSON_TABLE(jsonb '"world"', '$' COLUMNS (item text FORMAT JSON PATH '$' OMIT QUOTES));
SELECT * FROM JSON_TABLE(jsonb '"world"', '$' COLUMNS (item text FORMAT JSON PATH '$' WITHOUT WRAPPER KEEP QUOTES));
SELECT * FROM JSON_TABLE(jsonb '"world"', '$' COLUMNS (item text PATH '$' WITHOUT WRAPPER OMIT QUOTES));

SELECT * FROM JSON_TABLE(jsonb '"world"', '$' COLUMNS (item text FORMAT JSON PATH '$' WITH WRAPPER));

-- Error: QUOTES clause meaningless when WITH WRAPPER is present
SELECT * FROM JSON_TABLE(jsonb '"world"', '$' COLUMNS (item text FORMAT JSON PATH '$' WITH WRAPPER KEEP QUOTES));
SELECT * FROM JSON_TABLE(jsonb '"world"', '$' COLUMNS (item text PATH '$' WITH WRAPPER OMIT QUOTES));

-- Test PASSING args
SELECT *
FROM JSON_TABLE(
	jsonb '[1,2,3]',
	'$[*] ? (@ < $x)'
		PASSING 3 AS x
		COLUMNS (y text FORMAT JSON PATH '$')
	) jt;

-- PASSING arguments are also passed to column paths
SELECT *
FROM JSON_TABLE(
	jsonb '[1,2,3]',
	'$[*] ? (@ < $x)'
		PASSING 10 AS x, 3 AS y
		COLUMNS (a text FORMAT JSON PATH '$ ? (@ < $y)')
	) jt;

-- Should fail (not supported)
SELECT * FROM JSON_TABLE(jsonb '{"a": 123}', '$' || '.' || 'a' COLUMNS (foo int));
