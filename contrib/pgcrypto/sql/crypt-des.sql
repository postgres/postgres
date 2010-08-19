--
-- crypt() and gen_salt(): crypt-des
--

SELECT crypt('', 'NB');

SELECT crypt('foox', 'NB');

CREATE TABLE ctest (data text, res text, salt text);
INSERT INTO ctest VALUES ('password', '', '');

UPDATE ctest SET salt = gen_salt('des');
UPDATE ctest SET res = crypt(data, salt);
SELECT res = crypt(data, res) AS "worked"
FROM ctest;

DROP TABLE ctest;
