--
-- ALTER TABLE ADD COLUMN DEFAULT test
--

SET search_path = fast_default;
CREATE SCHEMA fast_default;
CREATE TABLE m(id OID);
INSERT INTO m VALUES (NULL::OID);

CREATE FUNCTION set(tabname name) RETURNS VOID
AS $$
BEGIN
  UPDATE m
  SET id = (SELECT c.relfilenode
            FROM pg_class AS c, pg_namespace AS s
            WHERE c.relname = tabname
                AND c.relnamespace = s.oid
                AND s.nspname = 'fast_default');
END;
$$ LANGUAGE 'plpgsql';

CREATE FUNCTION comp() RETURNS TEXT
AS $$
BEGIN
  RETURN (SELECT CASE
               WHEN m.id = c.relfilenode THEN 'Unchanged'
               ELSE 'Rewritten'
               END
           FROM m, pg_class AS c, pg_namespace AS s
           WHERE c.relname = 't'
               AND c.relnamespace = s.oid
               AND s.nspname = 'fast_default');
END;
$$ LANGUAGE 'plpgsql';

CREATE FUNCTION log_rewrite() RETURNS event_trigger
LANGUAGE plpgsql as
$func$

declare
   this_schema text;
begin
    select into this_schema relnamespace::regnamespace::text
    from pg_class
    where oid = pg_event_trigger_table_rewrite_oid();
    if this_schema = 'fast_default'
    then
        RAISE NOTICE 'rewriting table % for reason %',
          pg_event_trigger_table_rewrite_oid()::regclass,
          pg_event_trigger_table_rewrite_reason();
    end if;
end;
$func$;

CREATE TABLE has_volatile AS
SELECT * FROM generate_series(1,10) id;


CREATE EVENT TRIGGER has_volatile_rewrite
                  ON table_rewrite
   EXECUTE PROCEDURE log_rewrite();

-- only the last of these should trigger a rewrite
ALTER TABLE has_volatile ADD col1 int;
ALTER TABLE has_volatile ADD col2 int DEFAULT 1;
ALTER TABLE has_volatile ADD col3 timestamptz DEFAULT current_timestamp;
ALTER TABLE has_volatile ADD col4 int DEFAULT (random() * 10000)::int;



-- Test a large sample of different datatypes
CREATE TABLE T(pk INT NOT NULL PRIMARY KEY, c_int INT DEFAULT 1);

SELECT set('t');

INSERT INTO T VALUES (1), (2);

ALTER TABLE T ADD COLUMN c_bpchar BPCHAR(5) DEFAULT 'hello',
              ALTER COLUMN c_int SET DEFAULT 2;

INSERT INTO T VALUES (3), (4);


ALTER TABLE T ADD COLUMN c_text TEXT  DEFAULT 'world',
              ALTER COLUMN c_bpchar SET DEFAULT 'dog';

INSERT INTO T VALUES (5), (6);

ALTER TABLE T ADD COLUMN c_date DATE DEFAULT '2016-06-02',
              ALTER COLUMN c_text SET DEFAULT 'cat';

INSERT INTO T VALUES (7), (8);

ALTER TABLE T ADD COLUMN c_timestamp TIMESTAMP DEFAULT '2016-09-01 12:00:00',
              ADD COLUMN c_timestamp_null TIMESTAMP,
              ALTER COLUMN c_date SET DEFAULT '2010-01-01';

INSERT INTO T VALUES (9), (10);

ALTER TABLE T ADD COLUMN c_array TEXT[]
                  DEFAULT '{"This", "is", "the", "real", "world"}',
              ALTER COLUMN c_timestamp SET DEFAULT '1970-12-31 11:12:13',
              ALTER COLUMN c_timestamp_null SET DEFAULT '2016-09-29 12:00:00';

INSERT INTO T VALUES (11), (12);

ALTER TABLE T ADD COLUMN c_small SMALLINT DEFAULT -5,
              ADD COLUMN c_small_null SMALLINT,
              ALTER COLUMN c_array
                  SET DEFAULT '{"This", "is", "no", "fantasy"}';

INSERT INTO T VALUES (13), (14);

ALTER TABLE T ADD COLUMN c_big BIGINT DEFAULT 180000000000018,
              ALTER COLUMN c_small SET DEFAULT 9,
              ALTER COLUMN c_small_null SET DEFAULT 13;

INSERT INTO T VALUES (15), (16);

ALTER TABLE T ADD COLUMN c_num NUMERIC DEFAULT 1.00000000001,
              ALTER COLUMN c_big SET DEFAULT -9999999999999999;

INSERT INTO T VALUES (17), (18);

ALTER TABLE T ADD COLUMN c_time TIME DEFAULT '12:00:00',
              ALTER COLUMN c_num SET DEFAULT 2.000000000000002;

INSERT INTO T VALUES (19), (20);

