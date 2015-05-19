-- general regression test for transforms

DROP EXTENSION IF EXISTS hstore CASCADE;
DROP EXTENSION IF EXISTS plperl CASCADE;
DROP EXTENSION IF EXISTS hstore_plperl CASCADE;

CREATE EXTENSION hstore;
CREATE EXTENSION plperl;

CREATE FUNCTION hstore_to_plperl(val internal) RETURNS internal
LANGUAGE C STRICT IMMUTABLE
AS '$libdir/hstore_plperl';

CREATE FUNCTION plperl_to_hstore(val internal) RETURNS hstore
LANGUAGE C STRICT IMMUTABLE
AS '$libdir/hstore_plperl';

CREATE TRANSFORM FOR foo LANGUAGE plperl (FROM SQL WITH FUNCTION hstore_to_plperl(internal), TO SQL WITH FUNCTION plperl_to_hstore(internal));  -- fail
CREATE TRANSFORM FOR hstore LANGUAGE foo (FROM SQL WITH FUNCTION hstore_to_plperl(internal), TO SQL WITH FUNCTION plperl_to_hstore(internal));  -- fail
CREATE TRANSFORM FOR hstore LANGUAGE plperl (FROM SQL WITH FUNCTION hstore_out(hstore), TO SQL WITH FUNCTION plperl_to_hstore(internal));  -- fail
CREATE TRANSFORM FOR hstore LANGUAGE plperl (FROM SQL WITH FUNCTION internal_in(cstring), TO SQL WITH FUNCTION plperl_to_hstore(internal));  -- fail

CREATE TRANSFORM FOR hstore LANGUAGE plperl (FROM SQL WITH FUNCTION hstore_to_plperl(internal), TO SQL WITH FUNCTION plperl_to_hstore(internal));  -- ok
CREATE TRANSFORM FOR hstore LANGUAGE plperl (FROM SQL WITH FUNCTION hstore_to_plperl(internal), TO SQL WITH FUNCTION plperl_to_hstore(internal));  -- fail
CREATE OR REPLACE TRANSFORM FOR hstore LANGUAGE plperl (FROM SQL WITH FUNCTION hstore_to_plperl(internal), TO SQL WITH FUNCTION plperl_to_hstore(internal));  -- ok
CREATE OR REPLACE TRANSFORM FOR hstore LANGUAGE plperl (FROM SQL WITH FUNCTION hstore_to_plperl(internal));  -- ok
CREATE OR REPLACE TRANSFORM FOR hstore LANGUAGE plperl (TO SQL WITH FUNCTION plperl_to_hstore(internal));  -- ok

COMMENT ON TRANSFORM FOR hstore LANGUAGE plperl IS 'test';

DROP TRANSFORM IF EXISTS FOR fake_type LANGUAGE plperl;
DROP TRANSFORM IF EXISTS FOR hstore LANGUAGE fake_lang;
DROP TRANSFORM FOR foo LANGUAGE plperl;
DROP TRANSFORM FOR hstore LANGUAGE foo;
DROP TRANSFORM FOR hstore LANGUAGE plperl;
DROP TRANSFORM IF EXISTS FOR hstore LANGUAGE plperl;

DROP FUNCTION hstore_to_plperl(val internal);
DROP FUNCTION plperl_to_hstore(val internal);

CREATE EXTENSION hstore_plperl;
\dx+ hstore_plperl
ALTER EXTENSION hstore_plperl DROP TRANSFORM FOR hstore LANGUAGE plperl;
\dx+ hstore_plperl
ALTER EXTENSION hstore_plperl ADD TRANSFORM FOR hstore LANGUAGE plperl;
\dx+ hstore_plperl

DROP EXTENSION hstore CASCADE;
DROP EXTENSION plperl CASCADE;
