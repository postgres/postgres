--
-- SELECT_DISTINCT
--

--
-- awk '{print $3;}' onek.data | sort -n | uniq
--
SELECT DISTINCT two FROM tmp;

--
-- awk '{print $5;}' onek.data | sort -n | uniq
--
SELECT DISTINCT ten FROM tmp;

--
-- awk '{print $16;}' onek.data | sort -d | uniq
--
SELECT DISTINCT string4 FROM tmp;

--
-- awk '{print $3,$16,$5;}' onek.data | sort -d | uniq |
-- sort +0n -1 +1d -2 +2n -3
--
SELECT DISTINCT two, string4, ten
   FROM tmp
   ORDER BY two using <, string4 using <, ten using <;

--
-- awk '{print $2;}' person.data |
-- awk '{if(NF!=1){print $2;}else{print;}}' - emp.data |
-- awk '{if(NF!=1){print $2;}else{print;}}' - student.data |
-- awk 'BEGIN{FS="      ";}{if(NF!=1){print $5;}else{print;}}' - stud_emp.data |
-- sort -n -r | uniq
--
SELECT DISTINCT p.age FROM person* p ORDER BY age using >;

