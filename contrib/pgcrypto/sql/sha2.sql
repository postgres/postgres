--
-- SHA2 family
--

-- SHA224
SELECT encode(digest('', 'sha224'), 'hex');
SELECT encode(digest('a', 'sha224'), 'hex');
SELECT encode(digest('abc', 'sha224'), 'hex');
SELECT encode(digest('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq', 'sha224'), 'hex');
SELECT encode(digest('12345678901234567890123456789012345678901234567890123456789012345678901234567890', 'sha224'), 'hex');

-- SHA256
SELECT encode(digest('', 'sha256'), 'hex');
SELECT encode(digest('a', 'sha256'), 'hex');
SELECT encode(digest('abc', 'sha256'), 'hex');
SELECT encode(digest('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq', 'sha256'), 'hex');
SELECT encode(digest('12345678901234567890123456789012345678901234567890123456789012345678901234567890', 'sha256'), 'hex');

-- SHA384
SELECT encode(digest('', 'sha384'), 'hex');
SELECT encode(digest('a', 'sha384'), 'hex');
SELECT encode(digest('abc', 'sha384'), 'hex');
SELECT encode(digest('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq', 'sha384'), 'hex');
SELECT encode(digest('abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu', 'sha384'), 'hex');
SELECT encode(digest('abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz', 'sha384'), 'hex');

-- SHA512
SELECT encode(digest('', 'sha512'), 'hex');
SELECT encode(digest('a', 'sha512'), 'hex');
SELECT encode(digest('abc', 'sha512'), 'hex');
SELECT encode(digest('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq', 'sha512'), 'hex');
SELECT encode(digest('abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu', 'sha512'), 'hex');
SELECT encode(digest('abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz', 'sha512'), 'hex');


