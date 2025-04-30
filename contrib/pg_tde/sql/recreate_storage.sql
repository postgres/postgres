CREATE EXTENSION IF NOT EXISTS pg_tde;

SELECT pg_tde_add_database_key_provider_file('file-vault','/tmp/pg_tde_test_keyring.per');
SELECT pg_tde_set_key_using_database_key_provider('test-db-key','file-vault');

SET default_table_access_method = "tde_heap";

CREATE TABLE t1(n integer);
SELECT pg_tde_is_encrypted('t1');
VACUUM FULL t1;
SELECT pg_tde_is_encrypted('t1');

CREATE TABLE test_tab1 AS SELECT generate_series(1,10) a;
CREATE INDEX test_idx1 ON test_tab1(a);
SELECT pg_tde_is_encrypted('test_tab1');
SELECT pg_tde_is_encrypted('test_idx1');
REINDEX index CONCURRENTLY test_idx1;
SELECT pg_tde_is_encrypted('test_tab1');
SELECT pg_tde_is_encrypted('test_idx1');

CREATE TABLE mvtest_t (id int NOT NULL PRIMARY KEY, type text NOT NULL, amt numeric NOT NULL);
INSERT INTO mvtest_t VALUES
  (1, 'x', 2),
  (2, 'x', 3),
  (3, 'y', 5),
  (4, 'y', 7),
  (5, 'z', 11);
CREATE MATERIALIZED VIEW mvtest_tm AS SELECT type, sum(amt) AS totamt FROM mvtest_t GROUP BY type WITH NO DATA;
SELECT pg_tde_is_encrypted('mvtest_tm');
REFRESH MATERIALIZED VIEW mvtest_tm;
SELECT pg_tde_is_encrypted('mvtest_tm');

CREATE TYPE rewritetype AS (a int);
CREATE TABLE rewritemetoo1 OF rewritetype;
CREATE TABLE rewritemetoo2 OF rewritetype;
SELECT pg_tde_is_encrypted('rewritemetoo1');
SELECT pg_tde_is_encrypted('rewritemetoo2');
ALTER TYPE rewritetype ALTER ATTRIBUTE a TYPE text cascade;
SELECT pg_tde_is_encrypted('rewritemetoo1');
SELECT pg_tde_is_encrypted('rewritemetoo2');

CREATE TABLE encrypted_table (
    id SERIAL,
    id2 INT,
    data TEXT,
    created_at DATE NOT NULL,
    PRIMARY KEY (id, created_at)
) USING tde_heap;

CREATE INDEX idx_date ON encrypted_table (created_at);
SELECT pg_tde_is_encrypted('encrypted_table');
CLUSTER encrypted_table USING idx_date;
SELECT pg_tde_is_encrypted('encrypted_table');

SELECT pg_tde_is_encrypted('encrypted_table_id_seq');
ALTER SEQUENCE encrypted_table_id_seq RESTART;
SELECT pg_tde_is_encrypted('encrypted_table_id_seq');

CREATE TABLE plain_table (
    id2 INT
) USING heap;

-- Starts independent and becomes encrypted
CREATE SEQUENCE independent_seq;
SELECT pg_tde_is_encrypted('independent_seq');
ALTER SEQUENCE independent_seq OWNED BY encrypted_table.id2;
SELECT pg_tde_is_encrypted('independent_seq');

-- Starts independent and stays plain
CREATE SEQUENCE independent_seq2 OWNED BY NONE;
SELECT pg_tde_is_encrypted('independent_seq2');
ALTER SEQUENCE independent_seq2 OWNED BY plain_table.id2;
SELECT pg_tde_is_encrypted('independent_seq2');

-- Starts owned by an encrypted table and becomes owned by a plain table
CREATE SEQUENCE encrypted_table_id2_seq OWNED BY encrypted_table.id2;
SELECT pg_tde_is_encrypted('encrypted_table_id2_seq');
ALTER SEQUENCE encrypted_table_id2_seq OWNED BY plain_table.id2;
SELECT pg_tde_is_encrypted('encrypted_table_id2_seq');

-- Starts owned by an encrypted table and becomes independent
CREATE SEQUENCE encrypted_table_id2_seq2 OWNED BY encrypted_table.id2;
SELECT pg_tde_is_encrypted('encrypted_table_id2_seq2');
ALTER SEQUENCE encrypted_table_id2_seq2 OWNED BY NONE;
SELECT pg_tde_is_encrypted('encrypted_table_id2_seq2');

-- Starts owned by a plain table and becomes owned by an encrypted table
CREATE SEQUENCE plain_table_id2_seq OWNED BY plain_table.id2;
SELECT pg_tde_is_encrypted('plain_table_id2_seq');
ALTER SEQUENCE plain_table_id2_seq OWNED BY encrypted_table.id2;
SELECT pg_tde_is_encrypted('plain_table_id2_seq');

-- Starts owned by a plain table and becomes independent
CREATE SEQUENCE plain_table_id2_seq2 OWNED BY plain_table.id2;
SELECT pg_tde_is_encrypted('plain_table_id2_seq2');
ALTER SEQUENCE plain_table_id2_seq2 OWNED BY NONE;
SELECT pg_tde_is_encrypted('plain_table_id2_seq2');

-- Enforce that we do not mess up encryption status for toast table
CREATE TABLE cities (
  name       varchar(8),
  population real,
  elevation  int
) USING tde_heap;

CREATE TABLE state_capitals (
  state      char(2) UNIQUE NOT NULL
) INHERITS (cities) USING heap;

CREATE TABLE capitals (
  country      char(2) UNIQUE NOT NULL
) INHERITS (cities) USING tde_heap;

ALTER TABLE cities ALTER COLUMN name TYPE TEXT;

-- Enforce the same for typed tables
CREATE TYPE people_type AS (age int, name varchar(8), dob date);
CREATE TABLE sales_staff OF people_type USING tde_heap;
CREATE TABLE other_staff OF people_type USING heap;

ALTER TYPE people_type ALTER ATTRIBUTE name TYPE text CASCADE;

-- If all tpyed tables are encrypted everything should work as usual
ALTER TABLE other_staff SET ACCESS METHOD tde_heap;
ALTER TYPE people_type ALTER ATTRIBUTE name TYPE text CASCADE;

SELECT pg_tde_is_encrypted('pg_toast.pg_toast_' || 'sales_staff'::regclass::oid);
SELECT pg_tde_is_encrypted('pg_toast.pg_toast_' || 'other_staff'::regclass::oid);

DROP TYPE people_type CASCADE;
DROP TABLE cities CASCADE;
DROP TABLE plain_table;
DROP EXTENSION pg_tde CASCADE;
RESET default_table_access_method;
