-- JSON()
SELECT JSON();
SELECT JSON(NULL);
SELECT JSON('{ "a" : 1 } ');
SELECT JSON('{ "a" : 1 } ' FORMAT JSON);
SELECT JSON('{ "a" : 1 } ' FORMAT JSON ENCODING UTF8);
SELECT JSON('{ "a" : 1 } '::bytea FORMAT JSON ENCODING UTF8);
SELECT pg_typeof(JSON('{ "a" : 1 } '));

SELECT JSON('   1   '::json);
SELECT JSON('   1   '::jsonb);
SELECT JSON('   1   '::json WITH UNIQUE KEYS);
SELECT JSON(123);

SELECT JSON('{"a": 1, "a": 2}');
SELECT JSON('{"a": 1, "a": 2}' WITH UNIQUE KEYS);
SELECT JSON('{"a": 1, "a": 2}' WITHOUT UNIQUE KEYS);

EXPLAIN (VERBOSE, COSTS OFF) SELECT JSON('123');
EXPLAIN (VERBOSE, COSTS OFF) SELECT JSON('123' FORMAT JSON);
EXPLAIN (VERBOSE, COSTS OFF) SELECT JSON('123'::bytea FORMAT JSON);
EXPLAIN (VERBOSE, COSTS OFF) SELECT JSON('123'::bytea FORMAT JSON ENCODING UTF8);
EXPLAIN (VERBOSE, COSTS OFF) SELECT JSON('123' WITH UNIQUE KEYS);
EXPLAIN (VERBOSE, COSTS OFF) SELECT JSON('123' WITHOUT UNIQUE KEYS);

EXPLAIN (VERBOSE, COSTS OFF) SELECT JSON('123');
SELECT pg_typeof(JSON('123'));

-- JSON_SCALAR()
SELECT JSON_SCALAR();
SELECT JSON_SCALAR(NULL);
SELECT JSON_SCALAR(NULL::int);
SELECT JSON_SCALAR(123);
SELECT JSON_SCALAR(123.45);
SELECT JSON_SCALAR(123.45::numeric);
SELECT JSON_SCALAR(true);
SELECT JSON_SCALAR(false);
SELECT JSON_SCALAR(' 123.45');
SELECT JSON_SCALAR('2020-06-07'::date);
SELECT JSON_SCALAR('2020-06-07 01:02:03'::timestamp);
SELECT JSON_SCALAR('{}'::json);
SELECT JSON_SCALAR('{}'::jsonb);

EXPLAIN (VERBOSE, COSTS OFF) SELECT JSON_SCALAR(123);
EXPLAIN (VERBOSE, COSTS OFF) SELECT JSON_SCALAR('123');

-- JSON_SERIALIZE()
SELECT JSON_SERIALIZE();
SELECT JSON_SERIALIZE(NULL);
SELECT JSON_SERIALIZE(JSON('{ "a" : 1 } '));
SELECT JSON_SERIALIZE('{ "a" : 1 } ');
SELECT JSON_SERIALIZE('1');
SELECT JSON_SERIALIZE('1' FORMAT JSON);
SELECT JSON_SERIALIZE('{ "a" : 1 } ' RETURNING bytea);
SELECT JSON_SERIALIZE('{ "a" : 1 } ' RETURNING varchar);
SELECT pg_typeof(JSON_SERIALIZE(NULL));

-- only string types or bytea allowed
SELECT JSON_SERIALIZE('{ "a" : 1 } ' RETURNING jsonb);


EXPLAIN (VERBOSE, COSTS OFF) SELECT JSON_SERIALIZE('{}');
EXPLAIN (VERBOSE, COSTS OFF) SELECT JSON_SERIALIZE('{}' RETURNING bytea);

-- JSON_OBJECT()
SELECT JSON_OBJECT();
SELECT JSON_OBJECT(RETURNING json);
SELECT JSON_OBJECT(RETURNING json FORMAT JSON);
SELECT JSON_OBJECT(RETURNING jsonb);
SELECT JSON_OBJECT(RETURNING jsonb FORMAT JSON);
SELECT JSON_OBJECT(RETURNING text);
SELECT JSON_OBJECT(RETURNING text FORMAT JSON);
SELECT JSON_OBJECT(RETURNING text FORMAT JSON ENCODING UTF8);
SELECT JSON_OBJECT(RETURNING text FORMAT JSON ENCODING INVALID_ENCODING);
SELECT JSON_OBJECT(RETURNING bytea);
SELECT JSON_OBJECT(RETURNING bytea FORMAT JSON);
SELECT JSON_OBJECT(RETURNING bytea FORMAT JSON ENCODING UTF8);
SELECT JSON_OBJECT(RETURNING bytea FORMAT JSON ENCODING UTF16);
SELECT JSON_OBJECT(RETURNING bytea FORMAT JSON ENCODING UTF32);

