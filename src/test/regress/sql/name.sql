--
-- NAME
-- all inputs are silently truncated at NAMEDATALEN (32) characters
--

-- fixed-length by reference
SELECT name 'name string' = name 'name string' AS "True";

SELECT name 'name string' = name 'name string ' AS "False";

--
--
--

CREATE TABLE NAME_TBL(f1 name);

INSERT INTO NAME_TBL(f1) VALUES ('ABCDEFGHIJKLMNOP');

INSERT INTO NAME_TBL(f1) VALUES ('abcdefghijklmnop');

INSERT INTO NAME_TBL(f1) VALUES ('asdfghjkl;');

INSERT INTO NAME_TBL(f1) VALUES ('343f%2a');

INSERT INTO NAME_TBL(f1) VALUES ('d34aaasdf');

INSERT INTO NAME_TBL(f1) VALUES ('');

INSERT INTO NAME_TBL(f1) VALUES ('1234567890ABCDEFGHIJKLMNOPQRSTUVWXYZ');


SELECT '' AS seven, NAME_TBL.*;

SELECT '' AS six, c.f1 FROM NAME_TBL c WHERE c.f1 <> 'ABCDEFGHIJKLMNOP';

SELECT '' AS one, c.f1 FROM NAME_TBL c WHERE c.f1 = 'ABCDEFGHIJKLMNOP';

SELECT '' AS three, c.f1 FROM NAME_TBL c WHERE c.f1 < 'ABCDEFGHIJKLMNOP';

SELECT '' AS four, c.f1 FROM NAME_TBL c WHERE c.f1 <= 'ABCDEFGHIJKLMNOP';

SELECT '' AS three, c.f1 FROM NAME_TBL c WHERE c.f1 > 'ABCDEFGHIJKLMNOP';

SELECT '' AS four, c.f1 FROM NAME_TBL c WHERE c.f1 >= 'ABCDEFGHIJKLMNOP';

SELECT '' AS seven, c.f1 FROM NAME_TBL c WHERE c.f1 ~ '.*';

SELECT '' AS zero, c.f1 FROM NAME_TBL c WHERE c.f1 !~ '.*';

SELECT '' AS three, c.f1 FROM NAME_TBL c WHERE c.f1 ~ '[0-9]';

SELECT '' AS two, c.f1 FROM NAME_TBL c WHERE c.f1 ~ '.*asdf.*';

DROP TABLE NAME_TBL;
