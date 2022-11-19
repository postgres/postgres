CREATE EXTENSION test_lfind;

--
-- These tests don't produce any interesting output.  We're checking that
-- the operations complete without crashing or hanging and that none of their
-- internal sanity tests fail.
--
SELECT test_lfind8();
SELECT test_lfind8_le();
SELECT test_lfind32();
