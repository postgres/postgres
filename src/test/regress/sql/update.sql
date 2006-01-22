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

-- aliases for the UPDATE target table
UPDATE update_test AS t SET b = 10 WHERE t.a = 10;

SELECT * FROM update_test;

UPDATE update_test t SET b = t.b + 10 WHERE t.a = 10;

SELECT * FROM update_test;

-- if an alias for the target table is specified, don't allow references
-- to the original table name
BEGIN;
SET LOCAL add_missing_from = false;
UPDATE update_test AS t SET b = update_test.b + 10 WHERE t.a = 10;
ROLLBACK;

DROP TABLE update_test;
