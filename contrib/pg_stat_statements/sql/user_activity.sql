--
-- Track user activity and reset them
--

SET pg_stat_statements.track_utility = TRUE;
SELECT pg_stat_statements_reset();
CREATE ROLE regress_stats_user1;
CREATE ROLE regress_stats_user2;

SET ROLE regress_stats_user1;

SELECT 1 AS "ONE";
SELECT 1+1 AS "TWO";

RESET ROLE;
SET ROLE regress_stats_user2;

SELECT 1 AS "ONE";
SELECT 1+1 AS "TWO";

RESET ROLE;
SELECT query, calls, rows FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- Don't reset anything if any of the parameter is NULL
--
SELECT pg_stat_statements_reset(NULL);
SELECT query, calls, rows FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- remove query ('SELECT $1+$2 AS "TWO"') executed by regress_stats_user2
-- in the current_database
--
SELECT pg_stat_statements_reset(
	(SELECT r.oid FROM pg_roles AS r WHERE r.rolname = 'regress_stats_user2'),
	(SELECT d.oid FROM pg_database As d where datname = current_database()),
	(SELECT s.queryid FROM pg_stat_statements AS s
				WHERE s.query = 'SELECT $1+$2 AS "TWO"' LIMIT 1));
SELECT query, calls, rows FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- remove query ('SELECT $1 AS "ONE"') executed by two users
--
SELECT pg_stat_statements_reset(0,0,s.queryid)
	FROM pg_stat_statements AS s WHERE s.query = 'SELECT $1 AS "ONE"';
SELECT query, calls, rows FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- remove query of a user (regress_stats_user1)
--
SELECT pg_stat_statements_reset(r.oid)
		FROM pg_roles AS r WHERE r.rolname = 'regress_stats_user1';
SELECT query, calls, rows FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- reset all
--
SELECT pg_stat_statements_reset(0,0,0);
SELECT query, calls, rows FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- cleanup
--
DROP ROLE regress_stats_user1;
DROP ROLE regress_stats_user2;
SELECT pg_stat_statements_reset();
