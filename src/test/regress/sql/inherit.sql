--
-- Test inheritance features
--
CREATE TABLE a (aa TEXT);
CREATE TABLE b (bb TEXT) INHERITS (a);
CREATE TABLE c (cc TEXT) INHERITS (a);
CREATE TABLE d (dd TEXT) INHERITS (b,c,a);

INSERT INTO a(aa) VALUES('aaa');
INSERT INTO a(aa) VALUES('aaaa');
INSERT INTO a(aa) VALUES('aaaaa');
INSERT INTO a(aa) VALUES('aaaaaa');
INSERT INTO a(aa) VALUES('aaaaaaa');
INSERT INTO a(aa) VALUES('aaaaaaaa');

INSERT INTO b(aa) VALUES('bbb');
INSERT INTO b(aa) VALUES('bbbb');
INSERT INTO b(aa) VALUES('bbbbb');
INSERT INTO b(aa) VALUES('bbbbbb');
INSERT INTO b(aa) VALUES('bbbbbbb');
INSERT INTO b(aa) VALUES('bbbbbbbb');

INSERT INTO c(aa) VALUES('ccc');
INSERT INTO c(aa) VALUES('cccc');
INSERT INTO c(aa) VALUES('ccccc');
INSERT INTO c(aa) VALUES('cccccc');
INSERT INTO c(aa) VALUES('ccccccc');
INSERT INTO c(aa) VALUES('cccccccc');

INSERT INTO d(aa) VALUES('ddd');
INSERT INTO d(aa) VALUES('dddd');
INSERT INTO d(aa) VALUES('ddddd');
INSERT INTO d(aa) VALUES('dddddd');
INSERT INTO d(aa) VALUES('ddddddd');
INSERT INTO d(aa) VALUES('dddddddd');

SELECT relname, a.* FROM a, pg_class where a.tableoid = pg_class.oid;
SELECT relname, b.* FROM b, pg_class where b.tableoid = pg_class.oid;
SELECT relname, c.* FROM c, pg_class where c.tableoid = pg_class.oid;
SELECT relname, d.* FROM d, pg_class where d.tableoid = pg_class.oid;
SELECT relname, a.* FROM ONLY a, pg_class where a.tableoid = pg_class.oid;
SELECT relname, b.* FROM ONLY b, pg_class where b.tableoid = pg_class.oid;
SELECT relname, c.* FROM ONLY c, pg_class where c.tableoid = pg_class.oid;
SELECT relname, d.* FROM ONLY d, pg_class where d.tableoid = pg_class.oid;

UPDATE a SET aa='zzzz' WHERE aa='aaaa';
UPDATE ONLY a SET aa='zzzzz' WHERE aa='aaaaa';
UPDATE b SET aa='zzz' WHERE aa='aaa';
UPDATE ONLY b SET aa='zzz' WHERE aa='aaa';
UPDATE a SET aa='zzzzzz' WHERE aa LIKE 'aaa%';

SELECT relname, a.* FROM a, pg_class where a.tableoid = pg_class.oid;
SELECT relname, b.* FROM b, pg_class where b.tableoid = pg_class.oid;
SELECT relname, c.* FROM c, pg_class where c.tableoid = pg_class.oid;
SELECT relname, d.* FROM d, pg_class where d.tableoid = pg_class.oid;
SELECT relname, a.* FROM ONLY a, pg_class where a.tableoid = pg_class.oid;
SELECT relname, b.* FROM ONLY b, pg_class where b.tableoid = pg_class.oid;
SELECT relname, c.* FROM ONLY c, pg_class where c.tableoid = pg_class.oid;
SELECT relname, d.* FROM ONLY d, pg_class where d.tableoid = pg_class.oid;

UPDATE b SET aa='new';

SELECT relname, a.* FROM a, pg_class where a.tableoid = pg_class.oid;
SELECT relname, b.* FROM b, pg_class where b.tableoid = pg_class.oid;
SELECT relname, c.* FROM c, pg_class where c.tableoid = pg_class.oid;
SELECT relname, d.* FROM d, pg_class where d.tableoid = pg_class.oid;
SELECT relname, a.* FROM ONLY a, pg_class where a.tableoid = pg_class.oid;
SELECT relname, b.* FROM ONLY b, pg_class where b.tableoid = pg_class.oid;
SELECT relname, c.* FROM ONLY c, pg_class where c.tableoid = pg_class.oid;
SELECT relname, d.* FROM ONLY d, pg_class where d.tableoid = pg_class.oid;

