--
-- crypt() and gen_salt(): bcrypt
--

SELECT crypt('', '$2a$06$RQiOJ.3ELirrXwxIZY8q0O');

SELECT crypt('foox', '$2a$06$RQiOJ.3ELirrXwxIZY8q0O');

CREATE TABLE ctest (data text, res text, salt text);
INSERT INTO ctest VALUES ('password', '', '');

UPDATE ctest SET salt = gen_salt('bf', 8);
UPDATE ctest SET res = crypt(data, salt);
SELECT res = crypt(data, res) AS "worked" 
FROM ctest;

DROP TABLE ctest;

