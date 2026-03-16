CREATE SCHEMA create_property_graph_tests;
GRANT USAGE ON SCHEMA create_property_graph_tests TO PUBLIC;
SET search_path = create_property_graph_tests;
CREATE SCHEMA create_property_graph_tests_2;
GRANT USAGE ON SCHEMA create_property_graph_tests_2 TO PUBLIC;

CREATE ROLE regress_graph_user1;
CREATE ROLE regress_graph_user2;

CREATE PROPERTY GRAPH g1;

COMMENT ON PROPERTY GRAPH g1 IS 'a graph';

CREATE PROPERTY GRAPH g1;  -- error: duplicate

CREATE TABLE t1 (a int, b text);
CREATE TABLE t2 (i int PRIMARY KEY, j int, k int);
CREATE TABLE t3 (x int, y text, z text);

CREATE TABLE e1 (a int, i int, t text, PRIMARY KEY (a, i));
CREATE TABLE e2 (a int, x int, t text);

CREATE PROPERTY GRAPH g2
    VERTEX TABLES (t1 KEY (a), t2 DEFAULT LABEL, t3 KEY (x) LABEL t3l1 LABEL t3l2)
    EDGE TABLES (
        e1
            SOURCE KEY (a) REFERENCES t1 (a)
            DESTINATION KEY (i) REFERENCES t2 (i),
        e2 KEY (a, x)
            SOURCE KEY (a) REFERENCES t1 (a)
            DESTINATION KEY (x, t) REFERENCES t3 (x, y)
    );

-- test dependencies/object descriptions

DROP TABLE t1;  -- fail
ALTER TABLE t1 DROP COLUMN b;  -- non-key column; fail
ALTER TABLE t1 DROP COLUMN a;  -- key column; fail

-- like g2 but assembled with ALTER
CREATE PROPERTY GRAPH g3;
ALTER PROPERTY GRAPH g3 ADD VERTEX TABLES (t1 KEY (a), t2 DEFAULT LABEL);
ALTER PROPERTY GRAPH g3
    ADD VERTEX TABLES (t3 KEY (x) LABEL t3l1)
    ADD EDGE TABLES (
        e1 SOURCE KEY (a) REFERENCES t1 (a) DESTINATION KEY (i) REFERENCES t2 (i),
        e2 KEY (a, x) SOURCE KEY (a) REFERENCES t1 (a) DESTINATION KEY (x, t) REFERENCES t3 (x, y)
    );
ALTER PROPERTY GRAPH g3
    ALTER VERTEX TABLE t3
        ADD LABEL t3l2 PROPERTIES ALL COLUMNS
        ADD LABEL t3l3 PROPERTIES ALL COLUMNS;
ALTER PROPERTY GRAPH g3 ALTER VERTEX TABLE t3 DROP LABEL t3l3x;  -- error
ALTER PROPERTY GRAPH g3 ALTER VERTEX TABLE t3 DROP LABEL t3l3;
ALTER PROPERTY GRAPH g3 DROP VERTEX TABLES (t2);  -- fail
ALTER PROPERTY GRAPH g3 DROP VERTEX TABLES (t2) CASCADE;
ALTER PROPERTY GRAPH g3 DROP EDGE TABLES (e2);

CREATE PROPERTY GRAPH g4
    VERTEX TABLES (
        t1 KEY (a) NO PROPERTIES,
        t2 DEFAULT LABEL PROPERTIES (i + j AS i_j, k),
        t3 KEY (x) LABEL t3l1 PROPERTIES (x, y AS yy) LABEL t3l2 PROPERTIES (x, z AS zz)
    )
    EDGE TABLES (
        e1
            SOURCE KEY (a) REFERENCES t1 (a)
            DESTINATION KEY (i) REFERENCES t2 (i)
            PROPERTIES ALL COLUMNS,
        e2 KEY (a, x)
            SOURCE KEY (a) REFERENCES t1 (a)
            DESTINATION KEY (x, t) REFERENCES t3 (x, y)
            PROPERTIES ALL COLUMNS
    );

