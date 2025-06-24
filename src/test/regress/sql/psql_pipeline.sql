--
-- Tests using psql pipelining
--

CREATE TABLE psql_pipeline(a INTEGER PRIMARY KEY, s TEXT);

-- Single query
\startpipeline
SELECT $1 \bind 'val1' \sendpipeline
\endpipeline
\startpipeline
SELECT 'val1';
\endpipeline

-- Multiple queries
\startpipeline
SELECT $1 \bind 'val1' \sendpipeline
SELECT $1, $2 \bind 'val2' 'val3' \sendpipeline
SELECT $1, $2 \bind 'val2' 'val3' \sendpipeline
SELECT 'val4';
SELECT 'val5', 'val6';
\endpipeline

-- Multiple queries in single line, separated by semicolons
\startpipeline
SELECT 1; SELECT 2; SELECT 3
;
\echo :PIPELINE_COMMAND_COUNT
\endpipeline

-- Test \flush
\startpipeline
\flush
SELECT $1 \bind 'val1' \sendpipeline
\flush
SELECT $1, $2 \bind 'val2' 'val3' \sendpipeline
SELECT $1, $2 \bind 'val2' 'val3' \sendpipeline
\flush
SELECT 'val4';
SELECT 'val5', 'val6';
\endpipeline

-- Send multiple syncs
\startpipeline
\echo :PIPELINE_COMMAND_COUNT
\echo :PIPELINE_SYNC_COUNT
\echo :PIPELINE_RESULT_COUNT
SELECT $1 \bind 'val1' \sendpipeline
\syncpipeline
\syncpipeline
SELECT $1, $2 \bind 'val2' 'val3' \sendpipeline
\syncpipeline
SELECT $1, $2 \bind 'val4' 'val5' \sendpipeline
\echo :PIPELINE_COMMAND_COUNT
\echo :PIPELINE_SYNC_COUNT
\echo :PIPELINE_RESULT_COUNT
SELECT 'val7';
\syncpipeline
\syncpipeline
SELECT 'val8';
\syncpipeline
SELECT 'val9';
\echo :PIPELINE_COMMAND_COUNT
\echo :PIPELINE_SYNC_COUNT
\echo :PIPELINE_RESULT_COUNT
\endpipeline

-- Query terminated with a semicolon replaces an unnamed prepared
-- statement.
\startpipeline
SELECT $1 \parse ''
SELECT 1;
\bind_named ''
\endpipeline

-- Extended query is appended to pipeline by a semicolon after a
-- newline.
\startpipeline
SELECT $1 \bind 1
;
SELECT 2;
\endpipeline

-- \startpipeline should not have any effect if already in a pipeline.
\startpipeline
\startpipeline
SELECT $1 \bind 'val1' \sendpipeline
\endpipeline

-- Convert an implicit transaction block to an explicit transaction block.
\startpipeline
INSERT INTO psql_pipeline VALUES ($1) \bind 1 \sendpipeline
BEGIN \bind \sendpipeline
INSERT INTO psql_pipeline VALUES ($1) \bind 2 \sendpipeline
ROLLBACK \bind \sendpipeline
\endpipeline

-- Multiple explicit transactions
\startpipeline
BEGIN \bind \sendpipeline
INSERT INTO psql_pipeline VALUES ($1) \bind 1 \sendpipeline
ROLLBACK \bind \sendpipeline
BEGIN \bind \sendpipeline
INSERT INTO psql_pipeline VALUES ($1) \bind 1 \sendpipeline
COMMIT \bind \sendpipeline
\endpipeline

-- Use \parse and \bind_named
\startpipeline
SELECT $1 \parse ''
SELECT $1, $2 \parse ''
SELECT $2 \parse pipeline_1
\bind_named '' 1 2 \sendpipeline
\bind_named pipeline_1 2 \sendpipeline
\endpipeline

-- \getresults displays all results preceding a \flushrequest.
\startpipeline
SELECT $1 \bind 1 \sendpipeline
SELECT $1 \bind 2 \sendpipeline
\flushrequest
\getresults
\endpipeline

-- \getresults displays all results preceding a \syncpipeline.
\startpipeline
SELECT $1 \bind 1 \sendpipeline
SELECT $1 \bind 2 \sendpipeline
\syncpipeline
\getresults
\endpipeline

-- \getresults immediately returns if there is no result to fetch.
\startpipeline
\getresults
SELECT $1 \bind 2 \sendpipeline
\getresults
\flushrequest
\endpipeline
\getresults

-- \getresults only fetches results preceding a \flushrequest.
\startpipeline
SELECT $1 \bind 2 \sendpipeline
\flushrequest
SELECT $1 \bind 2 \sendpipeline
\getresults
\endpipeline