SELECT JSON_OBJECT('foo': NULL::int FORMAT JSON);
SELECT JSON_OBJECT('foo': NULL::int FORMAT JSON ENCODING UTF8);
SELECT JSON_OBJECT('foo': NULL::json FORMAT JSON);
SELECT JSON_OBJECT('foo': NULL::json FORMAT JSON ENCODING UTF8);
SELECT JSON_OBJECT('foo': NULL::jsonb FORMAT JSON);
SELECT JSON_OBJECT('foo': NULL::jsonb FORMAT JSON ENCODING UTF8);

SELECT JSON_OBJECT(NULL: 1);
SELECT JSON_OBJECT('a': 2 + 3);
SELECT JSON_OBJECT('a' VALUE 2 + 3);
--SELECT JSON_OBJECT(KEY 'a' VALUE 2 + 3);
SELECT JSON_OBJECT('a' || 2: 1);
SELECT JSON_OBJECT(('a' || 2) VALUE 1);
--SELECT JSON_OBJECT('a' || 2 VALUE 1);
--SELECT JSON_OBJECT(KEY 'a' || 2 VALUE 1);
SELECT JSON_OBJECT('a': 2::text);
SELECT JSON_OBJECT('a' VALUE 2::text);
--SELECT JSON_OBJECT(KEY 'a' VALUE 2::text);
SELECT JSON_OBJECT(1::text: 2);
SELECT JSON_OBJECT((1::text) VALUE 2);
--SELECT JSON_OBJECT(1::text VALUE 2);
--SELECT JSON_OBJECT(KEY 1::text VALUE 2);
SELECT JSON_OBJECT(json '[1]': 123);
SELECT JSON_OBJECT(ARRAY[1,2,3]: 'aaa');

SELECT JSON_OBJECT(
	'a': '123',
	1.23: 123,
	'c': json '[ 1,true,{ } ]',
	'd': jsonb '{ "x" : 123.45 }'
);

SELECT JSON_OBJECT(
	'a': '123',
	1.23: 123,
	'c': json '[ 1,true,{ } ]',
	'd': jsonb '{ "x" : 123.45 }'
	RETURNING jsonb
);

/*
SELECT JSON_OBJECT(
	'a': '123',
	KEY 1.23 VALUE 123,
	'c' VALUE json '[1, true, {}]'
);
*/

SELECT JSON_OBJECT('a': '123', 'b': JSON_OBJECT('a': 111, 'b': 'aaa'));
SELECT JSON_OBJECT('a': '123', 'b': JSON_OBJECT('a': 111, 'b': 'aaa' RETURNING jsonb));

SELECT JSON_OBJECT('a': JSON_OBJECT('b': 1 RETURNING text));
SELECT JSON_OBJECT('a': JSON_OBJECT('b': 1 RETURNING text) FORMAT JSON);
SELECT JSON_OBJECT('a': JSON_OBJECT('b': 1 RETURNING bytea));
SELECT JSON_OBJECT('a': JSON_OBJECT('b': 1 RETURNING bytea) FORMAT JSON);

SELECT JSON_OBJECT('a': '1', 'b': NULL, 'c': 2);
SELECT JSON_OBJECT('a': '1', 'b': NULL, 'c': 2 NULL ON NULL);
SELECT JSON_OBJECT('a': '1', 'b': NULL, 'c': 2 ABSENT ON NULL);

SELECT JSON_OBJECT(1: 1, '2': NULL, '3': 1, repeat('x', 1000): 1, 2: repeat('a', 100) WITH UNIQUE);

SELECT JSON_OBJECT(1: 1, '1': NULL WITH UNIQUE);
SELECT JSON_OBJECT(1: 1, '1': NULL ABSENT ON NULL WITH UNIQUE);
SELECT JSON_OBJECT(1: 1, '1': NULL NULL ON NULL WITH UNIQUE RETURNING jsonb);
SELECT JSON_OBJECT(1: 1, '1': NULL ABSENT ON NULL WITH UNIQUE RETURNING jsonb);

SELECT JSON_OBJECT(1: 1, '2': NULL, '1': 1 NULL ON NULL WITH UNIQUE);
SELECT JSON_OBJECT(1: 1, '2': NULL, '1': 1 ABSENT ON NULL WITH UNIQUE);
SELECT JSON_OBJECT(1: 1, '2': NULL, '1': 1 ABSENT ON NULL WITHOUT UNIQUE);
SELECT JSON_OBJECT(1: 1, '2': NULL, '1': 1 ABSENT ON NULL WITH UNIQUE RETURNING jsonb);
SELECT JSON_OBJECT(1: 1, '2': NULL, '1': 1 ABSENT ON NULL WITHOUT UNIQUE RETURNING jsonb);
SELECT JSON_OBJECT(1: 1, '2': NULL, '3': 1, 4: NULL, '5': 'a' ABSENT ON NULL WITH UNIQUE RETURNING jsonb);

