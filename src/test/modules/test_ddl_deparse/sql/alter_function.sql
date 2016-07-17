--
-- ALTER_FUNCTION
--

ALTER FUNCTION plpgsql_function_trigger_1 ()
   SET SCHEMA foo;

ALTER FUNCTION foo.plpgsql_function_trigger_1()
  COST 10;

CREATE ROLE regress_alter_function_role;

ALTER FUNCTION plpgsql_function_trigger_2()
  OWNER TO regress_alter_function_role;

DROP OWNED BY regress_alter_function_role;
DROP ROLE regress_alter_function_role;
