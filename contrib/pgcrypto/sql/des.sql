--
-- DES cipher
--
-- ensure consistent test output regardless of the default bytea format
SET bytea_output TO escape;

-- no official test vectors atm

-- from blowfish.sql
SELECT encode(encrypt(
decode('0123456789abcdef', 'hex'),
decode('fedcba9876543210', 'hex'),
'des-ecb/pad:none'), 'hex');

-- empty data
select encode(	encrypt('', 'foo', 'des'), 'hex');
-- 8 bytes key
select encode(	encrypt('foo', '01234589', 'des'), 'hex');

-- decrypt
select decrypt(encrypt('foo', '0123456', 'des'), '0123456', 'des');

-- iv
select encode(encrypt_iv('foo', '0123456', 'abcd', 'des'), 'hex');
select decrypt_iv(decode('50735067b073bb93', 'hex'), '0123456', 'abcd', 'des');

-- long message
select encode(encrypt('Lets try a longer message.', '01234567', 'des'), 'hex');
select decrypt(encrypt('Lets try a longer message.', '01234567', 'des'), '01234567', 'des');
