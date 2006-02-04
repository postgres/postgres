-- 
-- IF EXISTS tests
-- 

-- table (will be really dropped at the end)

DROP TABLE test_exists;

DROP TABLE IF EXISTS test_exists;

CREATE TABLE test_exists (a int, b text);

-- view

DROP VIEW test_view_exists;

DROP VIEW IF EXISTS test_view_exists;

CREATE VIEW test_view_exists AS select * from test_exists;

DROP VIEW IF EXISTS test_view_exists;

DROP VIEW test_view_exists;

-- index

DROP INDEX test_index_exists;

DROP INDEX IF EXISTS test_index_exists;

CREATE INDEX test_index_exists on test_exists(a);

DROP INDEX IF EXISTS test_index_exists;

DROP INDEX test_index_exists;

-- sequence

DROP SEQUENCE test_sequence_exists;

DROP SEQUENCE IF EXISTS test_sequence_exists;

CREATE SEQUENCE test_sequence_exists;

DROP SEQUENCE IF EXISTS test_sequence_exists;

DROP SEQUENCE test_sequence_exists;

-- schema

DROP SCHEMA test_schema_exists;

DROP SCHEMA IF EXISTS test_schema_exists;

CREATE SCHEMA test_schema_exists;

DROP SCHEMA IF EXISTS test_schema_exists;

DROP SCHEMA test_schema_exists;

-- type

DROP TYPE test_type_exists;

DROP TYPE IF EXISTS test_type_exists;

CREATE type test_type_exists as (a int, b text);

DROP TYPE IF EXISTS test_type_exists;

DROP TYPE test_type_exists;

-- domain

DROP DOMAIN test_domain_exists;

DROP DOMAIN IF EXISTS test_domain_exists;

CREATE domain test_domain_exists as int not null check (value > 0);

DROP DOMAIN IF EXISTS test_domain_exists;

DROP DOMAIN test_domain_exists;

-- drop the table


DROP TABLE IF EXISTS test_exists;

DROP TABLE test_exists;


---
--- role/user/group
---

CREATE USER tu1;
CREATE ROLE tr1;
CREATE GROUP tg1;

DROP USER tu2;

DROP USER IF EXISTS tu1, tu2;

DROP USER tu1;

DROP ROLE tr2;

DROP ROLE IF EXISTS tr1, tr2;

DROP ROLE tr1;

DROP GROUP tg2;

DROP GROUP IF EXISTS tg1, tg2;

DROP GROUP tg1;