-- BUG: https://postgr.es/m/CADXhmgTJtJZK9A3Na_ry%2BXrq-ghjcejBRhcRMzWZvbd__QdgJA%40mail.gmail.com
-- datum_to_jsonb_internal() didn't catch keys that are casts instead of a simple scalar
CREATE TYPE mood AS ENUM ('happy', 'sad', 'neutral');
CREATE FUNCTION mood_to_json(mood) RETURNS json AS $$
  SELECT to_json($1::text);
$$ LANGUAGE sql IMMUTABLE;
CREATE CAST (mood AS json) WITH FUNCTION mood_to_json(mood) AS IMPLICIT;
SELECT JSON_OBJECT('happy'::mood: '123'::jsonb);
DROP CAST (mood AS json);
DROP FUNCTION mood_to_json;
DROP TYPE mood;

-- JSON_ARRAY()
SELECT JSON_ARRAY();
SELECT JSON_ARRAY(RETURNING json);
SELECT JSON_ARRAY(RETURNING json FORMAT JSON);
SELECT JSON_ARRAY(RETURNING jsonb);
SELECT JSON_ARRAY(RETURNING jsonb FORMAT JSON);
SELECT JSON_ARRAY(RETURNING text);
SELECT JSON_ARRAY(RETURNING text FORMAT JSON);
SELECT JSON_ARRAY(RETURNING text FORMAT JSON ENCODING UTF8);
SELECT JSON_ARRAY(RETURNING text FORMAT JSON ENCODING INVALID_ENCODING);
SELECT JSON_ARRAY(RETURNING bytea);
SELECT JSON_ARRAY(RETURNING bytea FORMAT JSON);
SELECT JSON_ARRAY(RETURNING bytea FORMAT JSON ENCODING UTF8);
SELECT JSON_ARRAY(RETURNING bytea FORMAT JSON ENCODING UTF16);
SELECT JSON_ARRAY(RETURNING bytea FORMAT JSON ENCODING UTF32);

SELECT JSON_ARRAY('aaa', 111, true, array[1,2,3], NULL, json '{"a": [1]}', jsonb '["a",3]');

SELECT JSON_ARRAY('a',  NULL, 'b' NULL   ON NULL);
SELECT JSON_ARRAY('a',  NULL, 'b' ABSENT ON NULL);
SELECT JSON_ARRAY(NULL, NULL, 'b' ABSENT ON NULL);
SELECT JSON_ARRAY('a',  NULL, 'b' NULL   ON NULL RETURNING jsonb);
SELECT JSON_ARRAY('a',  NULL, 'b' ABSENT ON NULL RETURNING jsonb);
SELECT JSON_ARRAY(NULL, NULL, 'b' ABSENT ON NULL RETURNING jsonb);

SELECT JSON_ARRAY(JSON_ARRAY('{ "a" : 123 }' RETURNING text));
SELECT JSON_ARRAY(JSON_ARRAY('{ "a" : 123 }' FORMAT JSON RETURNING text));
SELECT JSON_ARRAY(JSON_ARRAY('{ "a" : 123 }' FORMAT JSON RETURNING text) FORMAT JSON);

SELECT JSON_ARRAY(SELECT i FROM (VALUES (1), (2), (NULL), (4)) foo(i));
SELECT JSON_ARRAY(SELECT i FROM (VALUES (NULL::int[]), ('{1,2}'), (NULL), (NULL), ('{3,4}'), (NULL)) foo(i));
SELECT JSON_ARRAY(SELECT i FROM (VALUES (NULL::int[]), ('{1,2}'), (NULL), (NULL), ('{3,4}'), (NULL)) foo(i) RETURNING jsonb);
--SELECT JSON_ARRAY(SELECT i FROM (VALUES (NULL::int[]), ('{1,2}'), (NULL), (NULL), ('{3,4}'), (NULL)) foo(i) NULL ON NULL);
--SELECT JSON_ARRAY(SELECT i FROM (VALUES (NULL::int[]), ('{1,2}'), (NULL), (NULL), ('{3,4}'), (NULL)) foo(i) NULL ON NULL RETURNING jsonb);
SELECT JSON_ARRAY(SELECT i FROM (VALUES (3), (1), (NULL), (2)) foo(i) ORDER BY i);
SELECT JSON_ARRAY(WITH x AS (SELECT 1) VALUES (TRUE));

