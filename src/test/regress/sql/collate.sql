/*
 * This test is intended to pass on all platforms supported by Postgres.
 * We can therefore only assume that the default, C, and POSIX collations
 * are available --- and since the regression tests are often run in a
 * C-locale database, these may well all have the same behavior.  But
 * fortunately, the system doesn't know that and will treat them as
 * incompatible collations.  It is therefore at least possible to test
 * parser behaviors such as collation conflict resolution.  This test will,
 * however, be more revealing when run in a database with non-C locale,
 * since any departure from C sorting behavior will show as a failure.
 */

CREATE SCHEMA collate_tests;
SET search_path = collate_tests;

CREATE TABLE collate_test1 (
    a int,
    b text COLLATE "C" NOT NULL
);

\d collate_test1

CREATE TABLE collate_test_fail (
    a int COLLATE "C",
    b text
);

CREATE TABLE collate_test_like (
    LIKE collate_test1
);

\d collate_test_like

CREATE TABLE collate_test2 (
    a int,
    b text COLLATE "POSIX"
);

INSERT INTO collate_test1 VALUES (1, 'abc'), (2, 'Abc'), (3, 'bbc'), (4, 'ABD');
INSERT INTO collate_test2 SELECT * FROM collate_test1;

SELECT * FROM collate_test1 WHERE b COLLATE "C" >= 'abc';
SELECT * FROM collate_test1 WHERE b >= 'abc' COLLATE "C";
SELECT * FROM collate_test1 WHERE b COLLATE "C" >= 'abc' COLLATE "C";
SELECT * FROM collate_test1 WHERE b COLLATE "C" >= 'bbc' COLLATE "POSIX"; -- fail

CREATE DOMAIN testdomain_p AS text COLLATE "POSIX";
CREATE DOMAIN testdomain_i AS int COLLATE "POSIX"; -- fail
CREATE TABLE collate_test4 (
    a int,
    b testdomain_p
);
INSERT INTO collate_test4 SELECT * FROM collate_test1;
SELECT a, b FROM collate_test4 ORDER BY b;

CREATE TABLE collate_test5 (
    a int,
    b testdomain_p COLLATE "C"
);
INSERT INTO collate_test5 SELECT * FROM collate_test1;
SELECT a, b FROM collate_test5 ORDER BY b;


SELECT a, b FROM collate_test1 ORDER BY b;
SELECT a, b FROM collate_test2 ORDER BY b;

SELECT a, b FROM collate_test1 ORDER BY b COLLATE "C";

-- star expansion
SELECT * FROM collate_test1 ORDER BY b;
SELECT * FROM collate_test2 ORDER BY b;

-- constant expression folding
SELECT 'bbc' COLLATE "C" > 'Abc' COLLATE "C" AS "true";
SELECT 'bbc' COLLATE "POSIX" < 'Abc' COLLATE "POSIX" AS "false";

-- upper/lower

CREATE TABLE collate_test10 (
    a int,
    x text COLLATE "C",
    y text COLLATE "POSIX"
);

INSERT INTO collate_test10 VALUES (1, 'hij', 'hij'), (2, 'HIJ', 'HIJ');

SELECT a, lower(x), lower(y), upper(x), upper(y), initcap(x), initcap(y) FROM collate_test10;
SELECT a, lower(x COLLATE "C"), lower(y COLLATE "C") FROM collate_test10;

SELECT a, x, y FROM collate_test10 ORDER BY lower(y), a;

-- backwards parsing

CREATE VIEW collview1 AS SELECT * FROM collate_test1 WHERE b COLLATE "C" >= 'bbc';
CREATE VIEW collview2 AS SELECT a, b FROM collate_test1 ORDER BY b COLLATE "C";
CREATE VIEW collview3 AS SELECT a, lower((x || x) COLLATE "POSIX") FROM collate_test10;

SELECT table_name, view_definition FROM information_schema.views
  WHERE table_name LIKE 'collview%' ORDER BY 1;


-- collation propagation in various expression types