ALTER PROPERTY GRAPH g4 ALTER VERTEX TABLE t2 ALTER LABEL t2 ADD PROPERTIES (k * 2 AS kk);
ALTER PROPERTY GRAPH g4 ALTER VERTEX TABLE t2 ALTER LABEL t2 DROP PROPERTIES (k);

CREATE TABLE t11 (a int PRIMARY KEY);
CREATE TABLE t12 (b int PRIMARY KEY);
CREATE TABLE t13 (
    c int PRIMARY KEY,
    d int REFERENCES t11,
    e int REFERENCES t12
);

CREATE PROPERTY GRAPH g5
    VERTEX TABLES (t11, t12)
    EDGE TABLES (t13 SOURCE t11 DESTINATION t12);

SELECT pg_get_propgraphdef('g5'::regclass);

-- error cases
CREATE UNLOGGED PROPERTY GRAPH gx VERTEX TABLES (xx, yy);
CREATE PROPERTY GRAPH gx VERTEX TABLES (xx, yy);
CREATE PROPERTY GRAPH gx VERTEX TABLES (t1 KEY (a), t2 KEY (i), t1 KEY (a));
ALTER PROPERTY GRAPH g3 ADD VERTEX TABLES (t3 KEY (x));  -- duplicate alias
CREATE PROPERTY GRAPH gx
    VERTEX TABLES (t1 AS tt KEY (a), t2 KEY (i))
    EDGE TABLES (
        e1 SOURCE t1 DESTINATION t2
    );
CREATE PROPERTY GRAPH gx
    VERTEX TABLES (t1 KEY (a), t2 KEY (i))
    EDGE TABLES (
        e1 SOURCE t1 DESTINATION tx
    );
COMMENT ON PROPERTY GRAPH gx IS 'not a graph';
CREATE PROPERTY GRAPH gx
    VERTEX TABLES (t1 KEY (a), t2)
    EDGE TABLES (
        e1 SOURCE t1 DESTINATION t2  -- no foreign keys
    );
CREATE PROPERTY GRAPH gx
    VERTEX TABLES (
        t1 KEY (a)
            LABEL foo PROPERTIES (a + 1 AS aa)
            LABEL bar PROPERTIES (1 + a AS aa)  -- expression mismatch
    );
ALTER PROPERTY GRAPH g2
    ADD VERTEX TABLES (
        t1 AS t1x KEY (a)
            LABEL foo PROPERTIES (a + 1 AS aa)
            LABEL bar PROPERTIES (1 + a AS aa)  -- expression mismatch
    );
CREATE PROPERTY GRAPH gx
    VERTEX TABLES (
        t1 KEY (a) PROPERTIES (b AS p1),
        t2 PROPERTIES (k AS p1)  -- type mismatch
    );
ALTER PROPERTY GRAPH g2 ALTER VERTEX TABLE t1 ADD LABEL foo PROPERTIES (b AS k);  -- type mismatch

CREATE TABLE t1x (a int, b varchar(10));
CREATE TABLE t2x (i int, j varchar(15));
CREATE PROPERTY GRAPH gx
    VERTEX TABLES (
        t1x KEY (a) PROPERTIES (b AS p1),
        t2x KEY (i) PROPERTIES (j AS p1)  -- typmod mismatch
    );
CREATE PROPERTY GRAPH gx
    VERTEX TABLES (
        t1x KEY (a) PROPERTIES (b::varchar(20) AS p1),
        t2x KEY (i) PROPERTIES (j::varchar(25) AS p1)  -- typmod mismatch
    );
CREATE PROPERTY GRAPH gx
    VERTEX TABLES (
        t1x KEY (a) PROPERTIES (b::varchar(20) AS p1),
        t2x KEY (i) PROPERTIES (j::varchar(20) AS p1)  -- matching typmods by casting works
    );
