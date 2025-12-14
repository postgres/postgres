-- directory paths and dlsuffix are passed to us in environment variables
\getenv libdir PG_LIBDIR
\getenv dlsuffix PG_DLSUFFIX

\set regresslib :libdir '/regress' :dlsuffix

CREATE FUNCTION test_translation()
    RETURNS void
    AS :'regresslib'
    LANGUAGE C;

-- Some BSDen are sticky about wanting a codeset name in lc_messages,
-- but it seems that at least on common platforms it doesn't have
-- to match the actual database encoding.
SET lc_messages = 'es_ES.UTF-8';

SELECT test_translation();

RESET lc_messages;