SELECT a, coalesce(b, 'foo') FROM collate_test1 ORDER BY 2;
SELECT a, coalesce(b, 'foo') FROM collate_test2 ORDER BY 2;
SELECT a, lower(coalesce(x, 'foo')), lower(coalesce(y, 'foo')) FROM collate_test10;

SELECT a, b, greatest(b, 'CCC') FROM collate_test1 ORDER BY 3;
SELECT a, b, greatest(b, 'CCC') FROM collate_test2 ORDER BY 3;
SELECT a, x, y, lower(greatest(x, 'foo')), lower(greatest(y, 'foo')) FROM collate_test10;

SELECT a, nullif(b, 'abc') FROM collate_test1 ORDER BY 2;
SELECT a, nullif(b, 'abc') FROM collate_test2 ORDER BY 2;
SELECT a, lower(nullif(x, 'foo')), lower(nullif(y, 'foo')) FROM collate_test10;

SELECT a, CASE b WHEN 'abc' THEN 'abcd' ELSE b END FROM collate_test1 ORDER BY 2;
SELECT a, CASE b WHEN 'abc' THEN 'abcd' ELSE b END FROM collate_test2 ORDER BY 2;

CREATE DOMAIN testdomain AS text;
SELECT a, b::testdomain FROM collate_test1 ORDER BY 2;
SELECT a, b::testdomain FROM collate_test2 ORDER BY 2;
SELECT a, b::testdomain_p FROM collate_test2 ORDER BY 2;
SELECT a, lower(x::testdomain), lower(y::testdomain) FROM collate_test10;

SELECT min(b), max(b) FROM collate_test1;
SELECT min(b), max(b) FROM collate_test2;

SELECT array_agg(b ORDER BY b) FROM collate_test1;
SELECT array_agg(b ORDER BY b) FROM collate_test2;

-- In aggregates, ORDER BY expressions don't affect aggregate's collation
SELECT string_agg(x COLLATE "C", y COLLATE "POSIX") FROM collate_test10;  -- fail
SELECT array_agg(x COLLATE "C" ORDER BY y COLLATE "POSIX") FROM collate_test10;
SELECT array_agg(a ORDER BY x COLLATE "C", y COLLATE "POSIX") FROM collate_test10;
SELECT array_agg(a ORDER BY x||y) FROM collate_test10;  -- fail

SELECT a, b FROM collate_test1 UNION ALL SELECT a, b FROM collate_test1 ORDER BY 2;
SELECT a, b FROM collate_test2 UNION SELECT a, b FROM collate_test2 ORDER BY 2;
SELECT a, b FROM collate_test2 WHERE a < 4 INTERSECT SELECT a, b FROM collate_test2 WHERE a > 1 ORDER BY 2;
SELECT a, b FROM collate_test2 EXCEPT SELECT a, b FROM collate_test2 WHERE a < 2 ORDER BY 2;

SELECT a, b FROM collate_test1 UNION ALL SELECT a, b FROM collate_test2 ORDER BY 2; -- fail
SELECT a, b FROM collate_test1 UNION ALL SELECT a, b FROM collate_test2; -- ok
SELECT a, b FROM collate_test1 UNION SELECT a, b FROM collate_test2 ORDER BY 2; -- fail
SELECT a, b COLLATE "C" FROM collate_test1 UNION SELECT a, b FROM collate_test2 ORDER BY 2; -- ok
SELECT a, b FROM collate_test1 INTERSECT SELECT a, b FROM collate_test2 ORDER BY 2; -- fail
SELECT a, b FROM collate_test1 EXCEPT SELECT a, b FROM collate_test2 ORDER BY 2; -- fail

CREATE TABLE test_u AS SELECT a, b FROM collate_test1 UNION ALL SELECT a, b FROM collate_test2; -- fail

-- ideally this would be a parse-time error, but for now it must be run-time:
select x < y from collate_test10; -- fail
select x || y from collate_test10; -- ok, because || is not collation aware
select x, y from collate_test10 order by x || y; -- not so ok

