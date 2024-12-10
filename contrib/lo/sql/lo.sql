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

SELECT lo_oid(1::lo);

DROP TABLE image;