-- Should fail
SELECT JSON_ARRAY(SELECT FROM (VALUES (1)) foo(i));
SELECT JSON_ARRAY(SELECT i, i FROM (VALUES (1)) foo(i));
SELECT JSON_ARRAY(SELECT * FROM (VALUES (1, 2)) foo(i, j));

-- JSON_ARRAYAGG()
SELECT	JSON_ARRAYAGG(i) IS NULL,
		JSON_ARRAYAGG(i RETURNING jsonb) IS NULL
FROM generate_series(1, 0) i;

SELECT	JSON_ARRAYAGG(i),
		JSON_ARRAYAGG(i RETURNING jsonb)
FROM generate_series(1, 5) i;

SELECT JSON_ARRAYAGG(i ORDER BY i DESC)
FROM generate_series(1, 5) i;

SELECT JSON_ARRAYAGG(i::text::json)
FROM generate_series(1, 5) i;

SELECT JSON_ARRAYAGG(JSON_ARRAY(i, i + 1 RETURNING text) FORMAT JSON)
FROM generate_series(1, 5) i;

SELECT	JSON_ARRAYAGG(NULL),
		JSON_ARRAYAGG(NULL RETURNING jsonb)
FROM generate_series(1, 5);

SELECT	JSON_ARRAYAGG(NULL NULL ON NULL),
		JSON_ARRAYAGG(NULL NULL ON NULL RETURNING jsonb)
FROM generate_series(1, 5);

\x
SELECT
	JSON_ARRAYAGG(bar) as no_options,
	JSON_ARRAYAGG(bar RETURNING jsonb) as returning_jsonb,
	JSON_ARRAYAGG(bar ABSENT ON NULL) as absent_on_null,
	JSON_ARRAYAGG(bar ABSENT ON NULL RETURNING jsonb) as absentonnull_returning_jsonb,
	JSON_ARRAYAGG(bar NULL ON NULL) as null_on_null,
	JSON_ARRAYAGG(bar NULL ON NULL RETURNING jsonb) as nullonnull_returning_jsonb,
	JSON_ARRAYAGG(foo) as row_no_options,
	JSON_ARRAYAGG(foo RETURNING jsonb) as row_returning_jsonb,
	JSON_ARRAYAGG(foo ORDER BY bar) FILTER (WHERE bar > 2) as row_filtered_agg,
	JSON_ARRAYAGG(foo ORDER BY bar RETURNING jsonb) FILTER (WHERE bar > 2) as row_filtered_agg_returning_jsonb
FROM
	(VALUES (NULL), (3), (1), (NULL), (NULL), (5), (2), (4), (NULL)) foo(bar);
\x

SELECT
	bar, JSON_ARRAYAGG(bar) FILTER (WHERE bar > 2) OVER (PARTITION BY foo.bar % 2)
FROM
	(VALUES (NULL), (3), (1), (NULL), (NULL), (5), (2), (4), (NULL), (5), (4)) foo(bar);

-- JSON_OBJECTAGG()
SELECT	JSON_OBJECTAGG('key': 1) IS NULL,
		JSON_OBJECTAGG('key': 1 RETURNING jsonb) IS NULL
WHERE FALSE;

SELECT JSON_OBJECTAGG(NULL: 1);

SELECT JSON_OBJECTAGG(NULL: 1 RETURNING jsonb);

SELECT
	JSON_OBJECTAGG(i: i),
--	JSON_OBJECTAGG(i VALUE i),
--	JSON_OBJECTAGG(KEY i VALUE i),
	JSON_OBJECTAGG(i: i RETURNING jsonb)
FROM
	generate_series(1, 5) i;

SELECT
	JSON_OBJECTAGG(k: v),
	JSON_OBJECTAGG(k: v NULL ON NULL),
	JSON_OBJECTAGG(k: v ABSENT ON NULL),
	JSON_OBJECTAGG(k: v RETURNING jsonb),
	JSON_OBJECTAGG(k: v NULL ON NULL RETURNING jsonb),
	JSON_OBJECTAGG(k: v ABSENT ON NULL RETURNING jsonb)
FROM
	(VALUES (1, 1), (1, NULL), (2, NULL), (3, 3)) foo(k, v);

SELECT JSON_OBJECTAGG(k: v WITH UNIQUE KEYS)
FROM (VALUES (1, 1), (1, NULL), (2, 2)) foo(k, v);

SELECT JSON_OBJECTAGG(k: v ABSENT ON NULL WITH UNIQUE KEYS)
FROM (VALUES (1, 1), (1, NULL), (2, 2)) foo(k, v);

SELECT JSON_OBJECTAGG(k: v ABSENT ON NULL WITH UNIQUE KEYS)
FROM (VALUES (1, 1), (0, NULL), (3, NULL), (2, 2), (4, NULL)) foo(k, v);

