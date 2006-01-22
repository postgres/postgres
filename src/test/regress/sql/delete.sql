CREATE TABLE delete_test (
    id SERIAL PRIMARY KEY,
    a INT
);

INSERT INTO delete_test (a) VALUES (10);
INSERT INTO delete_test (a) VALUES (50);
INSERT INTO delete_test (a) VALUES (100);

-- allow an alias to be specified for DELETE's target table
DELETE FROM delete_test AS dt WHERE dt.a > 75;

-- if an alias is specified, don't allow the original table name
-- to be referenced
BEGIN;
SET LOCAL add_missing_from = false;
DELETE FROM delete_test dt WHERE delete_test.a > 25;
ROLLBACK;

SELECT * FROM delete_test;

DROP TABLE delete_test;