-- \getresults only fetches results preceding a \syncpipeline.
\startpipeline
SELECT $1 \bind 2 \sendpipeline
\syncpipeline
SELECT $1 \bind 2 \sendpipeline
\getresults
\endpipeline

-- Use pipeline with chunked results for both \getresults and \endpipeline.
\startpipeline
\set FETCH_COUNT 10
SELECT $1 \bind 2 \sendpipeline
\flushrequest
\getresults
SELECT $1 \bind 2 \sendpipeline
\endpipeline
\unset FETCH_COUNT

-- \getresults with specific number of requested results.
\startpipeline
SELECT $1 \bind 1 \sendpipeline
SELECT $1 \bind 2 \sendpipeline
SELECT $1 \bind 3 \sendpipeline
\echo :PIPELINE_SYNC_COUNT
\syncpipeline
\echo :PIPELINE_SYNC_COUNT
\echo :PIPELINE_RESULT_COUNT
\getresults 1
\echo :PIPELINE_RESULT_COUNT
SELECT $1 \bind 4 \sendpipeline
\getresults 3
\echo :PIPELINE_RESULT_COUNT
\endpipeline

-- \syncpipeline count as one command to fetch for \getresults.
\startpipeline
\syncpipeline
\syncpipeline
SELECT $1 \bind 1 \sendpipeline
\flushrequest
\getresults 2
\getresults 1
\endpipeline

-- \getresults 0 should get all the results.
\startpipeline
SELECT $1 \bind 1 \sendpipeline
SELECT $1 \bind 2 \sendpipeline
SELECT $1 \bind 3 \sendpipeline
\syncpipeline
\getresults 0
\endpipeline

--
-- Pipeline errors
--

-- \endpipeline outside of pipeline should fail
\endpipeline

-- After an aborted pipeline, commands after a \syncpipeline should be
-- displayed.
\startpipeline
SELECT $1 \bind \sendpipeline
\syncpipeline
SELECT $1 \bind 1 \sendpipeline
\endpipeline

-- For an incorrect number of parameters, the pipeline is aborted and
-- the following queries will not be executed.
\startpipeline
SELECT \bind 'val1' \sendpipeline
SELECT $1 \bind 'val1' \sendpipeline
\endpipeline

-- Using a semicolon with a parameter triggers an error and aborts
-- the pipeline.
\startpipeline
SELECT $1;
SELECT 1;
\endpipeline

-- An explicit transaction with an error needs to be rollbacked after
-- the pipeline.
\startpipeline
BEGIN \bind \sendpipeline
INSERT INTO psql_pipeline VALUES ($1) \bind 1 \sendpipeline
ROLLBACK \bind \sendpipeline
\endpipeline
ROLLBACK;

-- \watch is not allowed in a pipeline.
\startpipeline
SELECT \bind \sendpipeline
\watch 1
\endpipeline

-- \gdesc should fail as synchronous commands are not allowed in a pipeline,
-- and the pipeline should still be usable.
\startpipeline
SELECT $1 \bind 1 \gdesc
SELECT $1 \bind 1 \sendpipeline
\endpipeline

-- \gset is not allowed in a pipeline, pipeline should still be usable.
\startpipeline
SELECT $1 as i, $2 as j \parse ''
SELECT $1 as k, $2 as l \parse 'second'
\bind_named '' 1 2 \gset
\bind_named second 1 2 \gset pref02_ \echo :pref02_i :pref02_j
\bind_named '' 1 2 \sendpipeline
\endpipeline

-- \g and \gx are not allowed, pipeline should still be usable.
\startpipeline
SELECT $1 \bind 1 \g
SELECT $1 \bind 1 \g (format=unaligned tuples_only=on)
SELECT $1 \bind 1 \gx
SELECT $1 \bind 1 \gx (format=unaligned tuples_only=on)
\reset
SELECT $1 \bind 1 \sendpipeline
\endpipeline

-- \g and \gx warnings should be emitted in an aborted pipeline, with
-- pipeline still usable.
\startpipeline
SELECT $1 \bind \sendpipeline
\flushrequest
\getresults
SELECT $1 \bind 1 \g
SELECT $1 \bind 1 \gx
\endpipeline

-- \sendpipeline is not allowed outside of a pipeline
\sendpipeline
SELECT $1 \bind 1 \sendpipeline
\reset

-- \sendpipeline is not allowed if not preceded by \bind or \bind_named
\startpipeline
\sendpipeline
SELECT 1 \sendpipeline
\endpipeline

