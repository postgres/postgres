--
-- HMAC-SHA1
--

SELECT hmac(
'Hi There',
'\x0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b'::bytea,
'sha1');

-- 2
SELECT hmac(
'Jefe',
'what do ya want for nothing?',
'sha1');

-- 3
SELECT hmac(
'\xdddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd'::bytea,
'\xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'::bytea,
'sha1');

-- 4
SELECT hmac(
'\xcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd'::bytea,
'\x0102030405060708090a0b0c0d0e0f10111213141516171819'::bytea,
'sha1');

-- 5
SELECT hmac(
'Test With Truncation',
'\x0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c'::bytea,
'sha1');

-- 6
SELECT hmac(
'Test Using Larger Than Block-Size Key - Hash Key First',
'\xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'::bytea,
'sha1');

-- 7
SELECT hmac(
'Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data',
'\xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'::bytea,
'sha1');
