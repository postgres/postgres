--
-- AES / Rijndael-128 cipher
--
-- ensure consistent test output regardless of the default bytea format
SET bytea_output TO escape;

-- some standard Rijndael testvalues
SELECT encode(encrypt(
decode('00112233445566778899aabbccddeeff', 'hex'),
decode('000102030405060708090a0b0c0d0e0f', 'hex'),
'aes-ecb/pad:none'), 'hex');

SELECT encode(encrypt(
decode('00112233445566778899aabbccddeeff', 'hex'),
decode('000102030405060708090a0b0c0d0e0f1011121314151617', 'hex'),
'aes-ecb/pad:none'), 'hex');

SELECT encode(encrypt(
decode('00112233445566778899aabbccddeeff', 'hex'),
decode('000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f', 'hex'),
'aes-ecb/pad:none'), 'hex');

-- cbc
SELECT encode(encrypt(
decode('00112233445566778899aabbccddeeff', 'hex'),
decode('000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f', 'hex'),
'aes-cbc/pad:none'), 'hex');

-- key padding

SELECT encode(encrypt(
decode('0011223344', 'hex'),
decode('000102030405', 'hex'),
'aes-cbc'), 'hex');

SELECT encode(encrypt(
decode('0011223344', 'hex'),
decode('000102030405060708090a0b0c0d0e0f10111213', 'hex'),
'aes-cbc'), 'hex');

SELECT encode(encrypt(
decode('0011223344', 'hex'),
decode('000102030405060708090a0b0c0d0e0f101112131415161718191a1b', 'hex'),
'aes-cbc'), 'hex');

-- empty data
select encode(	encrypt('', 'foo', 'aes'), 'hex');
-- 10 bytes key
select encode(	encrypt('foo', '0123456789', 'aes'), 'hex');
-- 22 bytes key
select encode(	encrypt('foo', '0123456789012345678901', 'aes'), 'hex');

-- decrypt
select decrypt(encrypt('foo', '0123456', 'aes'), '0123456', 'aes');

-- iv
select encode(encrypt_iv('foo', '0123456', 'abcd', 'aes'), 'hex');
select decrypt_iv(decode('2c24cb7da91d6d5699801268b0f5adad', 'hex'),
		'0123456', 'abcd', 'aes'); 

-- long message
select encode(encrypt('Lets try a longer message.', '0123456789', 'aes'), 'hex');
select decrypt(encrypt('Lets try a longer message.', '0123456789', 'aes'), '0123456789', 'aes');