DROP PROPERTY GRAPH gx;
DROP TABLE t1x, t2x;

CREATE PROPERTY GRAPH gx
    VERTEX TABLES (
        t1 KEY (a) LABEL l1 PROPERTIES (a, a AS aa),
        t2 KEY (i) LABEL l1 PROPERTIES (i AS a, j AS b, k)  -- mismatching number of properties on label
    );
CREATE PROPERTY GRAPH gx
    VERTEX TABLES (
        t1 KEY (a) LABEL l1 PROPERTIES (a, b),
        t2 KEY (i) LABEL l1 PROPERTIES (i AS a)  -- mismatching number of properties on label
    );
CREATE PROPERTY GRAPH gx
    VERTEX TABLES (
        t1 KEY (a) LABEL l1 PROPERTIES (a, b),
        t2 KEY (i) LABEL l1 PROPERTIES (i AS a, j AS j)  -- mismatching property names on label
    );
ALTER PROPERTY GRAPH g4 ALTER VERTEX TABLE t1 ADD LABEL t3l1 PROPERTIES (a AS x, b AS yy, b AS zz);  -- mismatching number of properties on label
ALTER PROPERTY GRAPH g4 ALTER VERTEX TABLE t1 ADD LABEL t3l1 PROPERTIES (a AS x, b AS zz);  -- mismatching property names on label
ALTER PROPERTY GRAPH g4 ALTER VERTEX TABLE t1 ADD LABEL t3l1 PROPERTIES (a AS x);  -- mismatching number of properties on label

ALTER PROPERTY GRAPH g1 OWNER TO regress_graph_user1;
SET ROLE regress_graph_user1;
GRANT SELECT ON PROPERTY GRAPH g1 TO regress_graph_user2;
GRANT UPDATE ON PROPERTY GRAPH g1 TO regress_graph_user2;  -- fail
RESET ROLE;

-- collation

CREATE TABLE tc1 (a int, b text);
CREATE TABLE tc2 (a int, b text);
CREATE TABLE tc3 (a int, b text COLLATE "C");

CREATE TABLE ec1 (ek1 int, ek2 int, eb text);
CREATE TABLE ec2 (ek1 int, ek2 int, eb text COLLATE "POSIX");

CREATE PROPERTY GRAPH gc1
    VERTEX TABLES (tc1 KEY (a), tc2 KEY (a), tc3 KEY (a)); -- fail
CREATE PROPERTY GRAPH gc1
    VERTEX TABLES (tc1 KEY (a), tc2 KEY (a))
    EDGE TABLES (
        ec1 KEY (ek1, ek2)
            SOURCE KEY (ek1) REFERENCES tc1 (a)
            DESTINATION KEY (ek2) REFERENCES tc2 (a),
        ec2 KEY (ek1, ek2)
            SOURCE KEY (ek1) REFERENCES tc1 (a)
            DESTINATION KEY (ek2) REFERENCES tc2 (a)
    ); -- fail
CREATE PROPERTY GRAPH gc1
    VERTEX TABLES (tc1 KEY (a) DEFAULT LABEL PROPERTIES (a), tc3 KEY (b))
    EDGE TABLES (
        ec2 KEY (ek1, eb)
            SOURCE KEY (ek1) REFERENCES tc1 (a)
            DESTINATION KEY (eb) REFERENCES tc3 (b)
    ); -- fail
CREATE PROPERTY GRAPH gc1
    VERTEX TABLES (tc1 KEY (a), tc2 KEY (a))
    EDGE TABLES (
        ec1 KEY (ek1, ek2)
            SOURCE KEY (ek1) REFERENCES tc1 (a)
            DESTINATION KEY (ek2) REFERENCES tc2 (a)
    );
ALTER PROPERTY GRAPH gc1 ADD VERTEX TABLES (tc3 KEY (a)); -- fail
ALTER PROPERTY GRAPH gc1
    ADD EDGE TABLES (
        ec2 KEY (ek1, ek2)
            SOURCE KEY (ek1) REFERENCES tc1 (a)
            DESTINATION KEY (ek2) REFERENCES tc2 (a)
    ); -- fail
