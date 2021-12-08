--
-- Blowfish cipher
--

-- some standard Blowfish testvalues
SELECT encrypt('\x0000000000000000', '\x0000000000000000', 'bf-ecb/pad:none');
SELECT encrypt('\xffffffffffffffff', '\xffffffffffffffff', 'bf-ecb/pad:none');
SELECT encrypt('\x1000000000000001', '\x3000000000000000', 'bf-ecb/pad:none');
SELECT encrypt('\x1111111111111111', '\x1111111111111111', 'bf-ecb/pad:none');
SELECT encrypt('\x0123456789abcdef', '\xfedcba9876543210', 'bf-ecb/pad:none');
SELECT encrypt('\x01a1d6d039776742', '\xfedcba9876543210', 'bf-ecb/pad:none');
SELECT encrypt('\xffffffffffffffff', '\x0000000000000000', 'bf-ecb/pad:none');

-- setkey
SELECT encrypt('\xfedcba9876543210', '\xf0e1d2c3b4a5968778695a4b3c2d1e0f', 'bf-ecb/pad:none');

-- with padding
SELECT encrypt('\x01234567890123456789', '\x33443344334433443344334433443344', 'bf-ecb');

-- cbc

-- 28 bytes key
SELECT encrypt('\x6b77b4d63006dee605b156e27403979358deb9e7154616d959f1652bd5',
               '\x37363534333231204e6f77206973207468652074696d6520666f7220',
               'bf-cbc');

-- 29 bytes key
SELECT encrypt('\x6b77b4d63006dee605b156e27403979358deb9e7154616d959f1652bd5ff92cc',
               '\x37363534333231204e6f77206973207468652074696d6520666f722000',
               'bf-cbc');

-- blowfish-448
SELECT encrypt('\xfedcba9876543210',
               '\xf0e1d2c3b4a5968778695a4b3c2d1e0f001122334455667704689104c2fd3b2f584023641aba61761f1f1f1f0e0e0e0effffffffffffffff',
               'bf-ecb/pad:none');

-- empty data
select encrypt('', 'foo', 'bf');
-- 10 bytes key
select encrypt('foo', '0123456789', 'bf');
-- 22 bytes key
select encrypt('foo', '0123456789012345678901', 'bf');

-- decrypt
select encode(decrypt(encrypt('foo', '0123456', 'bf'), '0123456', 'bf'), 'escape');

-- iv
select encrypt_iv('foo', '0123456', 'abcd', 'bf');
select encode(decrypt_iv('\x95c7e89322525d59', '0123456', 'abcd', 'bf'), 'escape');

-- long message
select encrypt('Lets try a longer message.', '0123456789', 'bf');
select encode(decrypt(encrypt('Lets try a longer message.', '0123456789', 'bf'), '0123456789', 'bf'), 'escape');
