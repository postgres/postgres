--
-- HASH_INDEX
-- grep 843938989 hash.data
--

SELECT * FROM hash_i4_heap
   WHERE hash_i4_heap.random = 843938989;

--
-- hash index
-- grep 66766766 hash.data
--
SELECT * FROM hash_i4_heap
   WHERE hash_i4_heap.random = 66766766;

--
-- hash index
-- grep 1505703298 hash.data
--
SELECT * FROM hash_name_heap
   WHERE hash_name_heap.random = '1505703298'::name;

--
-- hash index
-- grep 7777777 hash.data
--
SELECT * FROM hash_name_heap
   WHERE hash_name_heap.random = '7777777'::name;

--
-- hash index
-- grep 1351610853 hash.data
--
SELECT * FROM hash_txt_heap
   WHERE hash_txt_heap.random = '1351610853'::text;

--
-- hash index
-- grep 111111112222222233333333 hash.data
--
SELECT * FROM hash_txt_heap
   WHERE hash_txt_heap.random = '111111112222222233333333'::text;

--
-- hash index
-- grep 444705537 hash.data
--
SELECT * FROM hash_f8_heap
   WHERE hash_f8_heap.random = '444705537'::float8;

--
-- hash index
-- grep 88888888 hash.data
--
SELECT * FROM hash_f8_heap
   WHERE hash_f8_heap.random = '88888888'::float8;

--
-- hash index
-- grep '^90[^0-9]' hashovfl.data
--
-- SELECT count(*) AS i988 FROM hash_ovfl_heap
--    WHERE x = 90;

--
-- hash index
-- grep '^1000[^0-9]' hashovfl.data
--
-- SELECT count(*) AS i0 FROM hash_ovfl_heap
--    WHERE x = 1000;

--
-- HASH
--
UPDATE hash_i4_heap
   SET random = 1
   WHERE hash_i4_heap.seqno = 1492;

SELECT h.seqno AS i1492, h.random AS i1
   FROM hash_i4_heap h
   WHERE h.random = 1;

UPDATE hash_i4_heap 
   SET seqno = 20000 
   WHERE hash_i4_heap.random = 1492795354;

SELECT h.seqno AS i20000 
   FROM hash_i4_heap h
   WHERE h.random = 1492795354;

UPDATE hash_name_heap 
   SET random = '0123456789abcdef'::name
   WHERE hash_name_heap.seqno = 6543;

SELECT h.seqno AS i6543, h.random AS c0_to_f
   FROM hash_name_heap h
   WHERE h.random = '0123456789abcdef'::name;

UPDATE hash_name_heap
   SET seqno = 20000
   WHERE hash_name_heap.random = '76652222'::name;

--
-- this is the row we just replaced; index scan should return zero rows 
--
SELECT h.seqno AS emptyset
   FROM hash_name_heap h
   WHERE h.random = '76652222'::name;

UPDATE hash_txt_heap 
   SET random = '0123456789abcdefghijklmnop'::text
   WHERE hash_txt_heap.seqno = 4002;

SELECT h.seqno AS i4002, h.random AS c0_to_p
   FROM hash_txt_heap h
   WHERE h.random = '0123456789abcdefghijklmnop'::text;

UPDATE hash_txt_heap
   SET seqno = 20000
   WHERE hash_txt_heap.random = '959363399'::text;

SELECT h.seqno AS t20000
   FROM hash_txt_heap h
   WHERE h.random = '959363399'::text;

UPDATE hash_f8_heap
   SET random = '-1234.1234'::float8
   WHERE hash_f8_heap.seqno = 8906;

SELECT h.seqno AS i8096, h.random AS f1234_1234 
   FROM hash_f8_heap h
   WHERE h.random = '-1234.1234'::float8;

UPDATE hash_f8_heap 
   SET seqno = 20000
   WHERE hash_f8_heap.random = '488912369'::float8;

SELECT h.seqno AS f20000
   FROM hash_f8_heap h
   WHERE h.random = '488912369'::float8;

-- UPDATE hash_ovfl_heap
--    SET x = 1000
--   WHERE x = 90;

-- this vacuums the index as well
-- VACUUM hash_ovfl_heap;

-- SELECT count(*) AS i0 FROM hash_ovfl_heap
--   WHERE x = 90;

-- SELECT count(*) AS i988 FROM hash_ovfl_heap
--  WHERE x = 1000;

