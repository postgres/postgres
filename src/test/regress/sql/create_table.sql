--
-- CREATE_TABLE
--

--
-- CLASS DEFINITIONS
--
CREATE TABLE hobbies_r (
	name		text,
	person 		text
);

CREATE TABLE equipment_r (
	name 		text,
	hobby		text
);

CREATE TABLE onek (
	unique1		int4,
	unique2		int4,
	two			int4,
	four		int4,
	ten			int4,
	twenty		int4,
	hundred		int4,
	thousand	int4,
	twothousand	int4,
	fivethous	int4,
	tenthous	int4,
	odd			int4,
	even		int4,
	stringu1	name,
	stringu2	name,
	string4		name
);

CREATE TABLE tenk1 (
	unique1		int4,
	unique2		int4,
	two			int4,
	four		int4,
	ten			int4,
	twenty		int4,
	hundred		int4,
	thousand	int4,
	twothousand	int4,
	fivethous	int4,
	tenthous	int4,
	odd			int4,
	even		int4,
	stringu1	name,
	stringu2	name,
	string4		name
) WITH OIDS;

CREATE TABLE tenk2 (
	unique1 	int4,
	unique2 	int4,
	two 	 	int4,
	four 		int4,
	ten			int4,
	twenty 		int4,
	hundred 	int4,
	thousand 	int4,
	twothousand int4,
	fivethous 	int4,
	tenthous	int4,
	odd			int4,
	even		int4,
	stringu1	name,
	stringu2	name,
	string4		name
);


CREATE TABLE person (
	name 		text,
	age			int4,
	location 	point
);


CREATE TABLE emp (
	salary 		int4,
	manager 	name
) INHERITS (person) WITH OIDS;


CREATE TABLE student (
	gpa 		float8
) INHERITS (person);


CREATE TABLE stud_emp (
	percent 	int4
) INHERITS (emp, student);


CREATE TABLE city (
	name		name,
	location 	box,
	budget 		city_budget
);

CREATE TABLE dept (
	dname		name,
	mgrname 	text
);

CREATE TABLE slow_emp4000 (
	home_base	 box
);

CREATE TABLE fast_emp4000 (
	home_base	 box
);

CREATE TABLE road (
	name		text,
	thepath 	path
);

CREATE TABLE ihighway () INHERITS (road);

CREATE TABLE shighway (
	surface		text
) INHERITS (road);

CREATE TABLE real_city (
	pop			int4,
	cname		text,
	outline 	path
);

--
-- test the "star" operators a bit more thoroughly -- this time,
-- throw in lots of NULL fields...
--
-- a is the type root
-- b and c inherit from a (one-level single inheritance)
-- d inherits from b and c (two-level multiple inheritance)
-- e inherits from c (two-level single inheritance)
-- f inherits from e (three-level single inheritance)
--
CREATE TABLE a_star (
	class		char,
	a 			int4
);

CREATE TABLE b_star (
	b 			text
) INHERITS (a_star);

CREATE TABLE c_star (
	c 			name
) INHERITS (a_star);

CREATE TABLE d_star (
	d 			float8
) INHERITS (b_star, c_star);

CREATE TABLE e_star (
	e 			int2
) INHERITS (c_star);

CREATE TABLE f_star (
	f 			polygon
) INHERITS (e_star);

CREATE TABLE aggtest (
	a 			int2,
	b			float4
);

CREATE TABLE hash_i4_heap (
	seqno 		int4,
	random 		int4
);

CREATE TABLE hash_name_heap (
	seqno 		int4,
	random 		name
);

CREATE TABLE hash_txt_heap (
	seqno 		int4,
	random 		text
);

CREATE TABLE hash_f8_heap (
	seqno		int4,
	random 		float8
);

-- don't include the hash_ovfl_heap stuff in the distribution
-- the data set is too large for what it's worth
--
-- CREATE TABLE hash_ovfl_heap (
--	x			int4,
--	y			int4
-- );

