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

CREATE TYPE int42 (
   internallength = 4,
   input = int4in,
   output = int4out,
   alignment = int4,
   default = 42,
   passedbyvalue
);

CREATE TYPE text_w_default (
   internallength = variable,
   input = textin,
   output = textout,
   alignment = int4,
   default = 'zippo'
);

CREATE TABLE default_test (f1 text_w_default, f2 int42);

INSERT INTO default_test DEFAULT VALUES;

SELECT * FROM default_test;

DROP TABLE default_test;
