-- regression test for the uuid datatype
-- creating test tables
CREATE TABLE guid1
(
	guid_field UUID,
	text_field TEXT DEFAULT(now())
);
CREATE TABLE guid2
(
	guid_field UUID,
	text_field TEXT DEFAULT(now())
);

-- inserting invalid data tests
-- too long
INSERT INTO guid1(guid_field) VALUES('11111111-1111-1111-1111-111111111111F');
-- too short
INSERT INTO guid1(guid_field) VALUES('{11111111-1111-1111-1111-11111111111}');
-- valid data but invalid format
INSERT INTO guid1(guid_field) VALUES('111-11111-1111-1111-1111-111111111111');
INSERT INTO guid1(guid_field) VALUES('{22222222-2222-2222-2222-222222222222 ');
-- invalid data
INSERT INTO guid1(guid_field) VALUES('11111111-1111-1111-G111-111111111111');
INSERT INTO guid1(guid_field) VALUES('11+11111-1111-1111-1111-111111111111');

-- test non-error-throwing API
SELECT pg_input_is_valid('11', 'uuid');
SELECT * FROM pg_input_error_info('11', 'uuid');

--inserting three input formats
INSERT INTO guid1(guid_field) VALUES('11111111-1111-1111-1111-111111111111');
INSERT INTO guid1(guid_field) VALUES('{22222222-2222-2222-2222-222222222222}');
INSERT INTO guid1(guid_field) VALUES('3f3e3c3b3a3039383736353433a2313e');

-- retrieving the inserted data
SELECT guid_field FROM guid1;

-- ordering test
SELECT guid_field FROM guid1 ORDER BY guid_field ASC;
SELECT guid_field FROM guid1 ORDER BY guid_field DESC;

-- = operator test
SELECT COUNT(*) FROM guid1 WHERE guid_field = '3f3e3c3b-3a30-3938-3736-353433a2313e';

-- <> operator test
SELECT COUNT(*) FROM guid1 WHERE guid_field <> '11111111111111111111111111111111';

-- < operator test
SELECT COUNT(*) FROM guid1 WHERE guid_field < '22222222-2222-2222-2222-222222222222';

-- <= operator test
SELECT COUNT(*) FROM guid1 WHERE guid_field <= '22222222-2222-2222-2222-222222222222';

-- > operator test
SELECT COUNT(*) FROM guid1 WHERE guid_field > '22222222-2222-2222-2222-222222222222';

-- >= operator test
SELECT COUNT(*) FROM guid1 WHERE guid_field >= '22222222-2222-2222-2222-222222222222';

-- btree and hash index creation test
CREATE INDEX guid1_btree ON guid1 USING BTREE (guid_field);
CREATE INDEX guid1_hash  ON guid1 USING HASH  (guid_field);

-- unique index test
CREATE UNIQUE INDEX guid1_unique_BTREE ON guid1 USING BTREE (guid_field);

EXPLAIN (COSTS OFF)
SELECT COUNT(*) FROM guid1 WHERE guid_field <> '11111111111111111111111111111111' OR
							guid_field <> '3f3e3c3b-3a30-3938-3736-353433a2313e';
EXPLAIN (COSTS OFF)
SELECT COUNT(*) FROM guid1 WHERE guid_field <= '22222222-2222-2222-2222-222222222222' OR
									guid_field <= '11111111111111111111111111111111' OR
									guid_field <= '3f3e3c3b-3a30-3938-3736-353433a2313e';
EXPLAIN (COSTS OFF)
SELECT COUNT(*) FROM guid1 WHERE guid_field = '3f3e3c3b-3a30-3938-3736-353433a2313e' OR
							guid_field = '11111111111111111111111111111111';

-- should fail
INSERT INTO guid1(guid_field) VALUES('11111111-1111-1111-1111-111111111111');

-- check to see whether the new indexes are actually there
SELECT count(*) FROM pg_class WHERE relkind='i' AND relname LIKE 'guid%';

-- populating the test tables with additional records
INSERT INTO guid1(guid_field) VALUES('44444444-4444-4444-4444-444444444444');
INSERT INTO guid2(guid_field) VALUES('11111111-1111-1111-1111-111111111111');
INSERT INTO guid2(guid_field) VALUES('{22222222-2222-2222-2222-222222222222}');
INSERT INTO guid2(guid_field) VALUES('3f3e3c3b3a3039383736353433a2313e');

-- join test
SELECT COUNT(*) FROM guid1 g1 INNER JOIN guid2 g2 ON g1.guid_field = g2.guid_field;
SELECT COUNT(*) FROM guid1 g1 LEFT JOIN guid2 g2 ON g1.guid_field = g2.guid_field WHERE g2.guid_field IS NULL;

-- generation test
TRUNCATE guid1;
INSERT INTO guid1 (guid_field) VALUES (gen_random_uuid());
INSERT INTO guid1 (guid_field) VALUES (gen_random_uuid());
SELECT count(DISTINCT guid_field) FROM guid1;


-- extract functions

-- version
SELECT uuid_extract_version('11111111-1111-5111-8111-111111111111');  -- 5
SELECT uuid_extract_version(gen_random_uuid());  -- 4
SELECT uuid_extract_version('11111111-1111-1111-1111-111111111111');  -- null

-- timestamp
SELECT uuid_extract_timestamp('C232AB00-9414-11EC-B3C8-9F6BDECED846') = 'Tuesday, February 22, 2022 2:22:22.00 PM GMT+05:00';  -- RFC 4122bis test vector
SELECT uuid_extract_timestamp(gen_random_uuid());  -- null
SELECT uuid_extract_timestamp('11111111-1111-1111-1111-111111111111');  -- null


-- clean up
DROP TABLE guid1, guid2 CASCADE;
