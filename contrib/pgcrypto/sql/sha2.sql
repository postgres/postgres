--
-- SHA2 family
--

-- SHA224
SELECT digest('', 'sha224');
SELECT digest('a', 'sha224');
SELECT digest('abc', 'sha224');
SELECT digest('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq', 'sha224');
SELECT digest('12345678901234567890123456789012345678901234567890123456789012345678901234567890', 'sha224');

-- SHA256
SELECT digest('', 'sha256');
SELECT digest('a', 'sha256');
SELECT digest('abc', 'sha256');
SELECT digest('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq', 'sha256');
SELECT digest('12345678901234567890123456789012345678901234567890123456789012345678901234567890', 'sha256');

-- SHA384
SELECT digest('', 'sha384');
SELECT digest('a', 'sha384');
SELECT digest('abc', 'sha384');
SELECT digest('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq', 'sha384');
SELECT digest('abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu', 'sha384');
SELECT digest('abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz', 'sha384');

-- SHA512
SELECT digest('', 'sha512');
SELECT digest('a', 'sha512');
SELECT digest('abc', 'sha512');
SELECT digest('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq', 'sha512');
SELECT digest('abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu', 'sha512');
SELECT digest('abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz', 'sha512');
