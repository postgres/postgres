-- This script loads pg_proc entries for the 7.0 selectivity estimation
-- functions into a 7.0beta5 database.  You should not run it if you
-- initdb'd with 7.0RC1 or later.  If you do need it, run it in each
-- database you have, including template1.  Once you have run it in
-- template1, all subsequently-created databases will contain the entries,
-- so you won't need to run it again.
-- Be sure to run the script as the Postgres superuser!

COPY pg_proc WITH OIDS FROM stdin;
1818	regexeqsel	0	11	f	t	f	5	f	701	26 26 21 0 23	100	0	0	100	regexeqsel	-
1819	likesel	0	11	f	t	f	5	f	701	26 26 21 0 23	100	0	0	100	likesel	-
1820	icregexeqsel	0	11	f	t	f	5	f	701	26 26 21 0 23	100	0	0	100	icregexeqsel	-
1821	regexnesel	0	11	f	t	f	5	f	701	26 26 21 0 23	100	0	0	100	regexnesel	-
1822	nlikesel	0	11	f	t	f	5	f	701	26 26 21 0 23	100	0	0	100	nlikesel	-
1823	icregexnesel	0	11	f	t	f	5	f	701	26 26 21 0 23	100	0	0	100	icregexnesel	-
1824	regexeqjoinsel	0	11	f	t	f	5	f	701	26 26 21 26 21	100	0	0	100	regexeqjoinsel	-
1825	likejoinsel	0	11	f	t	f	5	f	701	26 26 21 26 21	100	0	0	100	likejoinsel	-
1826	icregexeqjoinsel	0	11	f	t	f	5	f	701	26 26 21 26 21	100	0	0	100	icregexeqjoinsel	-
1827	regexnejoinsel	0	11	f	t	f	5	f	701	26 26 21 26 21	100	0	0	100	regexnejoinsel	-
1828	nlikejoinsel	0	11	f	t	f	5	f	701	26 26 21 26 21	100	0	0	100	nlikejoinsel	-
1829	icregexnejoinsel	0	11	f	t	f	5	f	701	26 26 21 26 21	100	0	0	100	icregexnejoinsel	-
\.

UPDATE pg_proc SET proowner = pg_shadow.usesysid
WHERE oid >= 1818 AND oid <= 1829 AND pg_shadow.usename = CURRENT_USER;
