--
-- crypt() and gen_salt(): extended des
--

SELECT crypt('', '_J9..j2zz');

SELECT crypt('foox', '_J9..j2zz');

-- check XDES handling of keys longer than 8 chars
SELECT crypt('longlongpassword', '_J9..j2zz');

-- error, salt too short
SELECT crypt('foox', '_J9..BWH');

-- error, count specified in the second argument is 0
SELECT crypt('password', '_........');

-- error, count will wind up still being 0 due to invalid encoding
-- of the count: only chars ``./0-9A-Za-z' are valid
SELECT crypt('password', '_..!!!!!!');

-- count should be non-zero here, will work
SELECT crypt('password', '_/!!!!!!!');

CREATE TABLE ctest (data text, res text, salt text);
INSERT INTO ctest VALUES ('password', '', '');

UPDATE ctest SET salt = gen_salt('xdes', 1001);
UPDATE ctest SET res = crypt(data, salt);
SELECT res = crypt(data, res) AS "worked"
FROM ctest;

DROP TABLE ctest;
