--
-- SHA1 message digest
--

select encode(digest('', 'sha1'), 'hex');
select encode(digest('a', 'sha1'), 'hex');
select encode(digest('abc', 'sha1'), 'hex');
select encode(digest('message digest', 'sha1'), 'hex');
select encode(digest('abcdefghijklmnopqrstuvwxyz', 'sha1'), 'hex');
select encode(digest('ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789', 'sha1'), 'hex');
select encode(digest('12345678901234567890123456789012345678901234567890123456789012345678901234567890', 'sha1'), 'hex');