CREATE TABLE bt_i4_heap (
	seqno 		int4,
	random 		int4
);

CREATE TABLE bt_name_heap (
	seqno 		name,
	random 		int4
);

CREATE TABLE bt_txt_heap (
	seqno 		text,
	random 		int4
);

CREATE TABLE bt_f8_heap (
	seqno 		float8,
	random 		int4
);

CREATE TABLE array_op_test (
	seqno		int4,
	i			int4[],
	t			text[]
);

CREATE TABLE array_index_op_test (
	seqno		int4,
	i			int4[],
	t			text[]
);

CREATE TABLE testjsonb (
       j jsonb
);

CREATE TABLE unknowntab (
	u unknown    -- fail
);

CREATE TYPE unknown_comptype AS (
	u unknown    -- fail
);

CREATE TABLE IF NOT EXISTS test_tsvector(
	t text,
	a tsvector
);

CREATE TABLE IF NOT EXISTS test_tsvector(
	t text
);

CREATE UNLOGGED TABLE unlogged1 (a int primary key);			-- OK
CREATE TEMPORARY TABLE unlogged2 (a int primary key);			-- OK
SELECT relname, relkind, relpersistence FROM pg_class WHERE relname ~ '^unlogged\d' ORDER BY relname;
REINDEX INDEX unlogged1_pkey;
REINDEX INDEX unlogged2_pkey;
SELECT relname, relkind, relpersistence FROM pg_class WHERE relname ~ '^unlogged\d' ORDER BY relname;
DROP TABLE unlogged2;
INSERT INTO unlogged1 VALUES (42);
CREATE UNLOGGED TABLE public.unlogged2 (a int primary key);		-- also OK
CREATE UNLOGGED TABLE pg_temp.unlogged3 (a int primary key);	-- not OK
CREATE TABLE pg_temp.implicitly_temp (a int primary key);		-- OK
CREATE TEMP TABLE explicitly_temp (a int primary key);			-- also OK
CREATE TEMP TABLE pg_temp.doubly_temp (a int primary key);		-- also OK
CREATE TEMP TABLE public.temp_to_perm (a int primary key);		-- not OK
DROP TABLE unlogged1, public.unlogged2;

CREATE TABLE as_select1 AS SELECT * FROM pg_class WHERE relkind = 'r';
CREATE TABLE as_select1 AS SELECT * FROM pg_class WHERE relkind = 'r';
CREATE TABLE IF NOT EXISTS as_select1 AS SELECT * FROM pg_class WHERE relkind = 'r';
DROP TABLE as_select1;

-- check that the oid column is added before the primary key is checked
CREATE TABLE oid_pk (f1 INT, PRIMARY KEY(oid)) WITH OIDS;
DROP TABLE oid_pk;

--
-- Partitioned tables
--

-- cannot combine INHERITS and PARTITION BY (although grammar allows)
CREATE TABLE partitioned (
	a int
) INHERITS (some_table) PARTITION BY LIST (a);

-- cannot use more than 1 column as partition key for list partitioned table
CREATE TABLE partitioned (
	a1 int,
	a2 int
) PARTITION BY LIST (a1, a2);	-- fail

-- unsupported constraint type for partitioned tables
CREATE TABLE partitioned (
	a int PRIMARY KEY
) PARTITION BY RANGE (a);

CREATE TABLE pkrel (
	a int PRIMARY KEY
);
CREATE TABLE partitioned (
	a int REFERENCES pkrel(a)
) PARTITION BY RANGE (a);
DROP TABLE pkrel;

CREATE TABLE partitioned (
	a int UNIQUE
) PARTITION BY RANGE (a);

CREATE TABLE partitioned (
	a int,
	EXCLUDE USING gist (a WITH &&)
) PARTITION BY RANGE (a);

-- prevent column from being used twice in the partition key
CREATE TABLE partitioned (
	a int
) PARTITION BY RANGE (a, a);

