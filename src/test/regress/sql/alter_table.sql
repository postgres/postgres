--
-- add attribute
--
CREATE TABLE temp (initial int4);

ALTER TABLE temp ADD COLUMN a int4;

ALTER TABLE temp ADD COLUMN b name;

ALTER TABLE temp ADD COLUMN c text;

ALTER TABLE temp ADD COLUMN d float8;

ALTER TABLE temp ADD COLUMN e float4;

ALTER TABLE temp ADD COLUMN f int2;

ALTER TABLE temp ADD COLUMN g polygon;

ALTER TABLE temp ADD COLUMN h abstime;

ALTER TABLE temp ADD COLUMN i char;

ALTER TABLE temp ADD COLUMN j abstime[];

ALTER TABLE temp ADD COLUMN k dt;

ALTER TABLE temp ADD COLUMN l tid;

ALTER TABLE temp ADD COLUMN m xid;

ALTER TABLE temp ADD COLUMN n oid8;

--ALTER TABLE temp ADD COLUMN o lock;
ALTER TABLE temp ADD COLUMN p smgr;

ALTER TABLE temp ADD COLUMN q point;

ALTER TABLE temp ADD COLUMN r lseg;

ALTER TABLE temp ADD COLUMN s path;

ALTER TABLE temp ADD COLUMN t box;

ALTER TABLE temp ADD COLUMN u tinterval;

ALTER TABLE temp ADD COLUMN v oidint4;

ALTER TABLE temp ADD COLUMN w oidname;

ALTER TABLE temp ADD COLUMN x float8[];

ALTER TABLE temp ADD COLUMN y float4[];

ALTER TABLE temp ADD COLUMN z int2[];

INSERT INTO temp (a, b, c, d, e, f, g, h, i, j, k, l, m, n, p, q, r, s, t, u,
	v, w, x, y, z)
   VALUES (4, 'name', 'text', 4.1, 4.1, 2, '(4.1,4.1,3.1,3.1)', 
        'Mon May  1 00:30:30 1995', 'c', '{Mon May  1 00:30:30 1995, Monday Aug 24 14:43:07 1992, epoch}', 
	314159, '(1,1)', 512,
	'1 2 3 4 5 6 7 8', 'magnetic disk', '(1.1,1.1)', '(4.1,4.1,3.1,3.1)',
	'(0,2,4.1,4.1,3.1,3.1)', '(4.1,4.1,3.1,3.1)', '["current" "infinity"]',
	'1/3', '1,name', '{1.0,2.0,3.0,4.0}', '{1.0,2.0,3.0,4.0}', '{1,2,3,4}');

SELECT * FROM temp;

DROP TABLE temp;

-- the wolf bug - schema mods caused inconsistent row descriptors 
CREATE TABLE temp (
	initial 	int4
);

ALTER TABLE temp ADD COLUMN a int4;

ALTER TABLE temp ADD COLUMN b name;

ALTER TABLE temp ADD COLUMN c text;

ALTER TABLE temp ADD COLUMN d float8;

ALTER TABLE temp ADD COLUMN e float4;

ALTER TABLE temp ADD COLUMN f int2;

ALTER TABLE temp ADD COLUMN g polygon;

ALTER TABLE temp ADD COLUMN h abstime;

ALTER TABLE temp ADD COLUMN i char;

ALTER TABLE temp ADD COLUMN j abstime[];

ALTER TABLE temp ADD COLUMN k dt;

ALTER TABLE temp ADD COLUMN l tid;

ALTER TABLE temp ADD COLUMN m xid;

ALTER TABLE temp ADD COLUMN n oid8;

--ALTER TABLE temp ADD COLUMN o lock;
ALTER TABLE temp ADD COLUMN p smgr;

ALTER TABLE temp ADD COLUMN q point;

ALTER TABLE temp ADD COLUMN r lseg;

ALTER TABLE temp ADD COLUMN s path;

ALTER TABLE temp ADD COLUMN t box;

ALTER TABLE temp ADD COLUMN u tinterval;

ALTER TABLE temp ADD COLUMN v oidint4;

ALTER TABLE temp ADD COLUMN w oidname;

ALTER TABLE temp ADD COLUMN x float8[];

ALTER TABLE temp ADD COLUMN y float4[];

ALTER TABLE temp ADD COLUMN z int2[];

INSERT INTO temp (a, b, c, d, e, f, g, h, i, j, k, l, m, n, p, q, r, s, t, u,
	v, w, x, y, z)
   VALUES (4, 'name', 'text', 4.1, 4.1, 2, '(4.1,4.1,3.1,3.1)', 
	'Mon May  1 00:30:30 1995', 'c', '{Mon May  1 00:30:30 1995, Monday Aug 24 14:43:07 1992, epoch}',
	 314159, '(1,1)', 512,
	'1 2 3 4 5 6 7 8', 'magnetic disk', '(1.1,1.1)', '(4.1,4.1,3.1,3.1)',
	'(0,2,4.1,4.1,3.1,3.1)', '(4.1,4.1,3.1,3.1)', '["current" "infinity"]',
	'1/3', '1,name', '{1.0,2.0,3.0,4.0}', '{1.0,2.0,3.0,4.0}', '{1,2,3,4}');

SELECT * FROM temp;

DROP TABLE temp;


--
-- rename -
--   should preserve indices
--
ALTER TABLE tenk1 RENAME TO ten_k;

-- 20 values, sorted 
SELECT unique1 FROM ten_k WHERE unique1 < 20;

-- 20 values, sorted 
SELECT unique2 FROM ten_k WHERE unique2 < 20;

-- 100 values, sorted 
SELECT hundred FROM ten_k WHERE hundred = 50;

ALTER TABLE ten_k RENAME TO tenk1;

-- 5 values, sorted 
SELECT unique1 FROM tenk1 WHERE unique1 < 5;

