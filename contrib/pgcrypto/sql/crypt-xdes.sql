--
-- crypt() and gen_salt(): extended des
--

SELECT crypt('', '_J9..j2zz');

SELECT crypt('foox', '_J9..j2zz');

CREATE TABLE ctest (data text, res text, salt text);
INSERT INTO ctest VALUES ('password', '', '');

UPDATE ctest SET salt = gen_salt('xdes', 1001);
UPDATE ctest SET res = crypt(data, salt);
SELECT res = crypt(data, res) AS "worked"
FROM ctest;

DROP TABLE ctest;

