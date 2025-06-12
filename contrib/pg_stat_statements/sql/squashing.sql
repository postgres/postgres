--
-- Const squashing functionality
--
CREATE EXTENSION pg_stat_statements;

--
-- Simple Lists
--

CREATE TABLE test_squash (id int, data int);

-- single element will not be squashed
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash WHERE id IN (1);
SELECT ARRAY[1];
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- more than 1 element in a list will be squashed
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash WHERE id IN (1, 2, 3);
SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4);
SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4, 5);
SELECT ARRAY[1, 2, 3];
SELECT ARRAY[1, 2, 3, 4];
SELECT ARRAY[1, 2, 3, 4, 5];
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- built-in functions will be squashed
-- the IN and ARRAY forms of this statement will have the same queryId
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT WHERE 1 IN (1, int4(1), int4(2), 2);
SELECT WHERE 1 = ANY (ARRAY[1, int4(1), int4(2), 2]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- external parameters will not be squashed
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash WHERE id IN ($1, $2, $3, $4, $5)  \bind 1 2 3 4 5
;
SELECT * FROM test_squash WHERE id::text = ANY(ARRAY[$1, $2, $3, $4, $5]) \bind 1 2 3 4 5
;
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- neither are prepared statements
-- the IN and ARRAY forms of this statement will have the same queryId
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
PREPARE p1(int, int, int, int, int) AS
SELECT * FROM test_squash WHERE id IN ($1, $2, $3, $4, $5);
EXECUTE p1(1, 2, 3, 4, 5);
DEALLOCATE p1;
PREPARE p1(int, int, int, int, int) AS
SELECT * FROM test_squash WHERE id = ANY(ARRAY[$1, $2, $3, $4, $5]);
EXECUTE p1(1, 2, 3, 4, 5);
DEALLOCATE p1;
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- More conditions in the query
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4, 5, 6, 7, 8, 9) AND data = 2;
SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10) AND data = 2;
SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11) AND data = 2;
SELECT * FROM test_squash WHERE id = ANY (ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9]) AND data = 2;
SELECT * FROM test_squash WHERE id = ANY (ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10]) AND data = 2;
SELECT * FROM test_squash WHERE id = ANY (ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]) AND data = 2;
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Multiple squashed intervals
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4, 5, 6, 7, 8, 9)
    AND data IN (1, 2, 3, 4, 5, 6, 7, 8, 9);
SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
    AND data IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11)
    AND data IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11);
SELECT * FROM test_squash WHERE id = ANY (ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9])
    AND data = ANY (ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9]);
SELECT * FROM test_squash WHERE id = ANY (ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10])
    AND data = ANY (ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);
