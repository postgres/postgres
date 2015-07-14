CREATE OR REPLACE FUNCTION alter_op_test_fn(boolean, boolean)
RETURNS boolean AS $$ SELECT NULL::BOOLEAN; $$ LANGUAGE sql IMMUTABLE;

CREATE OPERATOR === (
    LEFTARG = boolean,
    RIGHTARG = boolean,
    PROCEDURE = alter_op_test_fn,
    COMMUTATOR = ===,
    NEGATOR = !==,
    RESTRICT = contsel,
    JOIN = contjoinsel,
    HASHES, MERGES
);

--
-- Reset and set params
--

ALTER OPERATOR === (boolean, boolean) SET (RESTRICT = NONE);
ALTER OPERATOR === (boolean, boolean) SET (JOIN = NONE);

SELECT oprrest, oprjoin FROM pg_operator WHERE oprname = '==='
  AND oprleft = 'boolean'::regtype AND oprright = 'boolean'::regtype;

ALTER OPERATOR === (boolean, boolean) SET (RESTRICT = contsel);
ALTER OPERATOR === (boolean, boolean) SET (JOIN = contjoinsel);

SELECT oprrest, oprjoin FROM pg_operator WHERE oprname = '==='
  AND oprleft = 'boolean'::regtype AND oprright = 'boolean'::regtype;

ALTER OPERATOR === (boolean, boolean) SET (RESTRICT = NONE, JOIN = NONE);

SELECT oprrest, oprjoin FROM pg_operator WHERE oprname = '==='
  AND oprleft = 'boolean'::regtype AND oprright = 'boolean'::regtype;

ALTER OPERATOR === (boolean, boolean) SET (RESTRICT = contsel, JOIN = contjoinsel);

SELECT oprrest, oprjoin FROM pg_operator WHERE oprname = '==='
  AND oprleft = 'boolean'::regtype AND oprright = 'boolean'::regtype;

--
-- Test invalid options.
--
ALTER OPERATOR === (boolean, boolean) SET (COMMUTATOR = ====);
ALTER OPERATOR === (boolean, boolean) SET (NEGATOR = ====);
ALTER OPERATOR === (boolean, boolean) SET (RESTRICT = non_existent_func);
ALTER OPERATOR === (boolean, boolean) SET (JOIN = non_existent_func);
ALTER OPERATOR === (boolean, boolean) SET (COMMUTATOR = !==);
ALTER OPERATOR === (boolean, boolean) SET (NEGATOR = !==);


--
-- Test permission check. Must be owner to ALTER OPERATOR.
--
CREATE USER regtest_alter_user;
SET SESSION AUTHORIZATION regtest_alter_user_user;

ALTER OPERATOR === (boolean, boolean) SET (RESTRICT = NONE);

RESET SESSION AUTHORIZATION;

-- Clean up
DROP USER regression_alter_user;
DROP OPERATOR === (boolean, boolean);
