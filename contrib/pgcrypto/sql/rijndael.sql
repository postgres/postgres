--
-- AES / Rijndael-128 cipher
--

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

