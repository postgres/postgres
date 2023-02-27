--
-- LINE
-- Infinite lines
--

--DROP TABLE LINE_TBL;
CREATE TABLE LINE_TBL (s line);

INSERT INTO LINE_TBL VALUES ('{0,-1,5}');	-- A == 0
INSERT INTO LINE_TBL VALUES ('{1,0,5}');	-- B == 0
INSERT INTO LINE_TBL VALUES ('{0,3,0}');	-- A == C == 0
INSERT INTO LINE_TBL VALUES (' (0,0), (6,6)');
INSERT INTO LINE_TBL VALUES ('10,-10 ,-5,-4');
INSERT INTO LINE_TBL VALUES ('[-1e6,2e2,3e5, -4e1]');

INSERT INTO LINE_TBL VALUES ('{3,NaN,5}');
INSERT INTO LINE_TBL VALUES ('{NaN,NaN,NaN}');

-- horizontal
INSERT INTO LINE_TBL VALUES ('[(1,3),(2,3)]');
-- vertical
INSERT INTO LINE_TBL VALUES (line(point '(3,1)', point '(3,2)'));

-- bad values for parser testing
INSERT INTO LINE_TBL VALUES ('{}');
INSERT INTO LINE_TBL VALUES ('{0');
INSERT INTO LINE_TBL VALUES ('{0,0}');
INSERT INTO LINE_TBL VALUES ('{0,0,1');
INSERT INTO LINE_TBL VALUES ('{0,0,1}');
INSERT INTO LINE_TBL VALUES ('{0,0,1} x');
INSERT INTO LINE_TBL VALUES ('(3asdf,2 ,3,4r2)');
INSERT INTO LINE_TBL VALUES ('[1,2,3, 4');
INSERT INTO LINE_TBL VALUES ('[(,2),(3,4)]');
INSERT INTO LINE_TBL VALUES ('[(1,2),(3,4)');
INSERT INTO LINE_TBL VALUES ('[(1,2),(1,2)]');

INSERT INTO LINE_TBL VALUES (line(point '(1,0)', point '(1,0)'));

select * from LINE_TBL;

select '{nan, 1, nan}'::line = '{nan, 1, nan}'::line as true,
	   '{nan, 1, nan}'::line = '{nan, 2, nan}'::line as false;

-- test non-error-throwing API for some core types
SELECT pg_input_is_valid('{1, 1}', 'line');
SELECT * FROM pg_input_error_info('{1, 1}', 'line');
SELECT pg_input_is_valid('{0, 0, 0}', 'line');
SELECT * FROM pg_input_error_info('{0, 0, 0}', 'line');
SELECT pg_input_is_valid('{1, 1, a}', 'line');
SELECT * FROM pg_input_error_info('{1, 1, a}', 'line');
SELECT pg_input_is_valid('{1, 1, 1e400}', 'line');
SELECT * FROM pg_input_error_info('{1, 1, 1e400}', 'line');
SELECT pg_input_is_valid('(1, 1), (1, 1e400)', 'line');
SELECT * FROM pg_input_error_info('(1, 1), (1, 1e400)', 'line');
