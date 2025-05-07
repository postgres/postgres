--
-- Const squashing functionality
--
CREATE EXTENSION pg_stat_statements;

CREATE TABLE test_squash (id int, data int);

-- IN queries

-- Normal scenario, too many simple constants for an IN query
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash WHERE id IN (1);
SELECT * FROM test_squash WHERE id IN (1, 2, 3);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4, 5, 6, 7, 8, 9);
SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- More conditions in the query
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4, 5, 6, 7, 8, 9) AND data = 2;
SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10) AND data = 2;
SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11) AND data = 2;
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Multiple squashed intervals
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4, 5, 6, 7, 8, 9)
    AND data IN (1, 2, 3, 4, 5, 6, 7, 8, 9);
SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
    AND data IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
SELECT * FROM test_squash WHERE id IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11)
    AND data IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- No constants simplification for OpExpr
SELECT pg_stat_statements_reset() IS NOT NULL AS t;

-- In the following two queries the operator expressions (+) and (@) have
-- different oppno, and will be given different query_id if squashed, even though
-- the normalized query will be the same
SELECT * FROM test_squash WHERE id IN
	(1 + 1, 2 + 2, 3 + 3, 4 + 4, 5 + 5, 6 + 6, 7 + 7, 8 + 8, 9 + 9);
SELECT * FROM test_squash WHERE id IN
	(@ '-1', @ '-2', @ '-3', @ '-4', @ '-5', @ '-6', @ '-7', @ '-8', @ '-9');
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- FuncExpr

-- Verify multiple type representation end up with the same query_id
CREATE TABLE test_float (data float);
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT data FROM test_float WHERE data IN (1, 2);
SELECT data FROM test_float WHERE data IN (1, '2');
SELECT data FROM test_float WHERE data IN ('1', 2);
SELECT data FROM test_float WHERE data IN ('1', '2');
SELECT data FROM test_float WHERE data IN (1.0, 1.0);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Numeric type, implicit cast is squashed
CREATE TABLE test_squash_numeric (id int, data numeric(5, 2));
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash_numeric WHERE data IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Bigint, implicit cast is squashed
CREATE TABLE test_squash_bigint (id int, data bigint);
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash_bigint WHERE data IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Bigint, explicit cast is not squashed
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash_bigint WHERE data IN
	(1::bigint, 2::bigint, 3::bigint, 4::bigint, 5::bigint, 6::bigint,
	 7::bigint, 8::bigint, 9::bigint, 10::bigint, 11::bigint);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Bigint, long tokens with parenthesis
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash_bigint WHERE id IN
	(abs(100), abs(200), abs(300), abs(400), abs(500), abs(600), abs(700),
	 abs(800), abs(900), abs(1000), ((abs(1100))));
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- CoerceViaIO, SubLink instead of a Const
CREATE TABLE test_squash_jsonb (id int, data jsonb);
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash_jsonb WHERE data IN
	((SELECT '"1"')::jsonb, (SELECT '"2"')::jsonb, (SELECT '"3"')::jsonb,
	 (SELECT '"4"')::jsonb, (SELECT '"5"')::jsonb, (SELECT '"6"')::jsonb,
	 (SELECT '"7"')::jsonb, (SELECT '"8"')::jsonb, (SELECT '"9"')::jsonb,
	 (SELECT '"10"')::jsonb);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- CoerceViaIO

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
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Some casting expression are simplified to Const
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash_jsonb WHERE data IN
	(('"1"')::jsonb, ('"2"')::jsonb, ('"3"')::jsonb, ('"4"')::jsonb,
	 ( '"5"')::jsonb, ( '"6"')::jsonb, ( '"7"')::jsonb, ( '"8"')::jsonb,
	 ( '"9"')::jsonb, ( '"10"')::jsonb);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- RelabelType
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM test_squash WHERE id IN (1::oid, 2::oid, 3::oid, 4::oid, 5::oid, 6::oid, 7::oid, 8::oid, 9::oid);
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";

-- Test constants evaluation in a CTE, which was causing issues in the past
WITH cte AS (
    SELECT 'const' as const FROM test_squash
)
SELECT ARRAY['a', 'b', 'c', const::varchar] AS result
FROM cte;

-- Simple array would be squashed as well
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
SELECT query, calls FROM pg_stat_statements ORDER BY query COLLATE "C";
