CREATE EXTENSION test_ginpostinglist;

--
-- All the logic is in the test_ginpostinglist() function. It will throw
-- an error if something fails.
--
SELECT test_ginpostinglist();
