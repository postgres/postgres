--
-- UPDATE ... SET <col> = DEFAULT;
--

CREATE TABLE update_test (
    a   INT DEFAULT 10,
    b   INT
);

INSERT INTO update_test VALUES (5, 10);
INSERT INTO update_test VALUES (10, 15);

SELECT * FROM update_test;

UPDATE update_test SET a = DEFAULT, b = DEFAULT;

SELECT * FROM update_test;

DROP TABLE update_test;