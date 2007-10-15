--
-- first, define the datatype.  Turn off echoing so that expected file
-- does not depend on contents of this file.
--
SET client_min_messages = warning;
\set ECHO none
\i dict_xsyn.sql
\set ECHO all
RESET client_min_messages;

--configuration
ALTER TEXT SEARCH DICTIONARY xsyn (RULES='xsyn_sample', KEEPORIG=false);

--lexize
SELECT ts_lexize('xsyn', 'supernova');
SELECT ts_lexize('xsyn', 'grb');