-- prevent using prohibited expressions in the key
CREATE FUNCTION retset (a int) RETURNS SETOF int AS $$ SELECT 1; $$ LANGUAGE SQL IMMUTABLE;
CREATE TABLE partitioned (
	a int
) PARTITION BY RANGE (retset(a));
DROP FUNCTION retset(int);

CREATE TABLE partitioned (
	a int
) PARTITION BY RANGE ((avg(a)));

CREATE TABLE partitioned (
	a int,
	b int
) PARTITION BY RANGE ((avg(a) OVER (PARTITION BY b)));

CREATE TABLE partitioned (
	a int
) PARTITION BY LIST ((a LIKE (SELECT 1)));

CREATE TABLE partitioned (
	a int
) PARTITION BY RANGE (('a'));

CREATE FUNCTION const_func () RETURNS int AS $$ SELECT 1; $$ LANGUAGE SQL IMMUTABLE;
CREATE TABLE partitioned (
	a int
) PARTITION BY RANGE (const_func());
DROP FUNCTION const_func();

-- only accept "list" and "range" as partitioning strategy
CREATE TABLE partitioned (
	a int
) PARTITION BY HASH (a);

-- specified column must be present in the table
CREATE TABLE partitioned (
	a int
) PARTITION BY RANGE (b);

-- cannot use system columns in partition key
CREATE TABLE partitioned (
	a int
) PARTITION BY RANGE (xmin);

-- functions in key must be immutable
CREATE FUNCTION immut_func (a int) RETURNS int AS $$ SELECT a + random()::int; $$ LANGUAGE SQL;
CREATE TABLE partitioned (
	a int
) PARTITION BY RANGE (immut_func(a));
DROP FUNCTION immut_func(int);

-- cannot contain whole-row references
CREATE TABLE partitioned (
	a	int
) PARTITION BY RANGE ((partitioned));

-- prevent using columns of unsupported types in key (type must have a btree operator class)
CREATE TABLE partitioned (
	a point
) PARTITION BY LIST (a);
CREATE TABLE partitioned (
	a point
) PARTITION BY LIST (a point_ops);
CREATE TABLE partitioned (
	a point
) PARTITION BY RANGE (a);
CREATE TABLE partitioned (
	a point
) PARTITION BY RANGE (a point_ops);

-- cannot add NO INHERIT constraints to partitioned tables
CREATE TABLE partitioned (
	a int,
	CONSTRAINT check_a CHECK (a > 0) NO INHERIT
) PARTITION BY RANGE (a);

-- some checks after successful creation of a partitioned table
CREATE FUNCTION plusone(a int) RETURNS INT AS $$ SELECT a+1; $$ LANGUAGE SQL;

CREATE TABLE partitioned (
	a int,
	b int,
	c text,
	d text
) PARTITION BY RANGE (a oid_ops, plusone(b), c collate "default", d collate "C");

-- check relkind
SELECT relkind FROM pg_class WHERE relname = 'partitioned';

-- check that range partition key columns are marked NOT NULL
SELECT attname, attnotnull FROM pg_attribute
  WHERE attrelid = 'partitioned'::regclass AND attnum > 0
  ORDER BY attnum;

-- prevent a function referenced in partition key from being dropped
DROP FUNCTION plusone(int);

-- partitioned table cannot partiticipate in regular inheritance
CREATE TABLE partitioned2 (
	a int
) PARTITION BY LIST ((a+1));
CREATE TABLE fail () INHERITS (partitioned2);

-- Partition key in describe output
\d partitioned
\d partitioned2

DROP TABLE partitioned, partitioned2;

--
-- Partitions
--

-- check partition bound syntax

CREATE TABLE list_parted (
	a int
) PARTITION BY LIST (a);
-- syntax allows only string literal, numeric literal and null to be
-- specified for a partition bound value
CREATE TABLE part_1 PARTITION OF list_parted FOR VALUES IN ('1');
CREATE TABLE part_2 PARTITION OF list_parted FOR VALUES IN (2);
CREATE TABLE part_null PARTITION OF list_parted FOR VALUES IN (null);
CREATE TABLE fail_part PARTITION OF list_parted FOR VALUES IN (int '1');
CREATE TABLE fail_part PARTITION OF list_parted FOR VALUES IN ('1'::int);

