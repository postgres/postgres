--
-- Cast5 cipher
--
-- ensure consistent test output regardless of the default bytea format
SET bytea_output TO escape;

-- test vectors from RFC2144

-- 128 bit key
SELECT encode(encrypt(
decode('01 23 45 67 89 AB CD EF', 'hex'),
decode('01 23 45 67 12 34 56 78 23 45 67 89 34 56 78 9A', 'hex'),
'cast5-ecb/pad:none'), 'hex');
-- result: 23 8B 4F E5 84 7E 44 B2

-- 80 bit key
SELECT encode(encrypt(
decode('01 23 45 67 89 AB CD EF', 'hex'),
decode('01 23 45 67 12 34 56 78 23 45', 'hex'),
'cast5-ecb/pad:none'), 'hex');
-- result: EB 6A 71 1A 2C 02 27 1B

-- 40 bit key
SELECT encode(encrypt(
decode('01 23 45 67 89 AB CD EF', 'hex'),
decode('01 23 45 67 12', 'hex'),
'cast5-ecb/pad:none'), 'hex');
-- result: 7A C8 16 D1 6E 9B 30 2E

-- cbc

-- empty data
select encode(	encrypt('', 'foo', 'cast5'), 'hex');
-- 10 bytes key
select encode(	encrypt('foo', '0123456789', 'cast5'), 'hex');

-- decrypt
select decrypt(encrypt('foo', '0123456', 'cast5'), '0123456', 'cast5');

-- iv
select encode(encrypt_iv('foo', '0123456', 'abcd', 'cast5'), 'hex');
select decrypt_iv(decode('384a970695ce016a', 'hex'),
                '0123456', 'abcd', 'cast5');

-- long message
select encode(encrypt('Lets try a longer message.', '0123456789', 'cast5'), 'hex');
select decrypt(encrypt('Lets try a longer message.', '0123456789', 'cast5'), '0123456789', 'cast5');

