--
-- init pgcrypto
--

\set ECHO none
\i pgcrypto.sql
\set ECHO all

-- check for encoding fn's
select encode('foo', 'hex');
select decode('666f6f', 'hex');

