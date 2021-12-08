--
-- 3DES cipher
--

-- test vector from somewhere
SELECT encrypt('\x8000000000000000',
               '\x010101010101010101010101010101010101010101010101',
               '3des-ecb/pad:none');

select encrypt('', 'foo', '3des');
-- 10 bytes key
select encrypt('foo', '0123456789', '3des');
-- 22 bytes key
select encrypt('foo', '0123456789012345678901', '3des');

-- decrypt
select encode(decrypt(encrypt('foo', '0123456', '3des'), '0123456', '3des'), 'escape');

-- iv
select encrypt_iv('foo', '0123456', 'abcd', '3des');
select encode(decrypt_iv('\x50735067b073bb93', '0123456', 'abcd', '3des'), 'escape');

-- long message
select encrypt('Lets try a longer message.', '0123456789012345678901', '3des');
select encode(decrypt(encrypt('Lets try a longer message.', '0123456789012345678901', '3des'), '0123456789012345678901', '3des'), 'escape');