SELECT JSON_OBJECTAGG(k: v WITH UNIQUE KEYS RETURNING jsonb)
FROM (VALUES (1, 1), (1, NULL), (2, 2)) foo(k, v);

SELECT JSON_OBJECTAGG(k: v ABSENT ON NULL WITH UNIQUE KEYS RETURNING jsonb)
FROM (VALUES (1, 1), (1, NULL), (2, 2)) foo(k, v);

SELECT JSON_OBJECTAGG(k: v ABSENT ON NULL WITH UNIQUE KEYS RETURNING jsonb)
FROM (VALUES (1, 1), (0, NULL),(4, null), (5, null),(6, null),(2, 2)) foo(k, v);

SELECT JSON_OBJECTAGG(mod(i,100): (i)::text FORMAT JSON WITH UNIQUE)
FROM generate_series(0, 199) i;

-- Test JSON_OBJECT deparsing
EXPLAIN (VERBOSE, COSTS OFF)
SELECT JSON_OBJECT('foo' : '1' FORMAT JSON, 'bar' : 'baz' RETURNING json);

CREATE VIEW json_object_view AS
SELECT JSON_OBJECT('foo' : '1' FORMAT JSON, 'bar' : 'baz' RETURNING json);

\sv json_object_view

DROP VIEW json_object_view;

SELECT to_json(a) AS a, JSON_OBJECTAGG(k : v WITH UNIQUE KEYS) OVER (ORDER BY k)
FROM (VALUES (1,1), (2,2)) a(k,v);

SELECT to_json(a) AS a, JSON_OBJECTAGG(k : v WITH UNIQUE KEYS) OVER (ORDER BY k)
FROM (VALUES (1,1), (1,2), (2,2)) a(k,v);

SELECT to_json(a) AS a, JSON_OBJECTAGG(k : v ABSENT ON NULL WITH UNIQUE KEYS)
   OVER (ORDER BY k)
FROM (VALUES (1,1), (1,null), (2,2)) a(k,v);

SELECT to_json(a) AS a, JSON_OBJECTAGG(k : v ABSENT ON NULL)
OVER (ORDER BY k)
FROM (VALUES (1,1), (1,null), (2,2)) a(k,v);

SELECT to_json(a) AS a, JSON_OBJECTAGG(k : v ABSENT ON NULL)
OVER (ORDER BY k RANGE BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING)
FROM (VALUES (1,1), (1,null), (2,2)) a(k,v);

-- Test JSON_ARRAY deparsing
EXPLAIN (VERBOSE, COSTS OFF)
SELECT JSON_ARRAY('1' FORMAT JSON, 2 RETURNING json);

CREATE VIEW json_array_view AS
SELECT JSON_ARRAY('1' FORMAT JSON, 2 RETURNING json);

\sv json_array_view

DROP VIEW json_array_view;

-- Test JSON_OBJECTAGG deparsing
EXPLAIN (VERBOSE, COSTS OFF)
SELECT JSON_OBJECTAGG(i: ('111' || i)::bytea FORMAT JSON WITH UNIQUE RETURNING text) FILTER (WHERE i > 3)
FROM generate_series(1,5) i;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT JSON_OBJECTAGG(i: ('111' || i)::bytea FORMAT JSON WITH UNIQUE RETURNING text) OVER (PARTITION BY i % 2)
FROM generate_series(1,5) i;

CREATE VIEW json_objectagg_view AS
SELECT JSON_OBJECTAGG(i: ('111' || i)::bytea FORMAT JSON WITH UNIQUE RETURNING text) FILTER (WHERE i > 3)
FROM generate_series(1,5) i;

\sv json_objectagg_view

DROP VIEW json_objectagg_view;

-- Test JSON_ARRAYAGG deparsing
EXPLAIN (VERBOSE, COSTS OFF)
SELECT JSON_ARRAYAGG(('111' || i)::bytea FORMAT JSON NULL ON NULL RETURNING text) FILTER (WHERE i > 3)
FROM generate_series(1,5) i;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT JSON_ARRAYAGG(('111' || i)::bytea FORMAT JSON NULL ON NULL RETURNING text) OVER (PARTITION BY i % 2)
FROM generate_series(1,5) i;

CREATE VIEW json_arrayagg_view AS
SELECT JSON_ARRAYAGG(('111' || i)::bytea FORMAT JSON NULL ON NULL RETURNING text) FILTER (WHERE i > 3)
FROM generate_series(1,5) i;

\sv json_arrayagg_view

DROP VIEW json_arrayagg_view;

-- Test JSON_ARRAY(subquery) deparsing
EXPLAIN (VERBOSE, COSTS OFF)
SELECT JSON_ARRAY(SELECT i FROM (VALUES (1), (2), (NULL), (4)) foo(i) RETURNING jsonb);

