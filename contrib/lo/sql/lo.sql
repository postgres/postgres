CREATE EXTENSION lo;

CREATE TABLE image (title text, raster lo);

CREATE TRIGGER t_raster BEFORE UPDATE OR DELETE ON image
    FOR EACH ROW EXECUTE PROCEDURE lo_manage(raster);

SELECT lo_create(43213);
SELECT lo_create(43214);

INSERT INTO image (title, raster) VALUES ('beautiful image', 43213);

SELECT lo_get(43213);
SELECT lo_get(43214);

UPDATE image SET raster = 43214 WHERE title = 'beautiful image';

SELECT lo_get(43213);
SELECT lo_get(43214);

-- test updating of unrelated column
UPDATE image SET title = 'beautiful picture' WHERE title = 'beautiful image';

SELECT lo_get(43214);

DELETE FROM image;

SELECT lo_get(43214);

-- Now let's try it with an AFTER trigger

DROP TRIGGER t_raster ON image;

CREATE CONSTRAINT TRIGGER t_raster AFTER UPDATE OR DELETE ON image
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW EXECUTE PROCEDURE lo_manage(raster);

SELECT lo_create(43223);
SELECT lo_create(43224);
SELECT lo_create(43225);

INSERT INTO image (title, raster) VALUES ('beautiful image', 43223);

SELECT lo_get(43223);

UPDATE image SET raster = 43224 WHERE title = 'beautiful image';

SELECT lo_get(43223);  -- gone
SELECT lo_get(43224);

-- test updating of unrelated column
UPDATE image SET title = 'beautiful picture' WHERE title = 'beautiful image';

SELECT lo_get(43224);

-- this case used to be buggy
BEGIN;
UPDATE image SET title = 'beautiful image' WHERE title = 'beautiful picture';
UPDATE image SET raster = 43225 WHERE title = 'beautiful image';
SELECT lo_get(43224);
COMMIT;

SELECT lo_get(43224);  -- gone
SELECT lo_get(43225);

DELETE FROM image;

SELECT lo_get(43225);  -- gone


SELECT lo_oid(1::lo);

DROP TABLE image;
