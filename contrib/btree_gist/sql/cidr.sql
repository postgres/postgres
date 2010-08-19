-- cidr check

CREATE TABLE cidrtmp AS
  SELECT cidr(a) AS a FROM inettmp ;

SET enable_seqscan=on;

SELECT count(*) FROM cidrtmp WHERE a <  '121.111.63.82';

SELECT count(*) FROM cidrtmp WHERE a <= '121.111.63.82';

SELECT count(*) FROM cidrtmp WHERE a  = '121.111.63.82';

SELECT count(*) FROM cidrtmp WHERE a >= '121.111.63.82';

SELECT count(*) FROM cidrtmp WHERE a >  '121.111.63.82';

CREATE INDEX cidridx ON cidrtmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM cidrtmp WHERE a <  '121.111.63.82'::cidr;

SELECT count(*) FROM cidrtmp WHERE a <= '121.111.63.82'::cidr;

SELECT count(*) FROM cidrtmp WHERE a  = '121.111.63.82'::cidr;

SELECT count(*) FROM cidrtmp WHERE a >= '121.111.63.82'::cidr;

SELECT count(*) FROM cidrtmp WHERE a >  '121.111.63.82'::cidr;