ALTER TABLE T ADD COLUMN c_interval INTERVAL DEFAULT '1 day',
              ALTER COLUMN c_time SET DEFAULT '23:59:59';

INSERT INTO T VALUES (21), (22);

ALTER TABLE T ADD COLUMN c_hugetext TEXT DEFAULT repeat('abcdefg',1000),
              ALTER COLUMN c_interval SET DEFAULT '3 hours';

INSERT INTO T VALUES (23), (24);

ALTER TABLE T ALTER COLUMN c_interval DROP DEFAULT,
              ALTER COLUMN c_hugetext SET DEFAULT repeat('poiuyt', 1000);

INSERT INTO T VALUES (25), (26);

ALTER TABLE T ALTER COLUMN c_bpchar    DROP DEFAULT,
              ALTER COLUMN c_date      DROP DEFAULT,
              ALTER COLUMN c_text      DROP DEFAULT,
              ALTER COLUMN c_timestamp DROP DEFAULT,
              ALTER COLUMN c_array     DROP DEFAULT,
              ALTER COLUMN c_small     DROP DEFAULT,
              ALTER COLUMN c_big       DROP DEFAULT,
              ALTER COLUMN c_num       DROP DEFAULT,
              ALTER COLUMN c_time      DROP DEFAULT,
              ALTER COLUMN c_hugetext  DROP DEFAULT;

INSERT INTO T VALUES (27), (28);

SELECT pk, c_int, c_bpchar, c_text, c_date, c_timestamp,
       c_timestamp_null, c_array, c_small, c_small_null,
       c_big, c_num, c_time, c_interval,
       c_hugetext = repeat('abcdefg',1000) as c_hugetext_origdef,
       c_hugetext = repeat('poiuyt', 1000) as c_hugetext_newdef
FROM T ORDER BY pk;

SELECT comp();

DROP TABLE T;

-- Test expressions in the defaults
CREATE OR REPLACE FUNCTION foo(a INT) RETURNS TEXT AS $$
DECLARE res TEXT := '';
        i INT;
BEGIN
  i := 0;
  WHILE (i < a) LOOP
    res := res || chr(ascii('a') + i);
    i := i + 1;
  END LOOP;
  RETURN res;
END; $$ LANGUAGE PLPGSQL STABLE;

CREATE TABLE T(pk INT NOT NULL PRIMARY KEY, c_int INT DEFAULT LENGTH(foo(6)));

SELECT set('t');

INSERT INTO T VALUES (1), (2);

ALTER TABLE T ADD COLUMN c_bpchar BPCHAR(5) DEFAULT foo(4),
              ALTER COLUMN c_int SET DEFAULT LENGTH(foo(8));

INSERT INTO T VALUES (3), (4);

ALTER TABLE T ADD COLUMN c_text TEXT  DEFAULT foo(6),
              ALTER COLUMN c_bpchar SET DEFAULT foo(3);

INSERT INTO T VALUES (5), (6);

ALTER TABLE T ADD COLUMN c_date DATE
                  DEFAULT '2016-06-02'::DATE  + LENGTH(foo(10)),
              ALTER COLUMN c_text SET DEFAULT foo(12);

INSERT INTO T VALUES (7), (8);

ALTER TABLE T ADD COLUMN c_timestamp TIMESTAMP
                  DEFAULT '2016-09-01'::DATE + LENGTH(foo(10)),
              ALTER COLUMN c_date
                  SET DEFAULT '2010-01-01'::DATE - LENGTH(foo(4));

INSERT INTO T VALUES (9), (10);

ALTER TABLE T ADD COLUMN c_array TEXT[]
                  DEFAULT ('{"This", "is", "' || foo(4) ||
                           '","the", "real", "world"}')::TEXT[],
              ALTER COLUMN c_timestamp
                  SET DEFAULT '1970-12-31'::DATE + LENGTH(foo(30));

INSERT INTO T VALUES (11), (12);

ALTER TABLE T ALTER COLUMN c_int DROP DEFAULT,
              ALTER COLUMN c_array
                  SET DEFAULT ('{"This", "is", "' || foo(1) ||
                               '", "fantasy"}')::text[];

INSERT INTO T VALUES (13), (14);

ALTER TABLE T ALTER COLUMN c_bpchar    DROP DEFAULT,
              ALTER COLUMN c_date      DROP DEFAULT,
              ALTER COLUMN c_text      DROP DEFAULT,
              ALTER COLUMN c_timestamp DROP DEFAULT,
              ALTER COLUMN c_array     DROP DEFAULT;

INSERT INTO T VALUES (15), (16);

SELECT * FROM T;

SELECT comp();

DROP TABLE T;

DROP FUNCTION foo(INT);

-- Fall back to full rewrite for volatile expressions
CREATE TABLE T(pk INT NOT NULL PRIMARY KEY);

INSERT INTO T VALUES (1);

