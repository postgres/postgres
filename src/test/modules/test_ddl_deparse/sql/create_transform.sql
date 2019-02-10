--
-- CREATE_TRANSFORM
--

-- Create a dummy transform
-- The function FROM SQL should have internal as single argument as well
-- as return type. The function TO SQL should have as single argument
-- internal and as return argument the datatype of the transform done.
-- We choose some random built-in functions that have the right signature.
-- This won't actually be used, because the SQL function language
-- doesn't implement transforms (there would be no point).
CREATE TRANSFORM FOR int LANGUAGE SQL (
    FROM SQL WITH FUNCTION prsd_lextype(internal),
    TO SQL WITH FUNCTION int4recv(internal));

DROP TRANSFORM FOR int LANGUAGE SQL;
