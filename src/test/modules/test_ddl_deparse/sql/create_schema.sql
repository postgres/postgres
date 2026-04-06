--
-- CREATE_SCHEMA
--

CREATE SCHEMA foo;

CREATE SCHEMA IF NOT EXISTS bar;

CREATE SCHEMA baz;

-- Will not be created, and will not be handled by the
-- event trigger
CREATE SCHEMA IF NOT EXISTS baz;

CREATE SCHEMA element_test
  CREATE TABLE foo (id int)
  CREATE VIEW bar AS SELECT * FROM foo
  CREATE COLLATION coll (LOCALE="C")
  CREATE DOMAIN d1 AS INT
  CREATE FUNCTION et_add(int4, int4) RETURNS int4 LANGUAGE sql
    AS 'SELECT $1 + $2'
  CREATE PROCEDURE et_proc(int4, int4)
    BEGIN ATOMIC SELECT et_add($1,$2); END
  CREATE TYPE floatrange AS RANGE (subtype = float8, subtype_diff = float8mi)
  CREATE TYPE ss AS (a int)
  CREATE TYPE sss
  CREATE TYPE rainbow AS ENUM ('red', 'orange')
  CREATE TEXT SEARCH PARSER et_ts_prs
    (start = prsd_start, gettoken = prsd_nexttoken, end = prsd_end,
     lextypes = prsd_lextype)
;

DROP SCHEMA element_test CASCADE;

CREATE SCHEMA regress_schema_1
CREATE TABLE t4(
    b INT,
    a INT REFERENCES t5 DEFERRABLE INITIALLY DEFERRED NOT ENFORCED
          REFERENCES t6 DEFERRABLE INITIALLY DEFERRED,
    CONSTRAINT fk FOREIGN KEY (a) REFERENCES t6 DEFERRABLE)
CREATE TABLE t5 (a INT, b INT, PRIMARY KEY (a))
CREATE TABLE t6 (a INT, b INT, PRIMARY KEY (a));

DROP SCHEMA regress_schema_1 CASCADE;
