-- This script disables use of the new LIKE-related selectivity estimation
-- functions, which are a little too new to be enabled by default in 7.0.
-- You can enable them again by running enablelike.sql.

-- Use of the functions will be disabled only in those databases you
-- run this script in.  If you run it in template1,
-- all subsequently-created databases will not use the functions.

-- Be sure to run the script as the Postgres superuser!

UPDATE pg_operator SET
	oprrest = 'eqsel'::regproc,
	oprjoin = 'eqjoinsel'::regproc
WHERE oprrest = 'regexeqsel'::regproc;

UPDATE pg_operator SET
	oprrest = 'eqsel'::regproc,
	oprjoin = 'eqjoinsel'::regproc
WHERE oprrest = 'icregexeqsel'::regproc;

UPDATE pg_operator SET
	oprrest = 'eqsel'::regproc,
	oprjoin = 'eqjoinsel'::regproc
WHERE oprrest = 'likesel'::regproc;

UPDATE pg_operator SET
	oprrest = 'neqsel'::regproc,
	oprjoin = 'neqjoinsel'::regproc
WHERE oprrest = 'regexnesel'::regproc;

UPDATE pg_operator SET
	oprrest = 'neqsel'::regproc,
	oprjoin = 'neqjoinsel'::regproc
WHERE oprrest = 'icregexnesel'::regproc;

UPDATE pg_operator SET
	oprrest = 'neqsel'::regproc,
	oprjoin = 'neqjoinsel'::regproc
WHERE oprrest = 'nlikesel'::regproc;
