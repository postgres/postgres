
VACUUM;
--
-- sanity check, if we don't have indices the test will take years to
-- complete.  But skip TOAST relations since they will have varying
-- names depending on the current OID counter.
--
SELECT relname, relhasindex
   FROM pg_class
   WHERE relhasindex AND relkind != 't'
   ORDER BY relname;