-- collation mismatch between recursive and non-recursive term
WITH RECURSIVE foo(x) AS
   (SELECT x FROM (VALUES('a' COLLATE "C"),('b')) t(x)
   UNION ALL
   SELECT (x || 'c') COLLATE "POSIX" FROM foo WHERE length(x) < 10)
SELECT * FROM foo;

SELECT a, b, a < b as lt FROM
  (VALUES ('a', 'B'), ('A', 'b' COLLATE "C")) v(a,b);


-- casting

SELECT CAST('42' AS text COLLATE "C");

SELECT a, CAST(b AS varchar) FROM collate_test1 ORDER BY 2;
SELECT a, CAST(b AS varchar) FROM collate_test2 ORDER BY 2;


-- polymorphism

SELECT * FROM unnest((SELECT array_agg(b ORDER BY b) FROM collate_test1)) ORDER BY 1;
SELECT * FROM unnest((SELECT array_agg(b ORDER BY b) FROM collate_test2)) ORDER BY 1;

CREATE FUNCTION dup (anyelement) RETURNS anyelement
    AS 'select $1' LANGUAGE sql;

SELECT a, dup(b) FROM collate_test1 ORDER BY 2;
SELECT a, dup(b) FROM collate_test2 ORDER BY 2;


-- indexes

CREATE INDEX collate_test1_idx1 ON collate_test1 (b);
CREATE INDEX collate_test1_idx2 ON collate_test1 (b COLLATE "POSIX");
CREATE INDEX collate_test1_idx3 ON collate_test1 ((b COLLATE "POSIX")); -- this is different grammatically
CREATE INDEX collate_test1_idx4 ON collate_test1 (((b||'foo') COLLATE "POSIX"));

CREATE INDEX collate_test1_idx5 ON collate_test1 (a COLLATE "POSIX"); -- fail
CREATE INDEX collate_test1_idx6 ON collate_test1 ((a COLLATE "POSIX")); -- fail

SELECT relname, pg_get_indexdef(oid) FROM pg_class WHERE relname LIKE 'collate_test%_idx%' ORDER BY 1;


-- foreign keys

-- force indexes and mergejoins to be used for FK checking queries,
-- else they might not exercise collation-dependent operators
SET enable_seqscan TO 0;
SET enable_hashjoin TO 0;
SET enable_nestloop TO 0;

CREATE TABLE collate_test20 (f1 text COLLATE "C" PRIMARY KEY);
INSERT INTO collate_test20 VALUES ('foo'), ('bar');
CREATE TABLE collate_test21 (f2 text COLLATE "POSIX" REFERENCES collate_test20);
INSERT INTO collate_test21 VALUES ('foo'), ('bar');
INSERT INTO collate_test21 VALUES ('baz'); -- fail
CREATE TABLE collate_test22 (f2 text COLLATE "POSIX");
INSERT INTO collate_test22 VALUES ('foo'), ('bar'), ('baz');
ALTER TABLE collate_test22 ADD FOREIGN KEY (f2) REFERENCES collate_test20; -- fail
DELETE FROM collate_test22 WHERE f2 = 'baz';
ALTER TABLE collate_test22 ADD FOREIGN KEY (f2) REFERENCES collate_test20;

RESET enable_seqscan;
RESET enable_hashjoin;
RESET enable_nestloop;

-- 9.1 bug with useless COLLATE in an expression subject to length coercion

CREATE TEMP TABLE vctable (f1 varchar(25));
INSERT INTO vctable VALUES ('foo' COLLATE "C");


SELECT collation for ('foo'); -- unknown type - null
SELECT collation for ('foo'::text);
SELECT collation for ((SELECT a FROM collate_test1 LIMIT 1)); -- non-collatable type - error
SELECT collation for ((SELECT b FROM collate_test1 LIMIT 1));


--
-- Clean up.  Many of these table names will be re-used if the user is
-- trying to run any platform-specific collation tests later, so we
-- must get rid of them.
--
DROP SCHEMA collate_tests CASCADE;