-- syntax does not allow empty list of values for list partitions
CREATE TABLE fail_part PARTITION OF list_parted FOR VALUES IN ();
-- trying to specify range for list partitioned table
CREATE TABLE fail_part PARTITION OF list_parted FOR VALUES FROM (1) TO (2);

-- specified literal can't be cast to the partition column data type
CREATE TABLE bools (
	a bool
) PARTITION BY LIST (a);
CREATE TABLE bools_true PARTITION OF bools FOR VALUES IN (1);
DROP TABLE bools;

CREATE TABLE range_parted (
	a date
) PARTITION BY RANGE (a);

-- trying to specify list for range partitioned table
CREATE TABLE fail_part PARTITION OF range_parted FOR VALUES IN ('a');
-- each of start and end bounds must have same number of values as the
-- length of the partition key
CREATE TABLE fail_part PARTITION OF range_parted FOR VALUES FROM ('a', 1) TO ('z');
CREATE TABLE fail_part PARTITION OF range_parted FOR VALUES FROM ('a') TO ('z', 1);

-- cannot specify null values in range bounds
CREATE TABLE fail_part PARTITION OF range_parted FOR VALUES FROM (null) TO (unbounded);

-- check if compatible with the specified parent

-- cannot create as partition of a non-partitioned table
CREATE TABLE unparted (
	a int
);
CREATE TABLE fail_part PARTITION OF unparted FOR VALUES IN ('a');
DROP TABLE unparted;

-- cannot create a permanent rel as partition of a temp rel
CREATE TEMP TABLE temp_parted (
	a int
) PARTITION BY LIST (a);
CREATE TABLE fail_part PARTITION OF temp_parted FOR VALUES IN ('a');
DROP TABLE temp_parted;

-- cannot create a table with oids as partition of table without oids
CREATE TABLE no_oids_parted (
	a int
) PARTITION BY RANGE (a) WITHOUT OIDS;
CREATE TABLE fail_part PARTITION OF no_oids_parted FOR VALUES FROM (1) TO (10 )WITH OIDS;
DROP TABLE no_oids_parted;

-- likewise, the reverse if also true
CREATE TABLE oids_parted (
	a int
) PARTITION BY RANGE (a) WITH OIDS;
CREATE TABLE fail_part PARTITION OF oids_parted FOR VALUES FROM (1) TO (10 ) WITHOUT OIDS;
DROP TABLE oids_parted;

-- check for partition bound overlap and other invalid specifications

CREATE TABLE list_parted2 (
	a varchar
) PARTITION BY LIST (a);
CREATE TABLE part_null_z PARTITION OF list_parted2 FOR VALUES IN (null, 'z');
CREATE TABLE part_ab PARTITION OF list_parted2 FOR VALUES IN ('a', 'b');

CREATE TABLE fail_part PARTITION OF list_parted2 FOR VALUES IN (null);
CREATE TABLE fail_part PARTITION OF list_parted2 FOR VALUES IN ('b', 'c');

CREATE TABLE range_parted2 (
	a int
) PARTITION BY RANGE (a);

-- trying to create range partition with empty range
CREATE TABLE fail_part PARTITION OF range_parted2 FOR VALUES FROM (1) TO (0);
-- note that the range '[1, 1)' has no elements
CREATE TABLE fail_part PARTITION OF range_parted2 FOR VALUES FROM (1) TO (1);

