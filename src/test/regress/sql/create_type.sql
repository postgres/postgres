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

