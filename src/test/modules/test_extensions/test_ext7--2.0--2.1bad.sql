/* src/test/modules/test_extensions/test_ext7--2.0--2.1bad.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION test_ext7 UPDATE TO '2.1bad'" to load this file. \quit

-- test reporting of a simple syntax error in an extension script
CREATE FUNCTION my_okay_func(int) RETURNS int LANGUAGE SQL
AS $$ SELECT $1 + 1 $$;

CREATE FUNCTIN my_erroneous_func(int) RETURNS int LANGUAGE SQL
AS $$ SELECT $1 + 1 $$;

CREATE FUNCTION my_other_func(int) RETURNS int LANGUAGE SQL
AS $$ SELECT $1 + 1 $$;
