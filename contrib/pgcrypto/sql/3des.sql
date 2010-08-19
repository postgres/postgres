--
-- 3DES cipher
--
-- ensure consistent test output regardless of the default bytea format
SET bytea_output TO escape;

-- test vector from somewhere
SELECT encode(encrypt(
decode('80 00 00 00 00 00 00 00', 'hex'),
decode('01 01 01 01 01 01 01 01
	01 01 01 01 01 01 01 01
	01 01 01 01 01 01 01 01', 'hex'),
'3des-ecb/pad:none'), 'hex');
-- val 95 F8 A5 E5 DD 31 D9 00

select encode(	encrypt('', 'foo', '3des'), 'hex');
-- 10 bytes key
select encode(	encrypt('foo', '0123456789', '3des'), 'hex');
-- 22 bytes key
select encode(	encrypt('foo', '0123456789012345678901', '3des'), 'hex');

-- decrypt
select decrypt(encrypt('foo', '0123456', '3des'), '0123456', '3des');

-- iv
select encode(encrypt_iv('foo', '0123456', 'abcd', '3des'), 'hex');
select decrypt_iv(decode('50735067b073bb93', 'hex'), '0123456', 'abcd', '3des');

-- long message
select encode(encrypt('Lets try a longer message.', '0123456789012345678901', '3des'), 'hex');
select decrypt(encrypt('Lets try a longer message.', '0123456789012345678901', '3des'), '0123456789012345678901', '3des');
