# Test trusted CREATE EXTENSION caller attacking the bootstrap superuser:
# - s1: CREATE EXTENSION
# - s2: lock a pg_proc_proname_args_nsp_index entry to make s1 pause
# - s3: ALTER SCHEMA RENAME while s1 is half-done
setup
{
	CREATE ROLE regress_attacker;
	DO $$
	BEGIN
		EXECUTE format('GRANT CREATE ON DATABASE %I TO regress_attacker',
					   current_catalog);
	END$$;
	CREATE SCHEMA s AUTHORIZATION regress_attacker;
}
teardown
{
	DROP OWNED BY regress_attacker;
	DROP ROLE regress_attacker;
}

# Start CREATE EXTENSION, pause at one CREATE FUNCTION until s2 aborts.
session s1
setup	{
	SET SESSION AUTHORIZATION regress_attacker;
}
# The attack splits the extension objects across two schemas.  That would make
# ltree--1.1--1.2.sql fail, so stop at 1.1.
step create1	{ CREATE EXTENSION ltree SCHEMA s VERSION '1.1'; }

# Control s1's CREATE FUNCTION wait.  As a non-superuser, we can only cause s1
# to wait at a CREATE FUNCTION where proargtypes does not contain an
# extension-member type.  That's because extension-member typoid values are
# not yet committed.  (Even for a superuser, you'd need a hack to deduce the
# OID.)  ALTER EXTENSION UPDATE attacks wouldn't have that restriction.
session s2
setup	{ SET SESSION AUTHORIZATION regress_attacker; }
step sleep2	{
	BEGIN;
	CREATE FUNCTION s.text2ltree(text) RETURNS int LANGUAGE sql AS 'SELECT 0';
}
step r2	{ ROLLBACK; }
# Since the server blocks the attack, this simply finds no lca(s.ltree,
# s.ltree) function.  The only lca is s_old.lca(s_old.ltree, s_old.ltree).
step crash2	{
	SET LOCAL search_path = s, s_old;
	SELECT lca(123::ltree, 456::ltree);
}

# ALTER SCHEMA RENAME while s1 CREATE EXTENSION is in a lock wait.  Extension
# objects created before the swap remain in the pre-swap schema, yet the
# attack must prevent later commands from failing and rolling back the whole
# CREATE EXTENSION.  Hence, make copies of functions and operators that later
# CREATE OPERATOR CLASS commands need and that the attack left behind in the
# pre-swap schema.  In the general case of designing attacks like this, an
# alternative can be to pause execution a second time and swap schemas back.
session s3
setup	{ SET SESSION AUTHORIZATION regress_attacker; }
step swap3	{
	ALTER SCHEMA s RENAME TO s_old;
	CREATE SCHEMA s;
	SET LOCAL search_path = s;
	CREATE DOMAIN ltree AS int8;

	CREATE FUNCTION ltree_fake(ltree, ltree) RETURNS bool LANGUAGE sql
		AS 'SELECT false';
	CREATE FUNCTION ltree_cmp(ltree, ltree) RETURNS int4 LANGUAGE sql
		AS 'SELECT -1';

	CREATE OPERATOR < (
		LEFTARG = ltree,
		RIGHTARG = ltree,
		PROCEDURE = ltree_fake,
		COMMUTATOR = '>',
		NEGATOR = '>=',
		RESTRICT = contsel,
		JOIN = contjoinsel
	);

	CREATE OPERATOR <= (
		LEFTARG = ltree,
		RIGHTARG = ltree,
		PROCEDURE = ltree_fake,
		COMMUTATOR = '>=',
		NEGATOR = '>',
		RESTRICT = contsel,
		JOIN = contjoinsel
	);

	CREATE OPERATOR >= (
		LEFTARG = ltree,
		RIGHTARG = ltree,
		PROCEDURE = ltree_fake,
		COMMUTATOR = '<=',
		NEGATOR = '<',
		RESTRICT = contsel,
		JOIN = contjoinsel
	);

	CREATE OPERATOR > (
		LEFTARG = ltree,
		RIGHTARG = ltree,
		PROCEDURE = ltree_fake,
		COMMUTATOR = '<',
		NEGATOR = '<=',
		RESTRICT = contsel,
		JOIN = contjoinsel
	);

	CREATE OPERATOR = (
		LEFTARG = ltree,
		RIGHTARG = ltree,
		PROCEDURE = ltree_fake,
		COMMUTATOR = '=',
		NEGATOR = '<>',
		RESTRICT = eqsel,
		JOIN = eqjoinsel,
		SORT1 = '<',
		SORT2 = '<'
	);
}

permutation
	sleep2			# uncommitted CREATE FUNCTION locks index entry
	create1			# CREATE EXTENSION blocks on s2
	swap3(create1)	# SCHEMA RENAME blocks on CREATE EXTENSION
	r2(swap3)		# unblock all
	crash2
