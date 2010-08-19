--
-- MD5 message digest
--

SELECT encode(digest('', 'md5'), 'hex');
SELECT encode(digest('a', 'md5'), 'hex');
SELECT encode(digest('abc', 'md5'), 'hex');
SELECT encode(digest('message digest', 'md5'), 'hex');
SELECT encode(digest('abcdefghijklmnopqrstuvwxyz', 'md5'), 'hex');
SELECT encode(digest('ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789', 'md5'), 'hex');
SELECT encode(digest('12345678901234567890123456789012345678901234567890123456789012345678901234567890', 'md5'), 'hex');
