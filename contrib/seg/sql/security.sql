--
--  Test extension script protection against search path overriding
--

CREATE ROLE regress_seg_role;
SELECT current_database() AS datname \gset
GRANT CREATE ON DATABASE :"datname" TO regress_seg_role;
SET ROLE regress_seg_role;
CREATE SCHEMA regress_seg_schema;

CREATE FUNCTION regress_seg_schema.exfun(i int) RETURNS int AS $$
BEGIN
  CREATE EXTENSION seg VERSION '1.2';

  CREATE FUNCTION regress_seg_schema.compare(oid, regclass) RETURNS boolean AS
  'BEGIN RAISE EXCEPTION ''overloaded compare() called by %'', current_user; END;' LANGUAGE plpgsql;

  CREATE OPERATOR = (LEFTARG = oid, RIGHTARG = regclass, PROCEDURE = regress_seg_schema.compare);

  ALTER EXTENSION seg UPDATE TO '1.3';

  RETURN i;
END; $$ LANGUAGE plpgsql;

CREATE SCHEMA test_schema
CREATE TABLE t(i int) PARTITION BY RANGE (i)
CREATE TABLE p1 PARTITION OF t FOR VALUES FROM (1) TO (regress_seg_schema.exfun(2));

DROP SCHEMA test_schema CASCADE;
RESET ROLE;
DROP OWNED BY regress_seg_role;
DROP ROLE regress_seg_role;
