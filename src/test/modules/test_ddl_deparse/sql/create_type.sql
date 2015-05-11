---
--- CREATE_TYPE
---

CREATE FUNCTION text_w_default_in(cstring)
   RETURNS text_w_default
   AS 'textin'
   LANGUAGE internal STABLE STRICT;

CREATE FUNCTION text_w_default_out(text_w_default)
   RETURNS cstring
   AS 'textout'
   LANGUAGE internal STABLE STRICT ;

CREATE TYPE employee_type AS (name TEXT, salary NUMERIC);

CREATE TYPE enum_test AS ENUM ('foo', 'bar', 'baz');

CREATE TYPE int2range AS RANGE (
  SUBTYPE = int2
);
