--
-- PATH
--

--DROP TABLE PATH_TBL;

CREATE TABLE PATH_TBL (f1 path);

INSERT INTO PATH_TBL VALUES ('[(1,2),(3,4)]');

INSERT INTO PATH_TBL VALUES ('((1,2),(3,4))');

INSERT INTO PATH_TBL VALUES ('[(0,0),(3,0),(4,5),(1,6)]');

INSERT INTO PATH_TBL VALUES ('((1,2),(3,4))');

INSERT INTO PATH_TBL VALUES ('1,2 ,3,4');

INSERT INTO PATH_TBL VALUES ('[1,2,3, 4]');

INSERT INTO PATH_TBL VALUES ('[11,12,13,14]');

INSERT INTO PATH_TBL VALUES ('(11,12,13,14)');

-- bad values for parser testing
INSERT INTO PATH_TBL VALUES ('[(,2),(3,4)]');

INSERT INTO PATH_TBL VALUES ('[(1,2),(3,4)');

SELECT f1 FROM PATH_TBL;

SELECT '' AS count, f1 AS open_path FROM PATH_TBL WHERE isopen(f1);

SELECT '' AS count, f1 AS closed_path FROM PATH_TBL WHERE isclosed(f1);

SELECT '' AS count, pclose(f1) AS closed_path FROM PATH_TBL;

SELECT '' AS count, popen(f1) AS open_path FROM PATH_TBL;