CREATE TABLE part0 PARTITION OF range_parted2 FOR VALUES FROM (unbounded) TO (1);
CREATE TABLE fail_part PARTITION OF range_parted2 FOR VALUES FROM (unbounded) TO (2);
CREATE TABLE part1 PARTITION OF range_parted2 FOR VALUES FROM (1) TO (10);
CREATE TABLE fail_part PARTITION OF range_parted2 FOR VALUES FROM (9) TO (unbounded);
CREATE TABLE part2 PARTITION OF range_parted2 FOR VALUES FROM (20) TO (30);
CREATE TABLE part3 PARTITION OF range_parted2 FOR VALUES FROM (30) TO (40);
CREATE TABLE fail_part PARTITION OF range_parted2 FOR VALUES FROM (10) TO (30);
CREATE TABLE fail_part PARTITION OF range_parted2 FOR VALUES FROM (10) TO (50);

-- now check for multi-column range partition key
CREATE TABLE range_parted3 (
	a int,
	b int
) PARTITION BY RANGE (a, (b+1));

CREATE TABLE part00 PARTITION OF range_parted3 FOR VALUES FROM (0, unbounded) TO (0, unbounded);
CREATE TABLE fail_part PARTITION OF range_parted3 FOR VALUES FROM (0, unbounded) TO (0, 1);

CREATE TABLE part10 PARTITION OF range_parted3 FOR VALUES FROM (1, unbounded) TO (1, 1);
CREATE TABLE part11 PARTITION OF range_parted3 FOR VALUES FROM (1, 1) TO (1, 10);
CREATE TABLE part12 PARTITION OF range_parted3 FOR VALUES FROM (1, 10) TO (1, unbounded);
CREATE TABLE fail_part PARTITION OF range_parted3 FOR VALUES FROM (1, 10) TO (1, 20);

-- cannot create a partition that says column b is allowed to range
-- from -infinity to +infinity, while there exist partitions that have
-- more specific ranges
CREATE TABLE fail_part PARTITION OF range_parted3 FOR VALUES FROM (1, unbounded) TO (1, unbounded);

-- check schema propagation from parent

CREATE TABLE parted (
	a text,
	b int NOT NULL DEFAULT 0,
	CONSTRAINT check_a CHECK (length(a) > 0)
) PARTITION BY LIST (a);

CREATE TABLE part_a PARTITION OF parted FOR VALUES IN ('a');

-- only inherited attributes (never local ones)
SELECT attname, attislocal, attinhcount FROM pg_attribute
  WHERE attrelid = 'part_a'::regclass and attnum > 0
  ORDER BY attnum;

-- able to specify column default, column constraint, and table constraint
CREATE TABLE part_b PARTITION OF parted (
	b NOT NULL DEFAULT 1 CHECK (b >= 0),
	CONSTRAINT check_a CHECK (length(a) > 0)
) FOR VALUES IN ('b');
-- conislocal should be false for any merged constraints
SELECT conislocal, coninhcount FROM pg_constraint WHERE conrelid = 'part_b'::regclass AND conname = 'check_a';

-- specify PARTITION BY for a partition
CREATE TABLE fail_part_col_not_found PARTITION OF parted FOR VALUES IN ('c') PARTITION BY RANGE (c);
CREATE TABLE part_c PARTITION OF parted FOR VALUES IN ('c') PARTITION BY RANGE ((b));

-- create a level-2 partition
CREATE TABLE part_c_1_10 PARTITION OF part_c FOR VALUES FROM (1) TO (10);

-- Partition bound in describe output
\d part_b

-- Both partition bound and partition key in describe output
\d part_c

-- Show partition count in the parent's describe output
-- Tempted to include \d+ output listing partitions with bound info but
-- output could vary depending on the order in which partition oids are
-- returned.
\d parted

-- cleanup: avoid using CASCADE
DROP TABLE parted, part_a, part_b, part_c, part_c_1_10;
DROP TABLE list_parted, part_1, part_2, part_null;
DROP TABLE range_parted;
DROP TABLE list_parted2, part_ab, part_null_z;
DROP TABLE range_parted2, part0, part1, part2, part3;
DROP TABLE range_parted3, part00, part10, part11, part12;