ALTER PROPERTY GRAPH gc1
    ADD VERTEX TABLES (
        tc3 KEY (a) DEFAULT LABEL PROPERTIES (a, b COLLATE pg_catalog.DEFAULT AS b)
    );
ALTER PROPERTY GRAPH gc1
    ADD EDGE TABLES (
        ec2 KEY (ek1, ek2)
            SOURCE KEY (ek1) REFERENCES tc1 (a)
            DESTINATION KEY (ek2) REFERENCES tc2 (a)
            DEFAULT LABEL PROPERTIES (ek1, ek2, eb COLLATE pg_catalog.DEFAULT AS eb)
    );
DROP PROPERTY GRAPH gc1;
CREATE PROPERTY GRAPH gc1
    VERTEX TABLES (
        tc1 KEY (a) DEFAULT LABEL PROPERTIES (a, b::varchar COLLATE "C" AS b),
        tc2 KEY (a) DEFAULT LABEL PROPERTIES (a, (b COLLATE "C")::varchar AS b),
        tc3 KEY (a) DEFAULT LABEL PROPERTIES (a, b::varchar AS b)
    )
    EDGE TABLES (
        ec1 KEY (ek1, ek2)
            SOURCE KEY (ek1) REFERENCES tc1 (a)
            DESTINATION KEY (ek2) REFERENCES tc2 (a)
            DEFAULT LABEL PROPERTIES (ek1, ek2, eb),
        ec2 KEY (ek1, ek2)
            SOURCE KEY (ek1) REFERENCES tc1 (a)
            DESTINATION KEY (ek2) REFERENCES tc2 (a)
            DEFAULT LABEL PROPERTIES (ek1, ek2, eb COLLATE pg_catalog.DEFAULT AS eb)
    );

-- type inconsistency check

CREATE TABLE v1 (a int primary key, b text);
CREATE TABLE e(k1 text, k2 text, c text);
CREATE TABLE v2 (m text, n text);
CREATE PROPERTY GRAPH gt
    VERTEX TABLES (v1 KEY (a), v2 KEY (m))
    EDGE TABLES (
        e KEY (k1, k2)
            SOURCE KEY (k1) REFERENCES v1(a)
            DESTINATION KEY (k2) REFERENCES v2(m)
    ); -- fail
ALTER TABLE e DROP COLUMN k1, ADD COLUMN k1 bigint primary key;
CREATE PROPERTY GRAPH gt
    VERTEX TABLES (v1 KEY (a), v2 KEY (m))
    EDGE TABLES (
        e KEY (k1, k2)
            SOURCE KEY (k1) REFERENCES v1(a)
            DESTINATION KEY (k2) REFERENCES v2(m)
    );

-- information schema

SELECT * FROM information_schema.property_graphs ORDER BY property_graph_name;
SELECT * FROM information_schema.pg_element_tables ORDER BY property_graph_name, element_table_alias;
SELECT * FROM information_schema.pg_element_table_key_columns ORDER BY property_graph_name, element_table_alias, ordinal_position;
SELECT * FROM information_schema.pg_edge_table_components ORDER BY property_graph_name, edge_table_alias, edge_end DESC, ordinal_position;
SELECT * FROM information_schema.pg_element_table_labels ORDER BY property_graph_name, element_table_alias, label_name;
SELECT * FROM information_schema.pg_element_table_properties ORDER BY property_graph_name, element_table_alias, property_name;
SELECT * FROM information_schema.pg_label_properties ORDER BY property_graph_name, label_name, property_name;
SELECT * FROM information_schema.pg_labels ORDER BY property_graph_name, label_name;
SELECT * FROM information_schema.pg_property_data_types ORDER BY property_graph_name, property_name;
SELECT * FROM information_schema.pg_property_graph_privileges WHERE grantee LIKE 'regress%' ORDER BY property_graph_name, grantor, grantee, privilege_type;