CREATE VIEW json_array_subquery_view AS
SELECT JSON_ARRAY(SELECT i FROM (VALUES (1), (2), (NULL), (4)) foo(i) RETURNING jsonb);

\sv json_array_subquery_view

DROP VIEW json_array_subquery_view;

-- Test mutability of JSON_OBJECTAGG, JSON_ARRAYAGG, JSON_ARRAY, JSON_OBJECT
create type comp1 as (a int, b date);
create domain d_comp1 as comp1;
create domain mydomain as timestamptz;
create type mydomainrange as range(subtype=mydomain);
create type comp3 as (a int, b mydomainrange);
create table test_mutability(
	a text[], b timestamp, c timestamptz,
	d date, f1 comp1[], f2 timestamp[],
	f3 d_comp1[],
	f4 mydomainrange[],
	f5 comp3,
	f6 mydomainmultirange);

-- JSON_OBJECTAGG, JSON_ARRAYAGG are aggregate functions, cannot be used in index
create index xx on test_mutability(json_objectagg(a: b absent on null with unique keys returning jsonb));
create index xx on test_mutability(json_objectagg(a: b absent on null with unique keys returning json));
create index xx on test_mutability(json_arrayagg(a returning jsonb));
create index xx on test_mutability(json_arrayagg(a returning json));

-- jsonb: create expression index via json_array
create index on test_mutability(json_array(a returning jsonb)); -- ok
create index on test_mutability(json_array(b returning jsonb)); -- error
create index on test_mutability(json_array(c returning jsonb)); -- error
create index on test_mutability(json_array(d returning jsonb)); -- error
create index on test_mutability(json_array(f1 returning jsonb)); -- error
create index on test_mutability(json_array(f2 returning jsonb)); -- error
create index on test_mutability(json_array(f3 returning jsonb)); -- error
create index on test_mutability(json_array(f4 returning jsonb)); -- error
create index on test_mutability(json_array(f5 returning jsonb)); -- error
create index on test_mutability(json_array(f6 returning jsonb)); -- error

-- jsonb: create expression index via json_object
create index on test_mutability(json_object('hello' value a returning jsonb)); -- ok
create index on test_mutability(json_object('hello' value b returning jsonb)); -- error
create index on test_mutability(json_object('hello' value c returning jsonb)); -- error
create index on test_mutability(json_object('hello' value d returning jsonb)); -- error
create index on test_mutability(json_object('hello' value f1 returning jsonb)); -- error
create index on test_mutability(json_object('hello' value f2 returning jsonb)); -- error
create index on test_mutability(json_object('hello' value f3 returning jsonb)); -- error
create index on test_mutability(json_object('hello' value f4 returning jsonb)); -- error
create index on test_mutability(json_object('hello' value f5 returning jsonb)); -- error
create index on test_mutability(json_object('hello' value f6 returning jsonb)); -- error

-- data type json doesn't have a default operator class for access method "btree" so
-- we use a generated column to test whether the JSON_ARRAY expression is
-- immutable
alter table test_mutability add column f10 json generated always as (json_array(a returning json)); -- ok
alter table test_mutability add column f11 json generated always as (json_array(b returning json)); -- error
alter table test_mutability add column f11 json generated always as (json_array(c returning json)); -- error
alter table test_mutability add column f11 json generated always as (json_array(d returning json)); -- error
alter table test_mutability add column f11 json generated always as (json_array(f1 returning json)); -- error
alter table test_mutability add column f11 json generated always as (json_array(f2 returning json)); -- error
alter table test_mutability add column f11 json generated always as (json_array(f3 returning json)); -- error
alter table test_mutability add column f11 json generated always as (json_array(f4 returning json)); -- error
alter table test_mutability add column f11 json generated always as (json_array(f5 returning json)); -- error
alter table test_mutability add column f11 json generated always as (json_array(f6 returning json)); -- error

-- data type json doesn't have a default operator class for access method "btree" so
-- we use a generated column to test whether the JSON_OBJECT expression is
-- immutable
alter table test_mutability add column f11 json generated always as (json_object('hello' value a returning json)); -- ok
alter table test_mutability add column f12 json generated always as (json_object('hello' value b returning json)); -- error
alter table test_mutability add column f12 json generated always as (json_object('hello' value c returning json)); -- error
alter table test_mutability add column f12 json generated always as (json_object('hello' value d returning json)); -- error
alter table test_mutability add column f12 json generated always as (json_object('hello' value f1 returning json)); -- error
alter table test_mutability add column f12 json generated always as (json_object('hello' value f2 returning json)); -- error
alter table test_mutability add column f12 json generated always as (json_object('hello' value f3 returning json)); -- error
alter table test_mutability add column f12 json generated always as (json_object('hello' value f4 returning json)); -- error
alter table test_mutability add column f12 json generated always as (json_object('hello' value f5 returning json)); -- error
alter table test_mutability add column f12 json generated always as (json_object('hello' value f6 returning json)); -- error

