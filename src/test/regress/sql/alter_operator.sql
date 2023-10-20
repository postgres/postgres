CREATE FUNCTION alter_op_test_fn(boolean, boolean)
RETURNS boolean AS $$ SELECT NULL::BOOLEAN; $$ LANGUAGE sql IMMUTABLE;

CREATE FUNCTION customcontsel(internal, oid, internal, integer)
RETURNS float8 AS 'contsel' LANGUAGE internal STABLE STRICT;

CREATE OPERATOR === (
    LEFTARG = boolean,
    RIGHTARG = boolean,
    PROCEDURE = alter_op_test_fn,
    COMMUTATOR = ===,
    NEGATOR = !==,
    RESTRICT = customcontsel,
    JOIN = contjoinsel,
    HASHES, MERGES
);

SELECT pg_describe_object(refclassid,refobjid,refobjsubid) as ref, deptype
FROM pg_depend
WHERE classid = 'pg_operator'::regclass AND
      objid = '===(bool,bool)'::regoperator
ORDER BY 1;

--
-- Test resetting and setting restrict and join attributes.
--

ALTER OPERATOR === (boolean, boolean) SET (RESTRICT = NONE);
ALTER OPERATOR === (boolean, boolean) SET (JOIN = NONE);

SELECT oprrest, oprjoin FROM pg_operator WHERE oprname = '==='
  AND oprleft = 'boolean'::regtype AND oprright = 'boolean'::regtype;

SELECT pg_describe_object(refclassid,refobjid,refobjsubid) as ref, deptype
FROM pg_depend
WHERE classid = 'pg_operator'::regclass AND
      objid = '===(bool,bool)'::regoperator
ORDER BY 1;

ALTER OPERATOR === (boolean, boolean) SET (RESTRICT = contsel);
ALTER OPERATOR === (boolean, boolean) SET (JOIN = contjoinsel);

SELECT oprrest, oprjoin FROM pg_operator WHERE oprname = '==='
  AND oprleft = 'boolean'::regtype AND oprright = 'boolean'::regtype;

SELECT pg_describe_object(refclassid,refobjid,refobjsubid) as ref, deptype
FROM pg_depend
WHERE classid = 'pg_operator'::regclass AND
      objid = '===(bool,bool)'::regoperator
ORDER BY 1;

ALTER OPERATOR === (boolean, boolean) SET (RESTRICT = NONE, JOIN = NONE);

SELECT oprrest, oprjoin FROM pg_operator WHERE oprname = '==='
  AND oprleft = 'boolean'::regtype AND oprright = 'boolean'::regtype;

SELECT pg_describe_object(refclassid,refobjid,refobjsubid) as ref, deptype
FROM pg_depend
WHERE classid = 'pg_operator'::regclass AND
      objid = '===(bool,bool)'::regoperator
ORDER BY 1;

ALTER OPERATOR === (boolean, boolean) SET (RESTRICT = customcontsel, JOIN = contjoinsel);

SELECT oprrest, oprjoin FROM pg_operator WHERE oprname = '==='
  AND oprleft = 'boolean'::regtype AND oprright = 'boolean'::regtype;

SELECT pg_describe_object(refclassid,refobjid,refobjsubid) as ref, deptype
FROM pg_depend
WHERE classid = 'pg_operator'::regclass AND
      objid = '===(bool,bool)'::regoperator
ORDER BY 1;

--
-- Test invalid options.
--
ALTER OPERATOR === (boolean, boolean) SET (RESTRICT = non_existent_func);
ALTER OPERATOR === (boolean, boolean) SET (JOIN = non_existent_func);

-- invalid: non-lowercase quoted identifiers
ALTER OPERATOR & (bit, bit) SET ("Restrict" = _int_contsel, "Join" = _int_contjoinsel);

--
-- Test permission check. Must be owner to ALTER OPERATOR.
--
CREATE USER regress_alter_op_user;
SET SESSION AUTHORIZATION regress_alter_op_user;

ALTER OPERATOR === (boolean, boolean) SET (RESTRICT = NONE);

RESET SESSION AUTHORIZATION;

--
-- Test setting commutator, negator, merges, and hashes attributes,
-- which can only be set if not already set
--

CREATE FUNCTION alter_op_test_fn_bool_real(boolean, real)
RETURNS boolean AS $$ SELECT NULL::BOOLEAN; $$ LANGUAGE sql IMMUTABLE;

CREATE FUNCTION alter_op_test_fn_real_bool(real, boolean)
RETURNS boolean AS $$ SELECT NULL::BOOLEAN; $$ LANGUAGE sql IMMUTABLE;

-- operator
CREATE OPERATOR === (
    LEFTARG = boolean,
    RIGHTARG = real,
    PROCEDURE = alter_op_test_fn_bool_real
);

-- commutator
CREATE OPERATOR ==== (
    LEFTARG = real,
    RIGHTARG = boolean,
    PROCEDURE = alter_op_test_fn_real_bool
);

