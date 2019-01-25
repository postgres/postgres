---------------------------------------------------------------------------
--
-- advanced.sql-
--    Tutorial on advanced PostgreSQL features
--
--
-- Copyright (c) 1994, Regents of the University of California
--
-- src/tutorial/advanced.source
--
---------------------------------------------------------------------------

-----------------------------
-- Inheritance:
--	A table can inherit from zero or more tables.  A query can reference
--	either all rows of a table or all rows of a table plus all of its
--	descendants.
-----------------------------

-- For example, the capitals table inherits from cities table. (It inherits
-- all data fields from cities.)

CREATE TABLE cities (
	name		text,
	population	float8,
	altitude	int		-- (in ft)
);

CREATE TABLE capitals (
	state		char(2)
) INHERITS (cities);

-- Now, let's populate the tables.
INSERT INTO cities VALUES ('San Francisco', 7.24E+5, 63);
INSERT INTO cities VALUES ('Las Vegas', 2.583E+5, 2174);
INSERT INTO cities VALUES ('Mariposa', 1200, 1953);

INSERT INTO capitals VALUES ('Sacramento', 3.694E+5, 30, 'CA');
INSERT INTO capitals VALUES ('Madison', 1.913E+5, 845, 'WI');

SELECT * FROM cities;
SELECT * FROM capitals;

-- You can find all cities, including capitals, that
-- are located at an altitude of 500 ft or higher by:

SELECT c.name, c.altitude
FROM cities c
WHERE c.altitude > 500;

-- To scan rows of the parent table only, use ONLY:

SELECT name, altitude
FROM ONLY cities
WHERE altitude > 500;


-- clean up (you must remove the children first)
DROP TABLE capitals;
DROP TABLE cities;