-- \gexec is not allowed, pipeline should still be usable.
\startpipeline
SELECT 'INSERT INTO psql_pipeline(a) SELECT generate_series(1, 10)' \parse 'insert_stmt'
\bind_named insert_stmt \gexec
\bind_named insert_stmt \sendpipeline
SELECT COUNT(*) FROM psql_pipeline \bind \sendpipeline
\endpipeline

-- After an error, pipeline is aborted and requires \syncpipeline to be
-- reusable.
\startpipeline
SELECT $1 \bind \sendpipeline
SELECT $1 \bind 1 \sendpipeline
SELECT $1 \parse a
\bind_named a 1 \sendpipeline
\close_prepared a
\flushrequest
\getresults
-- Pipeline is aborted.
SELECT $1 \bind 1 \sendpipeline
SELECT $1 \parse a
\bind_named a 1 \sendpipeline
\close_prepared a
-- Sync allows pipeline to recover.
\syncpipeline
\getresults
SELECT $1 \bind 1 \sendpipeline
SELECT $1 \parse a
\bind_named a 1 \sendpipeline
\close_prepared a
\flushrequest
\getresults
\endpipeline

-- In an aborted pipeline, \getresults 1 aborts commands one at a time.
\startpipeline
SELECT $1 \bind \sendpipeline
SELECT $1 \bind 1 \sendpipeline
SELECT $1 \parse a
\bind_named a 1 \sendpipeline
\syncpipeline
\getresults 1
\getresults 1
\getresults 1
\getresults 1
\getresults 1
\endpipeline

-- Test chunked results with an aborted pipeline.
\startpipeline
\set FETCH_COUNT 10
SELECT $1 \bind \sendpipeline
\flushrequest
\getresults
SELECT $1 \bind \sendpipeline
\endpipeline
\unset FETCH_COUNT

-- \getresults returns an error when an incorrect number is provided.
\startpipeline
\getresults -1
\endpipeline

-- \getresults when there is no result should not impact the next
-- query executed.
\getresults 1
select 1;

-- Error messages accumulate and are repeated.
\startpipeline
SELECT 1 \bind \sendpipeline
\gdesc
\gdesc
\endpipeline

--
-- Pipelines and transaction blocks
--

-- SET LOCAL will issue a warning when modifying a GUC outside of a
-- transaction block.  The change will still be valid as a pipeline
-- runs within an implicit transaction block.  Sending a sync will
-- commit the implicit transaction block. The first command after a
-- sync will not be seen as belonging to a pipeline.
\startpipeline
SET LOCAL statement_timeout='1h' \bind \sendpipeline
SHOW statement_timeout \bind \sendpipeline
\syncpipeline
SHOW statement_timeout \bind \sendpipeline
SET LOCAL statement_timeout='2h' \bind \sendpipeline
SHOW statement_timeout \bind \sendpipeline
\endpipeline

-- REINDEX CONCURRENTLY fails if not the first command in a pipeline.
\startpipeline
SELECT $1 \bind 1 \sendpipeline
REINDEX TABLE CONCURRENTLY psql_pipeline \bind \sendpipeline
SELECT $1 \bind 2 \sendpipeline
\endpipeline

-- REINDEX CONCURRENTLY works if it is the first command in a pipeline.
\startpipeline
REINDEX TABLE CONCURRENTLY psql_pipeline \bind \sendpipeline
SELECT $1 \bind 2 \sendpipeline
\endpipeline

-- Subtransactions are not allowed in a pipeline.
\startpipeline
SAVEPOINT a \bind \sendpipeline
SELECT $1 \bind 1 \sendpipeline
ROLLBACK TO SAVEPOINT a \bind \sendpipeline
SELECT $1 \bind 2 \sendpipeline
\endpipeline

-- LOCK fails as the first command in a pipeline, as not seen in an
-- implicit transaction block.
\startpipeline
LOCK psql_pipeline \bind \sendpipeline
SELECT $1 \bind 2 \sendpipeline
\endpipeline

-- LOCK succeeds as it is not the first command in a pipeline,
-- seen in an implicit transaction block.
\startpipeline
SELECT $1 \bind 1 \sendpipeline
LOCK psql_pipeline \bind \sendpipeline
SELECT $1 \bind 2 \sendpipeline
\endpipeline

-- VACUUM works as the first command in a pipeline.
\startpipeline
VACUUM psql_pipeline \bind \sendpipeline
\endpipeline

-- VACUUM fails when not the first command in a pipeline.
\startpipeline
SELECT 1 \bind \sendpipeline
VACUUM psql_pipeline \bind \sendpipeline
\endpipeline

-- VACUUM works after a \syncpipeline.
\startpipeline
SELECT 1 \bind \sendpipeline
\syncpipeline
VACUUM psql_pipeline \bind \sendpipeline
\endpipeline

-- Clean up
DROP TABLE psql_pipeline;
