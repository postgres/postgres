--
-- SELECT_DISTINCT_ON
--

SELECT DISTINCT ON string4 two, string4, ten
	   FROM tmp
   ORDER BY two using <, string4 using <, ten using <;