SELECT set('t');

-- now() is stable, because it returns the transaction timestamp
ALTER TABLE T ADD COLUMN c1 TIMESTAMP DEFAULT now();

SELECT comp();

-- clock_timestamp() is volatile
ALTER TABLE T ADD COLUMN c2 TIMESTAMP DEFAULT clock_timestamp();

SELECT comp();

DROP TABLE T;

-- Simple querie
CREATE TABLE T (pk INT NOT NULL PRIMARY KEY);

SELECT set('t');

INSERT INTO T SELECT * FROM generate_series(1, 10) a;

ALTER TABLE T ADD COLUMN c_bigint BIGINT NOT NULL DEFAULT -1;

INSERT INTO T SELECT b, b - 10 FROM generate_series(11, 20) a(b);

ALTER TABLE T ADD COLUMN c_text TEXT DEFAULT 'hello';

INSERT INTO T SELECT b, b - 10, (b + 10)::text FROM generate_series(21, 30) a(b);

-- WHERE clause
SELECT c_bigint, c_text FROM T WHERE c_bigint = -1 LIMIT 1;

EXPLAIN (VERBOSE TRUE, COSTS FALSE)
SELECT c_bigint, c_text FROM T WHERE c_bigint = -1 LIMIT 1;

SELECT c_bigint, c_text FROM T WHERE c_text = 'hello' LIMIT 1;

EXPLAIN (VERBOSE TRUE, COSTS FALSE) SELECT c_bigint, c_text FROM T WHERE c_text = 'hello' LIMIT 1;


-- COALESCE
SELECT COALESCE(c_bigint, pk), COALESCE(c_text, pk::text)
FROM T
ORDER BY pk LIMIT 10;

-- Aggregate function
SELECT SUM(c_bigint), MAX(c_text COLLATE "C" ), MIN(c_text COLLATE "C") FROM T;

-- ORDER BY
SELECT * FROM T ORDER BY c_bigint, c_text, pk LIMIT 10;

EXPLAIN (VERBOSE TRUE, COSTS FALSE)
SELECT * FROM T ORDER BY c_bigint, c_text, pk LIMIT 10;

-- LIMIT
SELECT * FROM T WHERE c_bigint > -1 ORDER BY c_bigint, c_text, pk LIMIT 10;

EXPLAIN (VERBOSE TRUE, COSTS FALSE)
SELECT * FROM T WHERE c_bigint > -1 ORDER BY c_bigint, c_text, pk LIMIT 10;

--  DELETE with RETURNING
DELETE FROM T WHERE pk BETWEEN 10 AND 20 RETURNING *;
EXPLAIN (VERBOSE TRUE, COSTS FALSE)
DELETE FROM T WHERE pk BETWEEN 10 AND 20 RETURNING *;

-- UPDATE
UPDATE T SET c_text = '"' || c_text || '"'  WHERE pk < 10;
SELECT * FROM T WHERE c_text LIKE '"%"' ORDER BY PK;

SELECT comp();

DROP TABLE T;


-- Combine with other DDL
CREATE TABLE T(pk INT NOT NULL PRIMARY KEY);

SELECT set('t');

INSERT INTO T VALUES (1), (2);

ALTER TABLE T ADD COLUMN c_int INT NOT NULL DEFAULT -1;

INSERT INTO T VALUES (3), (4);

ALTER TABLE T ADD COLUMN c_text TEXT DEFAULT 'Hello';

INSERT INTO T VALUES (5), (6);

ALTER TABLE T ALTER COLUMN c_text SET DEFAULT 'world',
              ALTER COLUMN c_int  SET DEFAULT 1;

INSERT INTO T VALUES (7), (8);

SELECT * FROM T ORDER BY pk;

-- Add an index
CREATE INDEX i ON T(c_int, c_text);

SELECT c_text FROM T WHERE c_int = -1;

SELECT comp();

-- query to exercise expand_tuple function
CREATE TABLE t1 AS
SELECT 1::int AS a , 2::int AS b
FROM generate_series(1,20) q;

ALTER TABLE t1 ADD COLUMN c text;

SELECT a,
       stddev(cast((SELECT sum(1) FROM generate_series(1,20) x) AS float4))
          OVER (PARTITION BY a,b,c ORDER BY b)
       AS z
FROM t1;

DROP TABLE t1;
DROP TABLE T;
DROP FUNCTION set(name);
DROP FUNCTION comp();
DROP TABLE m;
DROP TABLE has_volatile;
DROP EVENT TRIGGER has_volatile_rewrite;
DROP FUNCTION log_rewrite;
DROP SCHEMA fast_default;

-- Leave a table with an active fast default in place, for pg_upgrade testing
set search_path = public;
create table has_fast_default(f1 int);
insert into has_fast_default values(1);
alter table has_fast_default add column f2 int default 42;
table has_fast_default;
