--
-- HMAC-MD5
--

SELECT hmac(
'Hi There',
'\x0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b'::bytea,
'md5');

-- 2
SELECT hmac(
'Jefe',
'what do ya want for nothing?',
'md5');

-- 3
SELECT hmac(
'\xdddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd'::bytea,
'\xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'::bytea,
'md5');

-- 4
SELECT hmac(
'\xcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd'::bytea,
'\x0102030405060708090a0b0c0d0e0f10111213141516171819'::bytea,
'md5');

-- 5
SELECT hmac(
'Test With Truncation',
'\x0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c'::bytea,
'md5');

-- 6
SELECT hmac(
'Test Using Larger Than Block-Size Key - Hash Key First',
'\xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'::bytea,
'md5');

-- 7
SELECT hmac(
'Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data',
'\xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'::bytea,
'md5');
