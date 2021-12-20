--
-- CREATE_FUNCTION_0
--

-- directory path and dlsuffix are passed to us in environment variables
\getenv libdir PG_LIBDIR
\getenv dlsuffix PG_DLSUFFIX

\set autoinclib :libdir '/autoinc' :dlsuffix
\set refintlib :libdir '/refint' :dlsuffix
\set regresslib :libdir '/regress' :dlsuffix

-- Create a bunch of C functions that will be used by later tests:

CREATE FUNCTION check_primary_key ()
	RETURNS trigger
	AS :'refintlib'
	LANGUAGE C;

CREATE FUNCTION check_foreign_key ()
	RETURNS trigger
	AS :'refintlib'
	LANGUAGE C;

CREATE FUNCTION autoinc ()
	RETURNS trigger
	AS :'autoinclib'
	LANGUAGE C;

CREATE FUNCTION trigger_return_old ()
        RETURNS trigger
        AS :'regresslib'
        LANGUAGE C;

CREATE FUNCTION ttdummy ()
        RETURNS trigger
        AS :'regresslib'
        LANGUAGE C;

CREATE FUNCTION set_ttdummy (int4)
        RETURNS int4
        AS :'regresslib'
        LANGUAGE C STRICT;

CREATE FUNCTION make_tuple_indirect (record)
        RETURNS record
        AS :'regresslib'
        LANGUAGE C STRICT;

CREATE FUNCTION test_atomic_ops()
    RETURNS bool
    AS :'regresslib'
    LANGUAGE C;

CREATE FUNCTION test_fdw_handler()
    RETURNS fdw_handler
    AS :'regresslib', 'test_fdw_handler'
    LANGUAGE C;

CREATE FUNCTION test_support_func(internal)
    RETURNS internal
    AS :'regresslib', 'test_support_func'
    LANGUAGE C STRICT;

CREATE FUNCTION test_opclass_options_func(internal)
    RETURNS void
    AS :'regresslib', 'test_opclass_options_func'
    LANGUAGE C;

CREATE FUNCTION test_enc_conversion(bytea, name, name, bool, validlen OUT int, result OUT bytea)
    AS :'regresslib', 'test_enc_conversion'
    LANGUAGE C STRICT;

CREATE FUNCTION binary_coercible(oid, oid)
    RETURNS bool
    AS :'regresslib', 'binary_coercible'
    LANGUAGE C STRICT STABLE PARALLEL SAFE;

-- Things that shouldn't work:

CREATE FUNCTION test1 (int) RETURNS int LANGUAGE SQL
    AS 'SELECT ''not an integer'';';

CREATE FUNCTION test1 (int) RETURNS int LANGUAGE SQL
    AS 'not even SQL';

CREATE FUNCTION test1 (int) RETURNS int LANGUAGE SQL
    AS 'SELECT 1, 2, 3;';

CREATE FUNCTION test1 (int) RETURNS int LANGUAGE SQL
    AS 'SELECT $2;';

CREATE FUNCTION test1 (int) RETURNS int LANGUAGE SQL
    AS 'a', 'b';

CREATE FUNCTION test1 (int) RETURNS int LANGUAGE C
    AS 'nosuchfile';

-- To produce stable regression test output, we have to filter the name
-- of the regresslib file out of the error message in this test.
\set VERBOSITY sqlstate
CREATE FUNCTION test1 (int) RETURNS int LANGUAGE C
    AS :'regresslib', 'nosuchsymbol';
\set VERBOSITY default
SELECT regexp_replace(:'LAST_ERROR_MESSAGE', 'file ".*"', 'file "..."');

CREATE FUNCTION test1 (int) RETURNS int LANGUAGE internal
    AS 'nosuch';
