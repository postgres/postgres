
-- test error handling, i forgot to restore Warn_restart in
-- the trigger handler once. the errors and subsequent core dump were
-- interesting.

SELECT invalid_type_uncaught('rick');
SELECT invalid_type_caught('rick');
SELECT invalid_type_reraised('rick');
SELECT valid_type('rick');

--
-- Test Unicode error handling.
--

SELECT unicode_return_error();
INSERT INTO unicode_test (testvalue) VALUES ('test');
SELECT unicode_plan_error1();
SELECT unicode_plan_error2();
