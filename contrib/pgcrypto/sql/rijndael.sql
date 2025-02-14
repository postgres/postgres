--
-- AES cipher (aka Rijndael-128, -192, or -256)
--

-- some standard Rijndael testvalues
SELECT encrypt(
'\x00112233445566778899aabbccddeeff',
'\x000102030405060708090a0b0c0d0e0f',
'aes-ecb/pad:none');

SELECT encrypt(
'\x00112233445566778899aabbccddeeff',
'\x000102030405060708090a0b0c0d0e0f1011121314151617',
'aes-ecb/pad:none');

SELECT encrypt(
'\x00112233445566778899aabbccddeeff',
'\x000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f',
'aes-ecb/pad:none');

-- cbc
SELECT encrypt(
'\x00112233445566778899aabbccddeeff',
'\x000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f',
'aes-cbc/pad:none');

-- without padding, input not multiple of block size
SELECT encrypt(
'\x00112233445566778899aabbccddeeff00',
'\x000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f',
'aes-cbc/pad:none');

-- key padding

SELECT encrypt(
'\x0011223344',
'\x000102030405',
'aes-cbc');

SELECT encrypt(
'\x0011223344',
'\x000102030405060708090a0b0c0d0e0f10111213',
'aes-cbc');

SELECT encrypt(
'\x0011223344',
'\x000102030405060708090a0b0c0d0e0f101112131415161718191a1b',
'aes-cbc');

-- empty data
select encrypt('', 'foo', 'aes');
-- 10 bytes key
select encrypt('foo', '0123456789', 'aes');
-- 22 bytes key
select encrypt('foo', '0123456789012345678901', 'aes');

-- decrypt
select encode(decrypt(encrypt('foo', '0123456', 'aes'), '0123456', 'aes'), 'escape');
-- data not multiple of block size
select encode(decrypt(encrypt('foo', '0123456', 'aes') || '\x00'::bytea, '0123456', 'aes'), 'escape');
-- bad padding
-- (The input value is the result of encrypt_iv('abcdefghijklmnopqrstuvwxyz', '0123456', 'abcd', 'aes')
-- with the 16th byte changed (s/db/eb/) to corrupt the padding of the last block.)
select encode(decrypt_iv('\xa21a9c15231465964e3396d32095e67eb52bab05f556a581621dee1b85385789', '0123456', 'abcd', 'aes'), 'escape');

-- iv
select encrypt_iv('foo', '0123456', 'abcd', 'aes');
select encode(decrypt_iv('\x2c24cb7da91d6d5699801268b0f5adad', '0123456', 'abcd', 'aes'), 'escape');

-- long message
select encrypt('Lets try a longer message.', '0123456789', 'aes');
select encode(decrypt(encrypt('Lets try a longer message.', '0123456789', 'aes'), '0123456789', 'aes'), 'escape');

-- cfb
SELECT encrypt(
'\x00112233445566778899aabbccddeeff',
'\x000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f',
'aes-cfb/pad:none');

-- without padding, input not multiple of block size
SELECT encrypt(
'\x00112233445566778899aabbccddeeff00',
'\x000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f',
'aes-cfb/pad:none');

-- key padding

SELECT encrypt(
'\x0011223344',
'\x000102030405',
'aes-cfb');

SELECT encrypt(
'\x0011223344',
'\x000102030405060708090a0b0c0d0e0f10111213',
'aes-cfb');

SELECT encrypt(
'\x0011223344',
'\x000102030405060708090a0b0c0d0e0f101112131415161718191a1b',
'aes-cfb');

-- empty data
select encrypt('', 'foo', 'aes-cfb');
-- 10 bytes key
select encrypt('foo', '0123456789', 'aes-cfb');
-- 22 bytes key
select encrypt('foo', '0123456789012345678901', 'aes-cfb');

-- decrypt
select encode(decrypt(encrypt('foo', '0123456', 'aes-cfb'), '0123456', 'aes-cfb'), 'escape');
-- data not multiple of block size
select encode(decrypt(encrypt('foo', '0123456', 'aes-cfb') || '\x00'::bytea, '0123456', 'aes-cfb'), 'escape');
-- bad padding
-- (The input value is the result of encrypt_iv('abcdefghijklmnopqrstuvwxyz', '0123456', 'abcd', 'aes-cfb')
-- with the 16th byte changed (s/c5/d5/) to corrupt the padding of the last block.)
select encode(decrypt_iv('\xf9ad6817cb58d31dd9ba6571fbc4f55d56f65b631f0f437cb828', '0123456', 'abcd', 'aes-cfb'), 'escape');

-- iv
select encrypt_iv('foo', '0123456', 'abcd', 'aes-cfb');
select encode(decrypt_iv('\xfea064', '0123456', 'abcd', 'aes-cfb'), 'escape');

-- long message
select encrypt('Lets try a longer message.', '0123456789', 'aes-cfb');
select encode(decrypt(encrypt('Lets try a longer message.', '0123456789', 'aes-cfb'), '0123456789', 'aes-cfb'), 'escape');
