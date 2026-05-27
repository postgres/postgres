# Test that concurrent DROP and CREATE commands do not leave behind
# references to non-existent objects.

setup
{
	CREATE SCHEMA testschema;
	CREATE SCHEMA alterschema;
	CREATE TYPE public.foo as enum ('one', 'two');
	CREATE TYPE public.footab as enum ('three', 'four');
	CREATE DOMAIN id AS int;
	CREATE FUNCTION f() RETURNS int LANGUAGE SQL RETURN 1;
	CREATE FUNCTION public.falter() RETURNS int LANGUAGE SQL RETURN 1;
	CREATE FOREIGN DATA WRAPPER fdw_wrapper;
	CREATE ROLE regress_dependency;
}

teardown
{
	DROP FUNCTION IF EXISTS testschema.foo();
	DROP FUNCTION IF EXISTS fooargtype(num foo);
	DROP FUNCTION IF EXISTS footrettype();
	DROP FUNCTION IF EXISTS foofunc();
	DROP FUNCTION IF EXISTS public.falter();
	DROP FUNCTION IF EXISTS alterschema.falter();
	DROP DOMAIN IF EXISTS idid;
	DROP SERVER IF EXISTS srv_fdw_wrapper;
	DROP TABLE IF EXISTS tabtype;
	DROP SCHEMA IF EXISTS testschema;
	DROP SCHEMA IF EXISTS alterschema;
	DROP TYPE IF EXISTS public.foo;
	DROP TYPE IF EXISTS public.footab;
	DROP DOMAIN IF EXISTS id;
	DROP FUNCTION IF EXISTS f();
	DROP FOREIGN DATA WRAPPER IF EXISTS fdw_wrapper;
	DROP ROLE regress_dependency;
}

session "s1"

step "s1_begin" { BEGIN; }
step "s1_create_function_in_schema" { CREATE FUNCTION testschema.foo() RETURNS int AS 'select 1' LANGUAGE sql; }
step "s1_create_function_with_argtype" { CREATE FUNCTION fooargtype(num foo) RETURNS int AS 'select 1' LANGUAGE sql; }
step "s1_create_function_with_rettype" { CREATE FUNCTION footrettype() RETURNS id LANGUAGE sql RETURN 1; }
step "s1_create_function_with_function" { CREATE FUNCTION foofunc() RETURNS int LANGUAGE SQL RETURN f() + 1; }
step "s1_alter_function_owner" { ALTER FUNCTION public.falter() OWNER TO regress_dependency; }
step "s1_alter_function_schema" { ALTER FUNCTION public.falter() SET SCHEMA alterschema; }
step "s1_create_domain_with_domain" { CREATE DOMAIN idid as id; }
step "s1_create_table_with_type" { CREATE TABLE tabtype(a footab); }
step "s1_create_server_with_fdw_wrapper" { CREATE SERVER srv_fdw_wrapper FOREIGN DATA WRAPPER fdw_wrapper; }
step "s1_commit" { COMMIT; }

session "s2"

step "s2_begin" { BEGIN; }
step "s2_drop_schema" { DROP SCHEMA testschema; }
step "s2_drop_alterschema" { DROP SCHEMA alterschema; }
step "s2_drop_foo_type" { DROP TYPE public.foo; }
step "s2_drop_foo_rettype" { DROP DOMAIN id; }
step "s2_drop_footab_type" { DROP TYPE public.footab; }
step "s2_drop_function_f" { DROP FUNCTION f(); }
step "s2_drop_domain_id" { DROP DOMAIN id; }
step "s2_drop_fdw_wrapper" { DROP FOREIGN DATA WRAPPER fdw_wrapper RESTRICT; }
step "s2_drop_role" { DROP ROLE regress_dependency; }
step "s2_commit" { COMMIT; }

# create function - drop schema
permutation "s1_begin" "s1_create_function_in_schema" "s2_drop_schema" "s1_commit"
permutation "s2_begin" "s2_drop_schema" "s1_create_function_in_schema" "s2_commit"

# alter function - drop schema
permutation "s1_begin" "s1_alter_function_schema" "s2_drop_alterschema" "s1_commit"
permutation "s2_begin" "s2_drop_alterschema" "s1_alter_function_schema" "s2_commit"

# create function - drop argtype
permutation "s1_begin" "s1_create_function_with_argtype" "s2_drop_foo_type" "s1_commit"
permutation "s2_begin" "s2_drop_foo_type" "s1_create_function_with_argtype" "s2_commit"

# create function - drop rettype
permutation "s1_begin" "s1_create_function_with_rettype" "s2_drop_foo_rettype" "s1_commit"
permutation "s2_begin" "s2_drop_foo_rettype" "s1_create_function_with_rettype" "s2_commit"

# create function - drop function used in its body
permutation "s1_begin" "s1_create_function_with_function" "s2_drop_function_f" "s1_commit"
permutation "s2_begin" "s2_drop_function_f" "s1_create_function_with_function" "s2_commit"

# create domain over domain - drop the base domain
permutation "s1_begin" "s1_create_domain_with_domain" "s2_drop_domain_id" "s1_commit"
permutation "s2_begin" "s2_drop_domain_id" "s1_create_domain_with_domain" "s2_commit"

# create table - drop type used in column
permutation "s1_begin" "s1_create_table_with_type" "s2_drop_footab_type" "s1_commit"
permutation "s2_begin" "s2_drop_footab_type" "s1_create_table_with_type" "s2_commit"

# create server - drop foreign data wrapper
permutation "s1_begin" "s1_create_server_with_fdw_wrapper" "s2_drop_fdw_wrapper" "s1_commit"
permutation "s2_begin" "s2_drop_fdw_wrapper" "s1_create_server_with_fdw_wrapper" "s2_commit"

# create function - drop owner role
permutation "s1_begin" "s1_alter_function_owner" "s2_drop_role" "s1_commit"

# XXX: This permutation is disabled because the error message, "role
# <OID> was concurrently dropped", contains an OID that is not stable.
#
# permutation "s2_begin" "s2_drop_role" "s1_alter_function_owner" "s2_commit"
