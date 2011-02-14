--
-- init pgcrypto
--

CREATE EXTENSION pgcrypto;

-- ensure consistent test output regardless of the default bytea format
SET bytea_output TO escape;

-- check for encoding fn's
SELECT encode('foo', 'hex');
SELECT decode('666f6f', 'hex');

-- check error handling
select gen_salt('foo');
select digest('foo', 'foo');
select hmac('foo', 'foo', 'foo');
select encrypt('foo', 'foo', 'foo');
