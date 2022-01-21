--
-- Cast5 cipher
--

-- test vectors from RFC2144

-- 128 bit key
SELECT encrypt('\x0123456789ABCDEF', '\x0123456712345678234567893456789A', 'cast5-ecb/pad:none');

-- 80 bit key
SELECT encrypt('\x0123456789ABCDEF', '\x01234567123456782345', 'cast5-ecb/pad:none');

-- 40 bit key
SELECT encrypt('\x0123456789ABCDEF', '\x0123456712', 'cast5-ecb/pad:none');

-- cbc

-- empty data
select encrypt('', 'foo', 'cast5');
-- 10 bytes key
select encrypt('foo', '0123456789', 'cast5');

-- decrypt
select encode(decrypt(encrypt('foo', '0123456', 'cast5'), '0123456', 'cast5'), 'escape');

-- iv
select encrypt_iv('foo', '0123456', 'abcd', 'cast5');
select encode(decrypt_iv('\x384a970695ce016a', '0123456', 'abcd', 'cast5'), 'escape');

-- long message
select encrypt('Lets try a longer message.', '0123456789', 'cast5');
select encode(decrypt(encrypt('Lets try a longer message.', '0123456789', 'cast5'), '0123456789', 'cast5'), 'escape');