SELECT * FROM test_squash WHERE id = ANY (ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11])
    AND data = ANY (ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- No constants squashing for OpExpr
-- The IN and ARRAY forms of this statement will have the same queryId
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash WHERE id IN
	(1 + 1, 2 + 2, 3 + 3, 4 + 4, 5 + 5, 6 + 6, 7 + 7, 8 + 8, 9 + 9);
SELECT * FROM test_squash WHERE id IN
	(@ '-1', @ '-2', @ '-3', @ '-4', @ '-5', @ '-6', @ '-7', @ '-8', @ '-9');
SELECT * FROM test_squash WHERE id = ANY(ARRAY
	[1 + 1, 2 + 2, 3 + 3, 4 + 4, 5 + 5, 6 + 6, 7 + 7, 8 + 8, 9 + 9]);
SELECT * FROM test_squash WHERE id = ANY(ARRAY
	[@ '-1', @ '-2', @ '-3', @ '-4', @ '-5', @ '-6', @ '-7', @ '-8', @ '-9']);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- FuncExpr
--

-- Verify multiple type representation end up with the same query_id
CREATE TABLE test_float (data float);
-- The casted ARRAY expressions will have the same queryId as the IN clause
-- form of the query
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT data FROM test_float WHERE data IN (1, 2);
SELECT data FROM test_float WHERE data IN (1, '2');
SELECT data FROM test_float WHERE data IN ('1', 2);
SELECT data FROM test_float WHERE data IN ('1', '2');
SELECT data FROM test_float WHERE data IN (1.0, 1.0);
SELECT data FROM test_float WHERE data = ANY(ARRAY['1'::double precision, '2'::double precision]);
SELECT data FROM test_float WHERE data = ANY(ARRAY[1.0::double precision, 1.0::double precision]);
SELECT data FROM test_float WHERE data = ANY(ARRAY[1, 2]);
SELECT data FROM test_float WHERE data = ANY(ARRAY[1, '2']);
SELECT data FROM test_float WHERE data = ANY(ARRAY['1', 2]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Numeric type, implicit cast is squashed
CREATE TABLE test_squash_numeric (id int, data numeric(5, 2));
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash_numeric WHERE data IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11);
SELECT * FROM test_squash_numeric WHERE data = ANY(ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Bigint, implicit cast is squashed
CREATE TABLE test_squash_bigint (id int, data bigint);
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash_bigint WHERE data IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11);
SELECT * FROM test_squash_bigint WHERE data = ANY(ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Bigint, explicit cast is squashed
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash_bigint WHERE data IN
	(1::bigint, 2::bigint, 3::bigint, 4::bigint, 5::bigint, 6::bigint,
	 7::bigint, 8::bigint, 9::bigint, 10::bigint, 11::bigint);
SELECT * FROM test_squash_bigint WHERE data = ANY(ARRAY[
	 1::bigint, 2::bigint, 3::bigint, 4::bigint, 5::bigint, 6::bigint,
	 7::bigint, 8::bigint, 9::bigint, 10::bigint, 11::bigint]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Bigint, long tokens with parenthesis, will not squash
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash_bigint WHERE id IN
	(abs(100), abs(200), abs(300), abs(400), abs(500), abs(600), abs(700),
	 abs(800), abs(900), abs(1000), ((abs(1100))));
SELECT * FROM test_squash_bigint WHERE id = ANY(ARRAY[
	 abs(100), abs(200), abs(300), abs(400), abs(500), abs(600), abs(700),
	 abs(800), abs(900), abs(1000), ((abs(1100)))]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Multiple FuncExpr's. Will not squash
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT WHERE 1 IN (1::int::bigint::int, 2::int::bigint::int);
SELECT WHERE 1 = ANY(ARRAY[1::int::bigint::int, 2::int::bigint::int]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- CoerceViaIO
--

-- Create some dummy type to force CoerceViaIO
CREATE TYPE casttesttype;

CREATE FUNCTION casttesttype_in(cstring)
   RETURNS casttesttype
   AS 'textin'
   LANGUAGE internal STRICT IMMUTABLE;

CREATE FUNCTION casttesttype_out(casttesttype)
   RETURNS cstring
   AS 'textout'
   LANGUAGE internal STRICT IMMUTABLE;

CREATE TYPE casttesttype (
   internallength = variable,
   input = casttesttype_in,
   output = casttesttype_out,
   alignment = int4
);

CREATE CAST (int4 AS casttesttype) WITH INOUT;

CREATE FUNCTION casttesttype_eq(casttesttype, casttesttype)
returns boolean language sql immutable as $$
    SELECT true
$$;

CREATE OPERATOR = (
    leftarg = casttesttype,
    rightarg = casttesttype,
    procedure = casttesttype_eq,
    commutator = =);

CREATE TABLE test_squash_cast (id int, data casttesttype);

-- Use the introduced type to construct a list of CoerceViaIO around Const
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash_cast WHERE data IN
	(1::int4::casttesttype, 2::int4::casttesttype, 3::int4::casttesttype,
	 4::int4::casttesttype, 5::int4::casttesttype, 6::int4::casttesttype,
	 7::int4::casttesttype, 8::int4::casttesttype, 9::int4::casttesttype,
	 10::int4::casttesttype, 11::int4::casttesttype);
SELECT * FROM test_squash_cast WHERE data = ANY (ARRAY
	[1::int4::casttesttype, 2::int4::casttesttype, 3::int4::casttesttype,
	 4::int4::casttesttype, 5::int4::casttesttype, 6::int4::casttesttype,
	 7::int4::casttesttype, 8::int4::casttesttype, 9::int4::casttesttype,
	 10::int4::casttesttype, 11::int4::casttesttype]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Some casting expression are simplified to Const
CREATE TABLE test_squash_jsonb (id int, data jsonb);
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash_jsonb WHERE data IN
	(('"1"')::jsonb, ('"2"')::jsonb, ('"3"')::jsonb, ('"4"')::jsonb,
	 ('"5"')::jsonb, ('"6"')::jsonb, ('"7"')::jsonb, ('"8"')::jsonb,
	 ('"9"')::jsonb, ('"10"')::jsonb);
SELECT * FROM test_squash_jsonb WHERE data = ANY (ARRAY
	[('"1"')::jsonb, ('"2"')::jsonb, ('"3"')::jsonb, ('"4"')::jsonb,
	 ('"5"')::jsonb, ('"6"')::jsonb, ('"7"')::jsonb, ('"8"')::jsonb,
	 ('"9"')::jsonb, ('"10"')::jsonb]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- CoerceViaIO, SubLink instead of a Const. Will not squash
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash_jsonb WHERE data IN
	((SELECT '"1"')::jsonb, (SELECT '"2"')::jsonb, (SELECT '"3"')::jsonb,
	 (SELECT '"4"')::jsonb, (SELECT '"5"')::jsonb, (SELECT '"6"')::jsonb,
	 (SELECT '"7"')::jsonb, (SELECT '"8"')::jsonb, (SELECT '"9"')::jsonb,
	 (SELECT '"10"')::jsonb);
SELECT * FROM test_squash_jsonb WHERE data = ANY(ARRAY
	[(SELECT '"1"')::jsonb, (SELECT '"2"')::jsonb, (SELECT '"3"')::jsonb,
	 (SELECT '"4"')::jsonb, (SELECT '"5"')::jsonb, (SELECT '"6"')::jsonb,
	 (SELECT '"7"')::jsonb, (SELECT '"8"')::jsonb, (SELECT '"9"')::jsonb,
	 (SELECT '"10"')::jsonb]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Multiple CoerceViaIO wrapping a constant. Will not squash
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT WHERE 1 IN (1::text::int::text::int, 1::text::int::text::int);
SELECT WHERE 1 = ANY(ARRAY[1::text::int::text::int, 1::text::int::text::int]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- RelabelType
--

SELECT pg_stat_statements_reset() IS NOT NULL AS t;
-- if there is only one level of RelabelType, the list will be squashable
SELECT * FROM test_squash WHERE id IN
	(1::oid, 2::oid, 3::oid, 4::oid, 5::oid, 6::oid, 7::oid, 8::oid, 9::oid);
SELECT ARRAY[1::oid, 2::oid, 3::oid, 4::oid, 5::oid, 6::oid, 7::oid, 8::oid, 9::oid];
-- if there is at least one element with multiple levels of RelabelType,
-- the list will not be squashable
SELECT * FROM test_squash WHERE id IN (1::oid, 2::oid::int::oid);
SELECT * FROM test_squash WHERE id = ANY(ARRAY[1::oid, 2::oid::int::oid]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- edge cases
--

SELECT pg_stat_statements_reset() IS NOT NULL AS t;
-- for nested arrays, only constants are squashed
SELECT ARRAY[
    ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10],
    ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10],
    ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10],
    ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
    ];
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Test constants evaluation in a CTE, which was causing issues in the past
WITH cte AS (
    SELECT 'const' as const FROM test_squash
)
SELECT ARRAY['a', 'b', 'c', const::varchar] AS result
FROM cte;

SELECT pg_stat_statements_reset() IS NOT NULL AS t;
-- Rewritten as an OpExpr, so it will not be squashed
select where '1' IN ('1'::int, '2'::int::text);
-- Rewritten as an ArrayExpr, so it will be squashed
select where '1' IN ('1'::int, '2'::int);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

SELECT pg_stat_statements_reset() IS NOT NULL AS t;
-- Both of these queries will be rewritten as an ArrayExpr, so they
-- will be squashed, and have a similar queryId
select where '1' IN ('1'::int::text, '2'::int::text);
select where '1' = ANY (array['1'::int::text, '2'::int::text]);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- cleanup
--
DROP TABLE test_squash;
DROP TABLE test_float;
DROP TABLE test_squash_numeric;
DROP TABLE test_squash_bigint;
DROP TABLE test_squash_cast CASCADE;
DROP TABLE test_squash_jsonb;