-- negator
CREATE OPERATOR !==== (
    LEFTARG = boolean,
    RIGHTARG = real,
    PROCEDURE = alter_op_test_fn_bool_real
);

-- No-op setting already false hashes and merges to false works
ALTER OPERATOR === (boolean, real) SET (MERGES = false);
ALTER OPERATOR === (boolean, real) SET (HASHES = false);

-- Test setting merges and hashes
ALTER OPERATOR === (boolean, real) SET (MERGES);
ALTER OPERATOR === (boolean, real) SET (HASHES);
SELECT oprcanmerge, oprcanhash
FROM pg_operator WHERE oprname = '==='
  AND oprleft = 'boolean'::regtype AND oprright = 'real'::regtype;

-- Test setting commutator
ALTER OPERATOR === (boolean, real) SET (COMMUTATOR = ====);

-- Check that oprcom has been set on both the operator and commutator,
-- that they reference each other, and that the operator used is the existing
-- one we created and not a new shell operator.
SELECT op.oprname AS operator_name, com.oprname AS commutator_name,
  com.oprcode AS commutator_func
  FROM pg_operator op
  INNER JOIN pg_operator com ON (op.oid = com.oprcom AND op.oprcom = com.oid)
  WHERE op.oprname = '==='
  AND op.oprleft = 'boolean'::regtype AND op.oprright = 'real'::regtype;

-- Cannot set self as negator
ALTER OPERATOR === (boolean, real) SET (NEGATOR = ===);

-- Test setting negator
ALTER OPERATOR === (boolean, real) SET (NEGATOR = !====);

-- Check that oprnegate has been set on both the operator and negator,
-- that they reference each other, and that the operator used is the existing
-- one we created and not a new shell operator.
SELECT op.oprname AS operator_name, neg.oprname AS negator_name,
  neg.oprcode AS negator_func
  FROM pg_operator op
  INNER JOIN pg_operator neg ON (op.oid = neg.oprnegate AND op.oprnegate = neg.oid)
  WHERE op.oprname = '==='
  AND op.oprleft = 'boolean'::regtype AND op.oprright = 'real'::regtype;

-- Test that no-op set succeeds
ALTER OPERATOR === (boolean, real) SET (NEGATOR = !====);
ALTER OPERATOR === (boolean, real) SET (COMMUTATOR = ====);
ALTER OPERATOR === (boolean, real) SET (MERGES);
ALTER OPERATOR === (boolean, real) SET (HASHES);

-- Check that the final state of the operator is as we expect
SELECT oprcanmerge, oprcanhash,
       pg_describe_object('pg_operator'::regclass, oprcom, 0) AS commutator,
       pg_describe_object('pg_operator'::regclass, oprnegate, 0) AS negator
  FROM pg_operator WHERE oprname = '==='
  AND oprleft = 'boolean'::regtype AND oprright = 'real'::regtype;

-- Cannot change commutator, negator, merges, and hashes when already set

CREATE OPERATOR @= (
    LEFTARG = real,
    RIGHTARG = boolean,
    PROCEDURE = alter_op_test_fn_real_bool
);
CREATE OPERATOR @!= (
    LEFTARG = boolean,
    RIGHTARG = real,
    PROCEDURE = alter_op_test_fn_bool_real
);

ALTER OPERATOR === (boolean, real) SET (COMMUTATOR = @=);
ALTER OPERATOR === (boolean, real) SET (NEGATOR = @!=);
ALTER OPERATOR === (boolean, real) SET (MERGES = false);
ALTER OPERATOR === (boolean, real) SET (HASHES = false);

-- Cannot set an operator that already has a commutator as the commutator
ALTER OPERATOR @=(real, boolean) SET (COMMUTATOR = ===);

-- Cannot set an operator that already has a negator as the negator
ALTER OPERATOR @!=(boolean, real) SET (NEGATOR = ===);

-- Check no changes made
SELECT oprcanmerge, oprcanhash,
       pg_describe_object('pg_operator'::regclass, oprcom, 0) AS commutator,
       pg_describe_object('pg_operator'::regclass, oprnegate, 0) AS negator
  FROM pg_operator WHERE oprname = '==='
  AND oprleft = 'boolean'::regtype AND oprright = 'real'::regtype;

--
-- Clean up
--

DROP USER regress_alter_op_user;

DROP OPERATOR === (boolean, boolean);
DROP OPERATOR === (boolean, real);
DROP OPERATOR ==== (real, boolean);
DROP OPERATOR !==== (boolean, real);
DROP OPERATOR @= (real, boolean);
DROP OPERATOR @!= (boolean, real);

DROP FUNCTION customcontsel(internal, oid, internal, integer);
DROP FUNCTION alter_op_test_fn(boolean, boolean);
DROP FUNCTION alter_op_test_fn_bool_real(boolean, real);
DROP FUNCTION alter_op_test_fn_real_bool(real, boolean);
