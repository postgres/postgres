--
-- Blowfish cipher
--
-- ensure consistent test output regardless of the default bytea format
SET bytea_output TO escape;

-- some standard Blowfish testvalues
SELECT encode(encrypt(
decode('0000000000000000', 'hex'),
decode('0000000000000000', 'hex'),
'bf-ecb/pad:none'), 'hex');

SELECT encode(encrypt(
decode('ffffffffffffffff', 'hex'),
decode('ffffffffffffffff', 'hex'),
'bf-ecb/pad:none'), 'hex');

SELECT encode(encrypt(
decode('1000000000000001', 'hex'),
decode('3000000000000000', 'hex'),
'bf-ecb/pad:none'), 'hex');

SELECT encode(encrypt(
decode('1111111111111111', 'hex'),
decode('1111111111111111', 'hex'),
'bf-ecb/pad:none'), 'hex');

SELECT encode(encrypt(
decode('0123456789abcdef', 'hex'),
decode('fedcba9876543210', 'hex'),
'bf-ecb/pad:none'), 'hex');

SELECT encode(encrypt(
decode('01a1d6d039776742', 'hex'),
decode('fedcba9876543210', 'hex'),
'bf-ecb/pad:none'), 'hex');

SELECT encode(encrypt(
decode('ffffffffffffffff', 'hex'),
decode('0000000000000000', 'hex'),
'bf-ecb/pad:none'), 'hex');

-- setkey
SELECT encode(encrypt(
decode('fedcba9876543210', 'hex'),
decode('f0e1d2c3b4a5968778695a4b3c2d1e0f', 'hex'),
'bf-ecb/pad:none'), 'hex');

-- with padding
SELECT encode(encrypt(
decode('01234567890123456789', 'hex'),
decode('33443344334433443344334433443344', 'hex'),
'bf-ecb'), 'hex');

-- cbc

-- 28 bytes key
SELECT encode(encrypt(
decode('6b77b4d63006dee605b156e27403979358deb9e7154616d959f1652bd5', 'hex'),
decode('37363534333231204e6f77206973207468652074696d6520666f7220', 'hex'),
'bf-cbc'), 'hex');

-- 29 bytes key
SELECT encode(encrypt(
decode('6b77b4d63006dee605b156e27403979358deb9e7154616d959f1652bd5ff92cc', 'hex'),
decode('37363534333231204e6f77206973207468652074696d6520666f722000', 'hex'),
'bf-cbc'), 'hex');

-- blowfish-448 
SELECT encode(encrypt(
decode('fedcba9876543210', 'hex'),
decode('f0e1d2c3b4a5968778695a4b3c2d1e0f001122334455667704689104c2fd3b2f584023641aba61761f1f1f1f0e0e0e0effffffffffffffff', 'hex'),
'bf-ecb/pad:none'), 'hex');
-- result: c04504012e4e1f53

-- empty data
select encode(	encrypt('', 'foo', 'bf'), 'hex');
-- 10 bytes key
select encode(	encrypt('foo', '0123456789', 'bf'), 'hex');
-- 22 bytes key
select encode(	encrypt('foo', '0123456789012345678901', 'bf'), 'hex');

-- decrypt
select decrypt(encrypt('foo', '0123456', 'bf'), '0123456', 'bf');

-- iv
select encode(encrypt_iv('foo', '0123456', 'abcd', 'bf'), 'hex');
select decrypt_iv(decode('95c7e89322525d59', 'hex'), '0123456', 'abcd', 'bf');

-- long message
select encode(encrypt('Lets try a longer message.', '0123456789', 'bf'), 'hex');
select decrypt(encrypt('Lets try a longer message.', '0123456789', 'bf'), '0123456789', 'bf');
