--
-- MD5 message digest
--

select encode(digest('', 'md5'), 'hex');
select encode(digest('a', 'md5'), 'hex');
select encode(digest('abc', 'md5'), 'hex');
select encode(digest('message digest', 'md5'), 'hex');
select encode(digest('abcdefghijklmnopqrstuvwxyz', 'md5'), 'hex');
select encode(digest('ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789', 'md5'), 'hex');
select encode(digest('12345678901234567890123456789012345678901234567890123456789012345678901234567890', 'md5'), 'hex');

