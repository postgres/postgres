--
-- CREATE_VIEW
-- Virtual class definitions
--	(this also tests the query rewrite system)
--

CREATE VIEW street AS
   SELECT r.name, r.thepath, c.cname AS cname 
   FROM ONLY road r, real_city c
   WHERE c.outline ## r.thepath;

CREATE VIEW iexit AS
   SELECT ih.name, ih.thepath, 
	interpt_pp(ih.thepath, r.thepath) AS exit
   FROM ihighway ih, ramp r
   WHERE ih.thepath ## r.thepath;

CREATE VIEW toyemp AS
   SELECT name, age, location, 12*salary AS annualsal
   FROM emp;

