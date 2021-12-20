-- Initialization that requires path substitution.

-- directory paths and dlsuffix are passed to us in environment variables
\getenv abs_srcdir PG_ABS_SRCDIR
\getenv libdir PG_LIBDIR
\getenv dlsuffix PG_DLSUFFIX

\set regresslib :libdir '/regress' :dlsuffix

CREATE FUNCTION setenv(text, text)
   RETURNS void
   AS :'regresslib', 'regress_setenv'
   LANGUAGE C STRICT;

CREATE FUNCTION wait_pid(int)
   RETURNS void
   AS :'regresslib'
   LANGUAGE C STRICT;

\set path :abs_srcdir '/'
\set fnbody 'SELECT setenv(''PGSERVICEFILE'', ' :'path' ' || $1)'
CREATE FUNCTION set_pgservicefile(text) RETURNS void LANGUAGE SQL
    AS :'fnbody';
