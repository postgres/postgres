--
-- init pgcrypto
--

\set ECHO none
\i pgcrypto.sql
\set ECHO all

-- check for encoding fn's
SELECT encode('foo', 'hex');
SELECT decode('666f6f', 'hex');

-- check error handling
select gen_salt('foo');
select digest('foo', 'foo');
select hmac('foo', 'foo', 'foo');
select encrypt('foo', 'foo', 'foo');

