# Tests for transaction timeout that require long wait times

session s7
step s7_begin
{
    BEGIN ISOLATION LEVEL READ COMMITTED;
    SET transaction_timeout = '1s';
}
step s7_commit_and_chain { COMMIT AND CHAIN; }
step s7_sleep	{ SELECT pg_sleep(0.6); }
step s7_abort	{ ABORT; }

session s8
step s8_begin
{
    BEGIN ISOLATION LEVEL READ COMMITTED;
    SET transaction_timeout = '900ms';
}
# to test that quick query does not restart transaction_timeout
step s8_select_1 { SELECT 1; }
step s8_sleep	{ SELECT pg_sleep(0.6); }

session checker
step checker_sleep	{ SELECT pg_sleep(0.3); }
step s7_check	{ SELECT count(*) FROM pg_stat_activity WHERE application_name = 'isolation/timeouts/s7'; }
step s8_check	{ SELECT count(*) FROM pg_stat_activity WHERE application_name = 'isolation/timeouts/s8'; }

# COMMIT AND CHAIN must restart transaction timeout
permutation s7_begin s7_sleep s7_commit_and_chain s7_sleep s7_check s7_abort
# transaction timeout expires in presence of query flow, session s7 FATAL-out
# this relatevely long sleeps are picked to ensure 300ms gap between check and timeouts firing
# expected flow: timeouts is scheduled after s8_begin and fires approximately after checker_sleep (300ms before check)
# possible buggy flow: timeout is schedules after s8_select_1 and fires 300ms after s8_check
# to ensure this 300ms gap we need minimum transaction_timeout of 300ms
permutation s8_begin s8_sleep s8_select_1 s8_check checker_sleep checker_sleep s8_check
