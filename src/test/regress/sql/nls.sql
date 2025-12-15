-- directory paths and dlsuffix are passed to us in environment variables
\getenv libdir PG_LIBDIR
\getenv dlsuffix PG_DLSUFFIX

\set regresslib :libdir '/regress' :dlsuffix

CREATE FUNCTION test_translation()
    RETURNS void
    AS :'regresslib'
    LANGUAGE C;

-- We don't want to assume that the platform has any particular language
-- installed, so we use a "translation" for the C locale.  However, gettext
-- will short-circuit translation if lc_messages is just 'C'.  Fake it out
-- by appending a codeset name.  Fortunately it seems that that need not
-- match the actual database encoding.
SET lc_messages = 'C.UTF-8';

SELECT test_translation();

RESET lc_messages;
