set enable_seqscan=off;

CREATE TYPE rainbow AS ENUM ('r','o','y','g','b','i','v');

CREATE TABLE test_enum (
   i rainbow
);

INSERT INTO test_enum VALUES ('v'),('y'),('r'),('g'),('o'),('i'),('b');

CREATE INDEX idx_enum ON test_enum USING gin (i);

SELECT * FROM test_enum WHERE i<'g'::rainbow ORDER BY i;
SELECT * FROM test_enum WHERE i<='g'::rainbow ORDER BY i;
SELECT * FROM test_enum WHERE i='g'::rainbow ORDER BY i;
SELECT * FROM test_enum WHERE i>='g'::rainbow ORDER BY i;
SELECT * FROM test_enum WHERE i>'g'::rainbow ORDER BY i;

explain (costs off) SELECT * FROM test_enum WHERE i>='g'::rainbow ORDER BY i;


-- make sure we handle the non-evenly-numbered oid case for enums
create type e as enum ('0', '2', '3');
alter type e add value '1' after '0';
create table t as select (i % 4)::text::e from generate_series(0, 100000) as i;
create index on t using gin (e);
