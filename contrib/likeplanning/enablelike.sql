-- This script enables use of the new LIKE-related selectivity estimation
-- functions, which are a little too new to be enabled by default in 7.0.
-- You can disable them again by running disablelike.sql.

-- Use of the functions will be enabled only in those databases you
-- run this script in.  If you run it in template1,
-- all subsequently-created databases will use the functions.

-- Be sure to run the script as the Postgres superuser!

UPDATE pg_operator SET
	oprrest = 'regexeqsel'::regproc,
	oprjoin = 'regexeqjoinsel'::regproc
WHERE oprrest = 'eqsel'::regproc AND oprname = '~';

UPDATE pg_operator SET
	oprrest = 'icregexeqsel'::regproc,
	oprjoin = 'icregexeqjoinsel'::regproc
WHERE oprrest = 'eqsel'::regproc AND oprname = '~*';

UPDATE pg_operator SET
	oprrest = 'likesel'::regproc,
	oprjoin = 'likejoinsel'::regproc
WHERE oprrest = 'eqsel'::regproc AND oprname = '~~';

UPDATE pg_operator SET
	oprrest = 'regexnesel'::regproc,
	oprjoin = 'regexnejoinsel'::regproc
WHERE oprrest = 'neqsel'::regproc AND oprname = '!~';

UPDATE pg_operator SET
	oprrest = 'icregexnesel'::regproc,
	oprjoin = 'icregexnejoinsel'::regproc
WHERE oprrest = 'neqsel'::regproc AND oprname = '!~*';

UPDATE pg_operator SET
	oprrest = 'nlikesel'::regproc,
	oprjoin = 'nlikejoinsel'::regproc
WHERE oprrest = 'neqsel'::regproc AND oprname = '!~~';