drop table test_mutability;
drop domain d_comp1;
drop type comp3;
drop type mydomainrange;
drop domain mydomain;
drop type comp1;

-- Range/multirange with immutable subtype should be considered immutable
create type range_int as range(subtype=int);
create table test_range_immutable(r range_int, m multirange_int);
create index on test_range_immutable(json_array(r returning jsonb)); -- ok
create index on test_range_immutable(json_array(m returning jsonb)); -- ok
create index on test_range_immutable(json_object('key' value r returning jsonb)); -- ok
create index on test_range_immutable(json_object('key' value m returning jsonb)); -- ok
drop table test_range_immutable;
drop type range_int;

-- IS JSON predicate
SELECT NULL IS JSON;
SELECT NULL IS NOT JSON;
SELECT NULL::json IS JSON;
SELECT NULL::jsonb IS JSON;
SELECT NULL::text IS JSON;
SELECT NULL::bytea IS JSON;
SELECT NULL::int IS JSON;

-- IS JSON with domain types
CREATE DOMAIN jd1 AS json CHECK ((VALUE ->'a')::text <> '3');
CREATE DOMAIN jd2 AS jsonb CHECK ((VALUE ->'a') = '1'::jsonb);
CREATE DOMAIN jd3 AS text CHECK (VALUE <> 'a');
CREATE DOMAIN jd4 AS bytea CHECK (VALUE <> '\x61');
CREATE DOMAIN jd5 AS date CHECK (VALUE <> NULL);

-- NULLs through domains should return NULL (not error)
SELECT NULL::jd1 IS JSON, NULL::jd2 IS JSON, NULL::jd3 IS JSON, NULL::jd4 IS JSON;
SELECT NULL::jd1 IS NOT JSON;

-- domain over unsupported base type should error
SELECT NULL::jd5 IS JSON; -- error
SELECT NULL::jd5 IS JSON WITH UNIQUE KEYS; -- error

-- domain constraint violation during cast
SELECT a::jd2 IS JSON WITH UNIQUE KEYS as col1 FROM (VALUES('{"a": 1, "a": 2}')) s(a); -- error

-- view creation and deparsing with domain IS JSON
CREATE VIEW domain_isjson AS
WITH cte(a) AS (VALUES('{"a": 1, "a": 2}'))
SELECT	a::jd1 IS JSON WITH UNIQUE KEYS as jd1,
		a::jd3 IS JSON WITH UNIQUE KEYS as jd3,
		a::jd4 IS JSON WITH UNIQUE KEYS as jd4
FROM cte;
\sv domain_isjson
SELECT * FROM domain_isjson;

DROP VIEW domain_isjson;
DROP DOMAIN jd5, jd4, jd3, jd2, jd1;

SELECT '' IS JSON;

SELECT bytea '\x00' IS JSON;

CREATE TABLE test_is_json (js text);

INSERT INTO test_is_json VALUES
 (NULL),
 (''),
 ('123'),
 ('"aaa "'),
 ('true'),
 ('null'),
 ('[]'),
 ('[1, "2", {}]'),
 ('{}'),
 ('{ "a": 1, "b": null }'),
 ('{ "a": 1, "a": null }'),
 ('{ "a": 1, "b": [{ "a": 1 }, { "a": 2 }] }'),
 ('{ "a": 1, "b": [{ "a": 1, "b": 0, "a": 2 }] }'),
 ('aaa'),
 ('{a:1}'),
 ('["a",]');

SELECT
	js,
	js IS JSON "IS JSON",
	js IS NOT JSON "IS NOT JSON",
	js IS JSON VALUE "IS VALUE",
	js IS JSON OBJECT "IS OBJECT",
	js IS JSON ARRAY "IS ARRAY",
	js IS JSON SCALAR "IS SCALAR",
	js IS JSON WITHOUT UNIQUE KEYS "WITHOUT UNIQUE",
	js IS JSON WITH UNIQUE KEYS "WITH UNIQUE"
FROM
	test_is_json;

SELECT
	js,
	js IS JSON "IS JSON",
	js IS NOT JSON "IS NOT JSON",
	js IS JSON VALUE "IS VALUE",
	js IS JSON OBJECT "IS OBJECT",
	js IS JSON ARRAY "IS ARRAY",
	js IS JSON SCALAR "IS SCALAR",
	js IS JSON WITHOUT UNIQUE KEYS "WITHOUT UNIQUE",
	js IS JSON WITH UNIQUE KEYS "WITH UNIQUE"
FROM
	(SELECT js::json FROM test_is_json WHERE js IS JSON) foo(js);

