--
-- SELECT_DISTINCT_ON
--

SELECT DISTINCT ON (string4) string4, two, ten
   FROM tmp
   ORDER BY string4 using <, two using >, ten using <;

-- this will fail due to conflict of ordering requirements
SELECT DISTINCT ON (string4, ten) string4, two, ten
   FROM tmp
   ORDER BY string4 using <, two using <, ten using <;

SELECT DISTINCT ON (string4, ten) string4, ten, two
   FROM tmp
   ORDER BY string4 using <, ten using >, two using <;