-- test object address functions
SELECT pg_describe_object(classid, objid, objsubid) as obj,
       pg_describe_object(refclassid, refobjid, refobjsubid) as reference_graph
    FROM pg_depend
    WHERE refclassid = 'pg_class'::regclass AND
          refobjid = 'create_property_graph_tests.g2'::regclass
    ORDER BY 1, 2;
SELECT (pg_identify_object_as_address(classid, objid, objsubid)).*
    FROM pg_depend
    WHERE refclassid = 'pg_class'::regclass AND
          refobjid = 'create_property_graph_tests.g2'::regclass
    ORDER BY 1, 2, 3;
SELECT (pg_identify_object(classid, objid, objsubid)).*
    FROM pg_depend
    WHERE refclassid = 'pg_class'::regclass AND
          refobjid = 'create_property_graph_tests.g2'::regclass
    ORDER BY 1, 2, 3, 4;

\a\t
SELECT pg_get_propgraphdef('g2'::regclass);
SELECT pg_get_propgraphdef('g3'::regclass);
SELECT pg_get_propgraphdef('g4'::regclass);

SELECT pg_get_propgraphdef('pg_type'::regclass);  -- error
\a\t

-- Test \d variants for property graphs
\dG g1
\dG+ g1
\dGx g1
\d g2
\d g1
\d+ g2
\d+ g1
\dG g_nonexistent
\dG t11
\set QUIET 'off'
\dG g_nonexistent
\set QUIET 'on'

-- temporary property graph

-- Keep this at the end to avoid test failure due to changing temporary
-- namespace names in information schema query outputs
CREATE TEMPORARY PROPERTY GRAPH g1; -- same name as persistent graph
DROP PROPERTY GRAPH g1;  -- drops temporary graph retaining persistent graph
\dG g1
CREATE TEMPORARY TABLE v2tmp (m text, n text);
CREATE TEMPORARY PROPERTY GRAPH gtmp
    VERTEX TABLES (v1 KEY (a), v2tmp KEY (m))
    EDGE TABLES (
        e KEY (k1, k2)
            SOURCE KEY (k1) REFERENCES v1(a)
            DESTINATION KEY (k2) REFERENCES v2tmp(m)
    );
DROP PROPERTY GRAPH gtmp;
CREATE PROPERTY GRAPH gtmp
    VERTEX TABLES (v1 KEY (a), v2tmp KEY (m))
    EDGE TABLES (
        e KEY (k1, k2)
            SOURCE KEY (k1) REFERENCES v1(a)
            DESTINATION KEY (k2) REFERENCES v2tmp(m)
    );
ALTER PROPERTY GRAPH g1
    ADD VERTEX TABLES (v2tmp KEY (m));  -- error


-- DROP, ALTER SET SCHEMA, ALTER PROPERTY GRAPH RENAME TO

DROP TABLE g2;  -- error: wrong object type
CREATE VIEW vg1 AS SELECT * FROM GRAPH_TABLE(g1 MATCH () COLUMNS (1 AS one));
DROP PROPERTY GRAPH g1; -- error
ALTER PROPERTY GRAPH g1 SET SCHEMA create_property_graph_tests_2;
ALTER PROPERTY GRAPH create_property_graph_tests_2.g1 RENAME TO g2;
DROP PROPERTY GRAPH create_property_graph_tests_2.g2 CASCADE;
DROP PROPERTY GRAPH g1;  -- error
ALTER PROPERTY GRAPH g1 ADD VERTEX TABLES (t1 KEY (a));  -- error
ALTER PROPERTY GRAPH IF EXISTS g1 SET SCHEMA create_property_graph_tests_2;
DROP PROPERTY GRAPH IF EXISTS g1;

DROP ROLE regress_graph_user1, regress_graph_user2;

-- leave remaining objects behind for pg_upgrade/pg_dump tests
