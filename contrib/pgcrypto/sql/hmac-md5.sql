--
-- HMAC-MD5
--

SELECT encode(hmac(
'Hi There',
decode('0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b', 'hex'),
'md5'), 'hex');

-- 2
SELECT encode(hmac(
'Jefe',
'what do ya want for nothing?',
'md5'), 'hex');

-- 3
SELECT encode(hmac(
decode('dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd', 'hex'),
decode('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa', 'hex'),
'md5'), 'hex');

-- 4
SELECT encode(hmac(
decode('cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd', 'hex'),
decode('0102030405060708090a0b0c0d0e0f10111213141516171819', 'hex'),
'md5'), 'hex');

-- 5
SELECT encode(hmac(
'Test With Truncation',
decode('0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c', 'hex'),
'md5'), 'hex');

-- 6
SELECT encode(hmac(
'Test Using Larger Than Block-Size Key - Hash Key First',
decode('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa', 'hex'),
'md5'), 'hex');

-- 7
SELECT encode(hmac(
'Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data',
decode('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa', 'hex'),
'md5'), 'hex');


