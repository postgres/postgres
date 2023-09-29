DROP TABLE IF EXISTS pvactst;
CREATE TABLE pvactst (i INT, a INT[], p POINT) USING pg_tde;
INSERT INTO pvactst SELECT i, array[1,2,3], point(i, i+1) FROM generate_series(1,1000) i;
CREATE INDEX spgist_pvactst ON pvactst USING spgist (p);
UPDATE pvactst SET i = i WHERE i < 1000;
-- crash!