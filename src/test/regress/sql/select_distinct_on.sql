--
-- SELECT_DISTINCT_ON
--

SELECT DISTINCT ON (string4) string4, two, ten
   FROM onek
   ORDER BY string4 using <, two using >, ten using <;

-- this will fail due to conflict of ordering requirements
SELECT DISTINCT ON (string4, ten) string4, two, ten
   FROM onek
   ORDER BY string4 using <, two using <, ten using <;

SELECT DISTINCT ON (string4, ten) string4, ten, two
   FROM onek
   ORDER BY string4 using <, ten using >, two using <;

-- bug #5049: early 8.4.x chokes on volatile DISTINCT ON clauses
select distinct on (1) floor(random()) as r, f1 from int4_tbl order by 1,2;
