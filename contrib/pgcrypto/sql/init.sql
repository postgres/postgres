--
-- init pgcrypto
--

\set ECHO none
\i pgcrypto.sql
\set ECHO all

-- check for encoding fn's
SELECT encode('foo', 'hex');
SELECT decode('666f6f', 'hex');

