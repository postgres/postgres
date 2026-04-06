--
-- CREATE_SCHEMA
--

-- Schema creation with elements.

CREATE ROLE regress_create_schema_role SUPERUSER;

-- Cases where schema creation fails as objects are qualified with a schema
-- that does not match with what's expected.
-- This checks most object types that include schema qualifications.
CREATE SCHEMA AUTHORIZATION regress_create_schema_role
  CREATE SEQUENCE schema_not_existing.seq;
CREATE SCHEMA AUTHORIZATION regress_create_schema_role
  CREATE TABLE schema_not_existing.tab (id int);
CREATE SCHEMA AUTHORIZATION regress_create_schema_role
  CREATE VIEW schema_not_existing.view AS SELECT 1;
CREATE SCHEMA AUTHORIZATION regress_create_schema_role
  CREATE INDEX ON schema_not_existing.tab (id);
CREATE SCHEMA AUTHORIZATION regress_create_schema_role
  CREATE TRIGGER schema_trig BEFORE INSERT ON schema_not_existing.tab
  EXECUTE FUNCTION schema_trig.no_func();
CREATE SCHEMA AUTHORIZATION regress_create_schema_role
  CREATE FUNCTION schema_not_existing.func(int) RETURNS int
  AS 'SELECT $1' LANGUAGE sql;
-- Again, with a role specification and no schema names.
SET ROLE regress_create_schema_role;
CREATE SCHEMA AUTHORIZATION CURRENT_ROLE
  CREATE SEQUENCE schema_not_existing.seq;
CREATE SCHEMA AUTHORIZATION CURRENT_ROLE
  CREATE TABLE schema_not_existing.tab (id int);
CREATE SCHEMA AUTHORIZATION CURRENT_ROLE
  CREATE VIEW schema_not_existing.view AS SELECT 1;
CREATE SCHEMA AUTHORIZATION CURRENT_ROLE
  CREATE INDEX ON schema_not_existing.tab (id);
CREATE SCHEMA AUTHORIZATION CURRENT_ROLE
  CREATE TRIGGER schema_trig BEFORE INSERT ON schema_not_existing.tab
  EXECUTE FUNCTION schema_trig.no_func();
-- Again, with a schema name and a role specification.
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE SEQUENCE schema_not_existing.seq;
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE TABLE schema_not_existing.tab (id int);
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE VIEW schema_not_existing.view AS SELECT 1;
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE INDEX ON schema_not_existing.tab (id);
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE TRIGGER schema_trig BEFORE INSERT ON schema_not_existing.tab
  EXECUTE FUNCTION schema_trig.no_func();
RESET ROLE;

-- Forward references no longer work in general.
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE VIEW abcd_view AS SELECT a FROM abcd
  CREATE TABLE abcd (a int);

-- Cases where the schema creation succeeds.
-- The schema created matches the role name.
CREATE SCHEMA AUTHORIZATION regress_create_schema_role
  CREATE TABLE regress_create_schema_role.tab (id int);
\d regress_create_schema_role.tab
DROP SCHEMA regress_create_schema_role CASCADE;
-- Again, with a different role specification and no schema names.
SET ROLE regress_create_schema_role;
CREATE SCHEMA AUTHORIZATION CURRENT_ROLE
  CREATE TABLE regress_create_schema_role.tab (id int);
\d regress_create_schema_role.tab
DROP SCHEMA regress_create_schema_role CASCADE;
-- Again, with a schema name and a role specification.
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE TABLE regress_schema_1.tab (id int);
\d regress_schema_1.tab
DROP SCHEMA regress_schema_1 CASCADE;
RESET ROLE;

-- Test forward-referencing foreign key clauses.
CREATE SCHEMA regress_schema_fk
    CREATE TABLE regress_schema_fk.t2 (
        b int,
        a int REFERENCES t1 DEFERRABLE INITIALLY DEFERRED NOT ENFORCED
              REFERENCES t3 DEFERRABLE INITIALLY DEFERRED,
        CONSTRAINT fk FOREIGN KEY (a) REFERENCES t1 NOT DEFERRABLE)

    CREATE TABLE regress_schema_fk.t1 (a int PRIMARY KEY)

    CREATE TABLE t3 (a int PRIMARY KEY)

    CREATE TABLE t4 (
        b int,
        a int REFERENCES t5 NOT DEFERRABLE ENFORCED
              REFERENCES t6 DEFERRABLE INITIALLY IMMEDIATE,
        CONSTRAINT fk FOREIGN KEY (a) REFERENCES t6 DEFERRABLE INITIALLY DEFERRED)

    CREATE TABLE t5 (a int, b int, PRIMARY KEY (a))

    CREATE TABLE t6 (a int, b int, PRIMARY KEY (a));

\d regress_schema_fk.t2
\d regress_schema_fk.t4

DROP SCHEMA regress_schema_fk CASCADE;

-- Test miscellaneous object types within CREATE SCHEMA.
CREATE SCHEMA regress_schema_misc
  CREATE AGGREGATE cs_sum(int4)
    (
        SFUNC = int4_sum(int8, int4),
        STYPE = int8,
        INITCOND = '0'
    )

  CREATE COLLATION cs_builtin_c ( PROVIDER = builtin, LOCALE = "C" )

  CREATE DOMAIN cs_positive AS integer CHECK (VALUE > 0)

  CREATE FUNCTION cs_add(int4, int4) returns int4 language sql
    as 'select $1 + $2'

  CREATE OPERATOR + (function = cs_add, leftarg = int4, rightarg = int4)

  CREATE PROCEDURE cs_proc(int4, int4)
    BEGIN ATOMIC SELECT cs_add($1,$2); END

  CREATE TEXT SEARCH CONFIGURATION cs_ts_conf (copy=english)

  CREATE TEXT SEARCH DICTIONARY cs_ts_dict (template=simple)

  CREATE TEXT SEARCH PARSER cs_ts_prs
    (start = prsd_start, gettoken = prsd_nexttoken, end = prsd_end,
     lextypes = prsd_lextype)

  CREATE TEXT SEARCH TEMPLATE cs_ts_temp (lexize=dsimple_lexize)

  CREATE TYPE regress_schema_misc.cs_enum AS ENUM ('red', 'orange')

  CREATE TYPE cs_composite AS (a int, b float8)

  CREATE TYPE cs_range AS RANGE (subtype = float8, subtype_diff = float8mi)

  -- demonstrate creation of a base type with its I/O functions

  CREATE TYPE cs_type

  CREATE FUNCTION cs_type_in(cstring)
    RETURNS cs_type LANGUAGE internal IMMUTABLE PARALLEL SAFE STRICT
    AS 'int4in'

  CREATE FUNCTION cs_type_out(cs_type)
    RETURNS cstring LANGUAGE internal IMMUTABLE PARALLEL SAFE STRICT
    AS 'int4out'

  CREATE TYPE cs_type (
    INPUT = cs_type_in,
    OUTPUT = cs_type_out,
    LIKE = int4
  )

  GRANT USAGE ON TYPE cs_type TO public
;

\df regress_schema_misc.cs_add
\df regress_schema_misc.cs_proc
\da regress_schema_misc.cs_sum
\do regress_schema_misc.+
\dO regress_schema_misc.*
\dT regress_schema_misc.*
\dF regress_schema_misc.*
\dFd regress_schema_misc.*
\dFp regress_schema_misc.*
\dFt regress_schema_misc.*

DROP SCHEMA regress_schema_misc CASCADE;

-- Clean up
DROP ROLE regress_create_schema_role;
