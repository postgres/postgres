--
-- LINE
-- Infinite lines
--

--DROP TABLE LINE_TBL;
CREATE TABLE LINE_TBL (s line);

INSERT INTO LINE_TBL VALUES ('{1,-1,1}');
INSERT INTO LINE_TBL VALUES ('(0,0),(6,6)');
INSERT INTO LINE_TBL VALUES ('10,-10 ,-5,-4');
INSERT INTO LINE_TBL VALUES ('[-1e6,2e2,3e5, -4e1]');
INSERT INTO LINE_TBL VALUES ('(11,22,33,44)');

INSERT INTO LINE_TBL VALUES ('[(1,0),(1,0)]');

-- horizontal
INSERT INTO LINE_TBL VALUES ('[(1,3),(2,3)]');
-- vertical
INSERT INTO LINE_TBL VALUES ('[(3,1),(3,2)]');

-- bad values for parser testing
INSERT INTO LINE_TBL VALUES ('{0,0,1}');
INSERT INTO LINE_TBL VALUES ('(3asdf,2 ,3,4r2)');
INSERT INTO LINE_TBL VALUES ('[1,2,3, 4');
INSERT INTO LINE_TBL VALUES ('[(,2),(3,4)]');
INSERT INTO LINE_TBL VALUES ('[(1,2),(3,4)');

select * from LINE_TBL;


-- functions and operators

SELECT * FROM LINE_TBL WHERE (s <-> line '[(1,2),(3,4)]') < 10;

SELECT * FROM LINE_TBL WHERE (point '(0.1,0.1)' <-> s) < 1;

SELECT * FROM LINE_TBL WHERE (lseg '[(0.1,0.1),(0.2,0.2)]' <-> s) < 1;

SELECT line '[(1,1),(2,1)]' <-> line '[(-1,-1),(-2,-1)]';
SELECT lseg '[(1,1),(2,1)]' <-> line '[(-1,-1),(-2,-1)]';
SELECT point '(-1,1)' <-> line '[(-3,0),(-4,0)]';

SELECT lseg '[(1,1),(5,5)]' ?# line '[(2,0),(0,2)]';  -- true
SELECT lseg '[(1,1),(5,5)]' ?# line '[(0,0),(1,0)]';  -- false

SELECT line '[(0,0),(1,1)]' ?# box '(0,0,2,2)';  -- true
SELECT line '[(3,0),(4,1)]' ?# box '(0,0,2,2)';  -- false

SELECT point '(1,1)' <@ line '[(0,0),(2,2)]';  -- true
SELECT point '(1,1)' <@ line '[(0,0),(1,0)]';  -- false

SELECT point '(1,1)' @ line '[(0,0),(2,2)]';  -- true
SELECT point '(1,1)' @ line '[(0,0),(1,0)]';  -- false

SELECT lseg '[(1,1),(2,2)]' <@ line '[(0,0),(2,2)]';  -- true
SELECT lseg '[(1,1),(2,1)]' <@ line '[(0,0),(1,0)]';  -- false

SELECT lseg '[(1,1),(2,2)]' @ line '[(0,0),(2,2)]';  -- true
SELECT lseg '[(1,1),(2,1)]' @ line '[(0,0),(1,0)]';  -- false

SELECT point '(0,1)' ## line '[(0,0),(1,1)]';

SELECT line '[(0,0),(1,1)]' ## lseg '[(1,0),(2,0)]';

SELECT line '[(0,0),(1,1)]' ?# line '[(1,0),(2,1)]';  -- false
SELECT line '[(0,0),(1,1)]' ?# line '[(1,0),(1,1)]';  -- true

SELECT line '[(0,0),(1,1)]' # line '[(1,0),(2,1)]';
SELECT line '[(0,0),(1,1)]' # line '[(1,0),(1,1)]';

SELECT line '[(0,0),(1,1)]' ?|| line '[(1,0),(2,1)]';  -- true
SELECT line '[(0,0),(1,1)]' ?|| line '[(1,0),(1,1)]';  -- false

SELECT line '[(0,0),(1,0)]' ?-| line '[(0,0),(0,1)]';  -- true
SELECT line '[(0,0),(1,1)]' ?-| line '[(1,0),(1,1)]';  -- false

SELECT ?- line '[(0,0),(1,0)]';  -- true
SELECT ?- line '[(0,0),(1,1)]';  -- false

SELECT ?| line '[(0,0),(0,1)]';  -- true
SELECT ?| line '[(0,0),(1,1)]';  -- false

SELECT line(point '(1,2)', point '(3,4)');

SELECT line '[(1,2),(3,4)]' = line '[(3,4),(4,5)]';  -- true
SELECT line '[(1,2),(3,4)]' = line '[(3,4),(4,4)]';  -- false