SELECT
	js0,
	js IS JSON "IS JSON",
	js IS NOT JSON "IS NOT JSON",
	js IS JSON VALUE "IS VALUE",
	js IS JSON OBJECT "IS OBJECT",
	js IS JSON ARRAY "IS ARRAY",
	js IS JSON SCALAR "IS SCALAR",
	js IS JSON WITHOUT UNIQUE KEYS "WITHOUT UNIQUE",
	js IS JSON WITH UNIQUE KEYS "WITH UNIQUE"
FROM
	(SELECT js, js::bytea FROM test_is_json WHERE js IS JSON) foo(js0, js);

SELECT
	js,
	js IS JSON "IS JSON",
	js IS NOT JSON "IS NOT JSON",
	js IS JSON VALUE "IS VALUE",
	js IS JSON OBJECT "IS OBJECT",
	js IS JSON ARRAY "IS ARRAY",
	js IS JSON SCALAR "IS SCALAR",
	js IS JSON WITHOUT UNIQUE KEYS "WITHOUT UNIQUE",
	js IS JSON WITH UNIQUE KEYS "WITH UNIQUE"
FROM
	(SELECT js::jsonb FROM test_is_json WHERE js IS JSON) foo(js);

-- Test IS JSON deparsing
EXPLAIN (VERBOSE, COSTS OFF)
SELECT '1' IS JSON AS "any", ('1' || i) IS JSON SCALAR AS "scalar", '[]' IS NOT JSON ARRAY AS "array", '{}' IS JSON OBJECT WITH UNIQUE AS "object" FROM generate_series(1, 3) i;

CREATE VIEW is_json_view AS
SELECT '1' IS JSON AS "any", ('1' || i) IS JSON SCALAR AS "scalar", '[]' IS NOT JSON ARRAY AS "array", '{}' IS JSON OBJECT WITH UNIQUE AS "object" FROM generate_series(1, 3) i;

\sv is_json_view

DROP VIEW is_json_view;

-- Test implicit coercion to a fixed-length type specified in RETURNING
SELECT JSON_SERIALIZE('{ "a" : 1 } ' RETURNING varchar(2));
SELECT JSON_OBJECT('a': JSON_OBJECT('b': 1 RETURNING varchar(2)));
SELECT JSON_ARRAY(JSON_ARRAY('{ "a" : 123 }' RETURNING varchar(2)));
SELECT JSON_ARRAYAGG(('111' || i)::bytea FORMAT JSON NULL ON NULL RETURNING varchar(2)) FROM generate_series(1,1) i;
SELECT JSON_OBJECTAGG(i: ('111' || i)::bytea FORMAT JSON WITH UNIQUE RETURNING varchar(2)) FROM generate_series(1, 1) i;

-- Now try domain over fixed-length type
CREATE DOMAIN sqljson_char2 AS char(2) CHECK (VALUE NOT IN ('12'));
SELECT JSON_SERIALIZE('123' RETURNING sqljson_char2);
SELECT JSON_SERIALIZE('12' RETURNING sqljson_char2);

-- Bug #18657: JsonValueExpr.raw_expr was not initialized in ExecInitExprRec()
-- causing the Aggrefs contained in it to also not be initialized, which led
-- to a crash in ExecBuildAggTrans() as mentioned in the bug report:
-- https://postgr.es/m/18657-1b90ccce2b16bdb8@postgresql.org
CREATE FUNCTION volatile_one() RETURNS int AS $$ BEGIN RETURN 1; END; $$ LANGUAGE plpgsql VOLATILE;
CREATE FUNCTION stable_one() RETURNS int AS $$ BEGIN RETURN 1; END; $$ LANGUAGE plpgsql STABLE;
EXPLAIN (VERBOSE, COSTS OFF) SELECT JSON_OBJECT('a': JSON_OBJECTAGG('b': volatile_one() RETURNING text) FORMAT JSON);
SELECT JSON_OBJECT('a': JSON_OBJECTAGG('b': volatile_one() RETURNING text) FORMAT JSON);
EXPLAIN (VERBOSE, COSTS OFF) SELECT JSON_OBJECT('a': JSON_OBJECTAGG('b': stable_one() RETURNING text) FORMAT JSON);
SELECT JSON_OBJECT('a': JSON_OBJECTAGG('b': stable_one() RETURNING text) FORMAT JSON);
EXPLAIN (VERBOSE, COSTS OFF) SELECT JSON_OBJECT('a': JSON_OBJECTAGG('b': 1 RETURNING text) FORMAT JSON);
SELECT JSON_OBJECT('a': JSON_OBJECTAGG('b': 1 RETURNING text) FORMAT JSON);
DROP FUNCTION volatile_one, stable_one;
