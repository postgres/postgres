--
-- sanity check, if we don't have indices the test will take years to
-- complete.
--
SELECT relname, relhasindex
   FROM pg_class
   WHERE relhasindex
   ORDER BY relname;