UPDATE a SET aa='new';

DELETE FROM ONLY c WHERE aa='new';

SELECT relname, a.* FROM a, pg_class where a.tableoid = pg_class.oid;
SELECT relname, b.* FROM b, pg_class where b.tableoid = pg_class.oid;
SELECT relname, c.* FROM c, pg_class where c.tableoid = pg_class.oid;
SELECT relname, d.* FROM d, pg_class where d.tableoid = pg_class.oid;
SELECT relname, a.* FROM ONLY a, pg_class where a.tableoid = pg_class.oid;
SELECT relname, b.* FROM ONLY b, pg_class where b.tableoid = pg_class.oid;
SELECT relname, c.* FROM ONLY c, pg_class where c.tableoid = pg_class.oid;
SELECT relname, d.* FROM ONLY d, pg_class where d.tableoid = pg_class.oid;

DELETE FROM a;

SELECT relname, a.* FROM a, pg_class where a.tableoid = pg_class.oid;
SELECT relname, b.* FROM b, pg_class where b.tableoid = pg_class.oid;
SELECT relname, c.* FROM c, pg_class where c.tableoid = pg_class.oid;
SELECT relname, d.* FROM d, pg_class where d.tableoid = pg_class.oid;
SELECT relname, a.* FROM ONLY a, pg_class where a.tableoid = pg_class.oid;
SELECT relname, b.* FROM ONLY b, pg_class where b.tableoid = pg_class.oid;
SELECT relname, c.* FROM ONLY c, pg_class where c.tableoid = pg_class.oid;
SELECT relname, d.* FROM ONLY d, pg_class where d.tableoid = pg_class.oid;

-- Confirm PRIMARY KEY adds NOT NULL constraint to child table
CREATE TEMP TABLE z (b TEXT, PRIMARY KEY(aa, b)) inherits (a);
INSERT INTO z VALUES (NULL, 'text'); -- should fail

-- Check UPDATE with inherited target and an inherited source table
create temp table foo(f1 int, f2 int);
create temp table foo2(f3 int) inherits (foo);
create temp table bar(f1 int, f2 int);
create temp table bar2(f3 int) inherits (bar);

insert into foo values(1,1);
insert into foo values(3,3);
insert into foo2 values(2,2,2);
insert into foo2 values(3,3,3);
insert into bar values(1,1);
insert into bar values(2,2);
insert into bar values(3,3);
insert into bar values(4,4);
insert into bar2 values(1,1,1);
insert into bar2 values(2,2,2);
insert into bar2 values(3,3,3);
insert into bar2 values(4,4,4);

update bar set f2 = f2 + 100 where f1 in (select f1 from foo);

SELECT relname, bar.* FROM bar, pg_class where bar.tableoid = pg_class.oid;


/* Test inheritance of structure (LIKE) */
CREATE TABLE inhx (xx text DEFAULT 'text');

/*
 * Test double inheritance
 *
 * Ensure that defaults are NOT included unless
 * INCLUDING DEFAULTS is specified 
 */
CREATE TABLE inhe (ee text, LIKE inhx) inherits (b);
INSERT INTO inhe VALUES ('ee-col1', 'ee-col2', DEFAULT, 'ee-col4');
SELECT * FROM inhe; /* Columns aa, bb, xx value NULL, ee */
SELECT * FROM inhx; /* Empty set since LIKE inherits structure only */
SELECT * FROM b; /* Has ee entry */
SELECT * FROM a; /* Has ee entry */

CREATE TABLE inhf (LIKE inhx, LIKE inhx); /* Throw error */

CREATE TABLE inhf (LIKE inhx INCLUDING DEFAULTS);
INSERT INTO inhf DEFAULT VALUES;
SELECT * FROM inhf; /* Single entry with value 'text' */
