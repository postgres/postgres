-- *************testing built-in type text  ****************

--
-- adt operators in the target list
--
-- fixed-length by reference 
SELECT 'char 16 string'::char16 = 'char 16 string '::char16 AS false;

-- fixed-length by value 
SELECT 'c'::char = 'c'::char AS true;

-- variable-length 
SELECT 'this is a text string'::text = 'this is a text string'::text AS true;

SELECT 'this is a text string'::text = 'this is a text strin'::text AS false;


