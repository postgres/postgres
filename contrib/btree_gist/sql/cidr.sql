-- cidr check

CREATE TABLE cidrtmp AS
  SELECT cidr(a) AS a FROM inettmp ;

SET enable_seqscan=on;

SELECT count(*) FROM cidrtmp WHERE a <  '121.111.63.82';

SELECT count(*) FROM cidrtmp WHERE a <= '121.111.63.82';

SELECT count(*) FROM cidrtmp WHERE a  = '121.111.63.82';

SELECT count(*) FROM cidrtmp WHERE a >= '121.111.63.82';

SELECT count(*) FROM cidrtmp WHERE a >  '121.111.63.82';

SET client_min_messages = DEBUG1;
CREATE INDEX cidridx ON cidrtmp USING gist ( a );
CREATE INDEX cidridx_b ON cidrtmp USING gist ( a ) WITH (buffering=on);
DROP INDEX cidridx_b;
RESET client_min_messages;

SET enable_seqscan=off;

SELECT count(*) FROM cidrtmp WHERE a <  '121.111.63.82'::cidr;

SELECT count(*) FROM cidrtmp WHERE a <= '121.111.63.82'::cidr;

SELECT count(*) FROM cidrtmp WHERE a  = '121.111.63.82'::cidr;

SELECT count(*) FROM cidrtmp WHERE a >= '121.111.63.82'::cidr;

SELECT count(*) FROM cidrtmp WHERE a >  '121.111.63.82'::cidr;
