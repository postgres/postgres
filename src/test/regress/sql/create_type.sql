--
-- CREATE_TYPE
--

CREATE TYPE widget (
   internallength = 24, 
   input = widget_in,
   output = widget_out,
   alignment = double
);

CREATE TYPE city_budget ( 
   internallength = 16, 
   input = int44in, 
   output = int44out, 
   element = int4
);

-- Test type-related default values (broken in releases before PG 7.2)

-- Make dummy I/O routines using the existing internal support for int4, text
CREATE FUNCTION int42_in(cstring)
   RETURNS int42
   AS 'int4in'
   LANGUAGE 'internal' WITH (isStrict);
CREATE FUNCTION int42_out(int42)
   RETURNS cstring
   AS 'int4out'
   LANGUAGE 'internal' WITH (isStrict);
CREATE FUNCTION text_w_default_in(cstring)
   RETURNS text_w_default
   AS 'textin'
   LANGUAGE 'internal' WITH (isStrict);
CREATE FUNCTION text_w_default_out(text_w_default)
   RETURNS cstring
   AS 'textout'
   LANGUAGE 'internal' WITH (isStrict);

CREATE TYPE int42 (
   internallength = 4,
   input = int42_in,
   output = int42_out,
   alignment = int4,
   default = 42,
   passedbyvalue
);

CREATE TYPE text_w_default (
   internallength = variable,
   input = text_w_default_in,
   output = text_w_default_out,
   alignment = int4,
   default = 'zippo'
);

CREATE TABLE default_test (f1 text_w_default, f2 int42);

INSERT INTO default_test DEFAULT VALUES;

SELECT * FROM default_test;

-- Test stand-alone composite type

CREATE TYPE default_test_row AS (f1 text_w_default, f2 int42);

CREATE FUNCTION get_default_test() RETURNS SETOF default_test_row AS '
  SELECT * FROM default_test;
' LANGUAGE SQL;

SELECT * FROM get_default_test();

DROP TYPE default_test_row CASCADE;

DROP TABLE default_test;
