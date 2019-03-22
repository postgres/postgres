CREATE EXTENSION test_integerset;

--
-- These tests don't produce any interesting output.  We're checking that
-- the operations complete without crashing or hanging and that none of their
-- internal sanity tests fail.  They print progress information as INFOs,
-- which are not interesting for automated tests, so suppress those.
--
SET client_min_messages = 'warning';

SELECT test_integerset();
