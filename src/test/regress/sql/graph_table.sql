CREATE SCHEMA graph_table_tests;
GRANT USAGE ON SCHEMA graph_table_tests TO PUBLIC;
SET search_path = graph_table_tests;

CREATE TABLE products (
    product_no integer PRIMARY KEY,
    name varchar,
    price numeric
);

CREATE TABLE customers (
    customer_id integer PRIMARY KEY,
    name varchar,
    address varchar
);

CREATE TABLE orders (
    order_id integer PRIMARY KEY,
    ordered_when date
);

CREATE TABLE order_items (
    order_items_id integer PRIMARY KEY,
    order_id integer REFERENCES orders (order_id),
    product_no integer REFERENCES products (product_no),
    quantity integer
);

CREATE TABLE customer_orders (
    customer_orders_id integer PRIMARY KEY,
    customer_id integer REFERENCES customers (customer_id),
    order_id integer REFERENCES orders (order_id)
);

CREATE TABLE wishlists (
    wishlist_id integer PRIMARY KEY,
    wishlist_name varchar
);

CREATE TABLE wishlist_items (
    wishlist_items_id integer PRIMARY KEY,
    wishlist_id integer REFERENCES wishlists (wishlist_id),
    product_no integer REFERENCES products (product_no)
);

CREATE TABLE customer_wishlists (
    customer_wishlist_id integer PRIMARY KEY,
    customer_id integer REFERENCES customers (customer_id),
    wishlist_id integer REFERENCES wishlists (wishlist_id)
);

CREATE PROPERTY GRAPH myshop
    VERTEX TABLES (
        products,
        customers,
        orders
           DEFAULT LABEL
           LABEL lists PROPERTIES (order_id AS node_id, 'order'::varchar(10) AS list_type),
        wishlists
           DEFAULT LABEL
           LABEL lists PROPERTIES (wishlist_id AS node_id, 'wishlist'::varchar(10) AS list_type)
    )
    EDGE TABLES (
        order_items KEY (order_items_id)
            SOURCE KEY (order_id) REFERENCES orders (order_id)
            DESTINATION KEY (product_no) REFERENCES products (product_no)
            DEFAULT LABEL
            LABEL list_items PROPERTIES (order_id AS link_id, product_no),
        wishlist_items KEY (wishlist_items_id)
            SOURCE KEY (wishlist_id) REFERENCES wishlists (wishlist_id)
            DESTINATION KEY (product_no) REFERENCES products (product_no)
            DEFAULT LABEL
            LABEL list_items PROPERTIES (wishlist_id AS link_id, product_no),
        customer_orders KEY (customer_orders_id)
            SOURCE KEY (customer_id) REFERENCES customers (customer_id)
            DESTINATION KEY (order_id) REFERENCES orders (order_id)
            DEFAULT LABEL
            LABEL cust_lists PROPERTIES (customer_id, order_id AS link_id),
        customer_wishlists KEY (customer_wishlist_id)
            SOURCE KEY (customer_id) REFERENCES customers (customer_id)
            DESTINATION KEY (wishlist_id) REFERENCES wishlists (wishlist_id)
            DEFAULT LABEL
            LABEL cust_lists PROPERTIES (customer_id, wishlist_id AS link_id)
    );

SELECT customer_name FROM GRAPH_TABLE (xxx MATCH (c IS customers WHERE c.address = 'US')-[IS customer_orders]->(o IS orders) COLUMNS (c.name AS customer_name));  -- error
SELECT customer_name FROM GRAPH_TABLE (pg_class MATCH (c IS customers WHERE c.address = 'US')-[IS customer_orders]->(o IS orders) COLUMNS (c.name AS customer_name));  -- error
SELECT customer_name FROM GRAPH_TABLE (myshop MATCH (c IS customers WHERE c.address = 'US')-[IS customer_orders]->(o IS orders) COLUMNS (cx.name AS customer_name));  -- error
SELECT customer_name FROM GRAPH_TABLE (myshop MATCH (c IS customers WHERE c.address = 'US')-[IS customer_orders]->(o IS orders) COLUMNS (c.namex AS customer_name));  -- error
SELECT customer_name FROM GRAPH_TABLE (myshop MATCH (c IS customers|employees WHERE c.address = 'US')-[IS customer_orders]->(o IS orders) COLUMNS (c.name AS customer_name));  -- error
SELECT customer_name FROM GRAPH_TABLE (myshop MATCH (c IS customers WHERE c.address = 'US')-[IS customer_orders] COLUMNS (c.name AS customer_name));  -- error
SELECT * FROM GRAPH_TABLE (myshop MATCH (c IS customers), (o IS orders) COLUMNS (c.name AS customer_name));  -- error
SELECT * FROM GRAPH_TABLE (myshop MATCH COLUMNS (1 AS col));  -- error, empty match clause
SELECT customer_name FROM GRAPH_TABLE (myshop MATCH (c IS customers)->{1,2}(o IS orders) COLUMNS (c.name AS customer_name));  -- error
SELECT * FROM GRAPH_TABLE (myshop MATCH ((c IS customers)->(o IS orders)) COLUMNS (c.name));

-- a property graph can be referenced only from within GRAPH_TABLE clause.
SELECT * FROM myshop; -- error
COPY myshop TO stdout; -- error
INSERT INTO myshop VALUES (1); -- error

INSERT INTO products VALUES
    (1, 'product1', 10),
    (2, 'product2', 20),
    (3, 'product3', 30);
INSERT INTO customers VALUES
    (1, 'customer1', 'US'),
    (2, 'customer2', 'CA'),
    (3, 'customer3', 'GL');
INSERT INTO orders VALUES
    (1, date '2024-01-01'),
    (2, date '2024-01-02'),
    (3, date '2024-01-03');
INSERT INTO wishlists VALUES
    (1, 'wishlist1'),
    (2, 'wishlist2'),
    (3, 'wishlist3');
INSERT INTO order_items (order_items_id, order_id, product_no, quantity) VALUES
    (1, 1, 1, 5),
    (2, 1, 2, 10),
    (3, 2, 1, 7);
INSERT INTO customer_orders (customer_orders_id, customer_id, order_id) VALUES
    (1, 1, 1),
    (2, 2, 2);
INSERT INTO customer_wishlists (customer_wishlist_id, customer_id, wishlist_id) VALUES
    (1, 2, 3),
    (2, 3, 1),
    (3, 3, 2);
INSERT INTO wishlist_items (wishlist_items_id, wishlist_id, product_no) VALUES
    (1, 1, 2),
    (2, 1, 3),
    (3, 2, 1),
    (4, 3, 1);

-- single element path pattern
SELECT * FROM GRAPH_TABLE (myshop MATCH (c IS customers) COLUMNS (c.name));
SELECT * FROM GRAPH_TABLE (myshop MATCH (c IS customers WHERE c.address = 'US')-[IS customer_orders]->(o IS orders) COLUMNS (c.name));
-- graph element specification without label or variable
SELECT * FROM GRAPH_TABLE (myshop MATCH (c IS customers WHERE c.address = 'US')-[]->(o IS orders) COLUMNS (c.name AS customer_name));
SELECT * FROM GRAPH_TABLE (myshop MATCH (c IS customers)-[co IS customer_orders]->(o IS orders WHERE o.ordered_when = date '2024-01-02') COLUMNS (c.name, c.address));
SELECT * FROM GRAPH_TABLE (myshop MATCH (o IS orders)-[IS customer_orders]->(c IS customers) COLUMNS (c.name, o.ordered_when));
SELECT * FROM GRAPH_TABLE (myshop MATCH (o IS orders)<-[IS customer_orders]-(c IS customers) COLUMNS (c.name, o.ordered_when));
-- spaces around pattern operators
SELECT * FROM GRAPH_TABLE (myshop MATCH ( o IS orders ) <- [ IS customer_orders ] - (c IS customers) COLUMNS ( c.name, o.ordered_when));
SELECT * FROM GRAPH_TABLE (myshop MATCH (c IS customers)-[IS cust_lists]->(l IS lists)-[ IS list_items]->(p IS products) COLUMNS (c.name AS customer_name, p.name AS product_name, l.list_type)) ORDER BY customer_name, product_name, list_type;
-- label disjunction
SELECT * FROM GRAPH_TABLE (myshop MATCH (c IS customers)-[IS customer_orders | customer_wishlists ]->(l IS orders | wishlists)-[ IS list_items]->(p IS products) COLUMNS (c.name AS customer_name, p.name AS product_name)) ORDER BY customer_name, product_name;
-- property not associated with labels queried results in error
SELECT * FROM GRAPH_TABLE (myshop MATCH (c IS customers)-[IS customer_orders | customer_wishlists ]->(l IS orders | wishlists)-[ IS list_items]->(p IS products) COLUMNS (c.name AS customer_name, p.name AS product_name, l.list_type)) ORDER BY 1, 2, 3;
-- vertex to vertex connection abbreviation
SELECT * FROM GRAPH_TABLE (myshop MATCH (c IS customers)->(o IS orders) COLUMNS (c.name, o.ordered_when)) ORDER BY 1;

-- lateral test
CREATE TABLE x1 (a int, b text);
INSERT INTO x1 VALUES (1, 'one'), (2, 'two');
SELECT * FROM x1, GRAPH_TABLE (myshop MATCH (c IS customers WHERE c.address = 'US' AND c.customer_id = x1.a)-[IS customer_orders]->(o IS orders) COLUMNS (c.name AS customer_name, c.customer_id AS cid));
DROP TABLE x1;

CREATE TABLE v1 (
    id int PRIMARY KEY,
    vname varchar(10),
    vprop1 int,
    vprop2 int
);

CREATE TABLE v2 (
    id1 int,
    id2 int,
    vname varchar(10),
    vprop1 int,
    vprop2 int
);

CREATE TABLE v3 (
    id int PRIMARY KEY,
    vname varchar(10),
    vprop1 int,
    vprop2 int
);

-- edge connecting v1 and v2
CREATE TABLE e1_2 (
    id_1 int,
    id_2_1 int,
    id_2_2 int,
    ename varchar(10),
    eprop1 int
);

-- edge connecting v1 and v3
CREATE TABLE e1_3 (
    id_1 int,
    id_3 int,
    ename varchar(10),
    eprop1 int,
    PRIMARY KEY (id_1, id_3)
);

CREATE TABLE e2_3 (
    id_2_1 int,
    id_2_2 int,
    id_3 int,
    ename varchar(10),
    eprop1 int
);

CREATE PROPERTY GRAPH g1
    VERTEX TABLES (
        v1
            LABEL vl1 PROPERTIES (vname, vprop1)
            LABEL l1 PROPERTIES (vname AS elname), -- label shared by vertexes as well as edges
        v2 KEY (id1, id2)
            LABEL vl2 PROPERTIES (vname, vprop2, 'vl2_prop'::varchar(10) AS lprop1)
            LABEL vl3 PROPERTIES (vname, vprop1, 'vl2_prop'::varchar(10) AS lprop1)
            LABEL l1 PROPERTIES (vname AS elname),
        v3
            LABEL vl3 PROPERTIES (vname, vprop1, 'vl3_prop'::varchar(10) AS lprop1)
            LABEL l1 PROPERTIES (vname AS elname)
    )
    -- edges with differing number of columns in destination keys
    EDGE TABLES (
        e1_2 key (id_1, id_2_1, id_2_2)
            SOURCE KEY (id_1) REFERENCES v1 (id)
            DESTINATION KEY (id_2_1, id_2_2) REFERENCES v2 (id1, id2)
            LABEL el1 PROPERTIES (eprop1, ename)
            LABEL l1 PROPERTIES (ename AS elname),
        e1_3
            SOURCE KEY (id_1) REFERENCES v1 (id)
            DESTINATION KEY (id_3) REFERENCES v3 (id)
            -- order of property names doesn't matter
            LABEL el1 PROPERTIES (ename, eprop1)
            LABEL l1 PROPERTIES (ename AS elname),
        e2_3 key (id_2_1, id_2_2, id_3)
            SOURCE KEY (id_2_1, id_2_2) REFERENCES v2 (id1, id2)
            DESTINATION KEY (id_3) REFERENCES v3 (id)
            -- new property lprop2 not shared by el1
            -- does not share eprop1 from by el1
            LABEL el2 PROPERTIES (ename, eprop1 * 10 AS lprop2)
            LABEL l1 PROPERTIES (ename AS elname)
    );

INSERT INTO v1 VALUES
    (1, 'v11', 10, 100),
    (2, 'v12', 20, 200),
    (3, 'v13', 30, 300);
INSERT INTO v2 VALUES
    (1000, 1, 'v21', 1010, 1100),
    (1000, 2, 'v22', 1020, 1200),
    (1000, 3, 'v23', 1030, 1300);
INSERT INTO v3 VALUES
    (2001, 'v31', 2010, 2100),
    (2002, 'v32', 2020, 2200),
    (2003, 'v33', 2030, 2300);
INSERT INTO e1_2 VALUES
    (1, 1000, 2, 'e121', 10001),
    (2, 1000, 1, 'e122', 10002);
INSERT INTO e1_3 VALUES
    (1, 2003, 'e131', 10003),
    (1, 2001, 'e132', 10004);
INSERT INTO e2_3 VALUES (1000, 2, 2002, 'e231', 10005);

-- empty element path pattern, counts number of edges in the graph
SELECT count(*) FROM GRAPH_TABLE (g1 MATCH ()-[]->() COLUMNS (1 AS one));
SELECT count(*) FROM GRAPH_TABLE (g1 MATCH ()->() COLUMNS (1 AS one));
-- Project property associated with a label specified in the graph pattern even
-- if it is defined for a graph element through a different label. (Refer
-- section 6.5 of SQL/PGQ standard). For example, vprop1 in the query below. It
-- is defined on v2 through label vl3, but gets exposed in the query through
-- label vl1 which is not associated with v2. v2, in turn, is included because
-- of label vl2.
SELECT * FROM GRAPH_TABLE (g1 MATCH (a IS vl1 | vl2) COLUMNS (a.vname, a.vprop1));
-- vprop2 is associated with vl2 but not vl3
SELECT src, conn, dest, lprop1, vprop2, vprop1 FROM GRAPH_TABLE (g1 MATCH (a IS vl1)-[b IS el1]->(c IS vl2 | vl3) COLUMNS (a.vname AS src, b.ename AS conn, c.vname AS dest, c.lprop1, c.vprop2, c.vprop1));
-- edges directed in both ways - to and from v2
SELECT * FROM GRAPH_TABLE (g1 MATCH (v1 IS vl2)-[conn]-(v2) COLUMNS (v1.vname AS v1name, conn.ename AS cname, v2.vname AS v2name));
SELECT * FROM GRAPH_TABLE (g1 MATCH (v1 IS vl2)-(v2) COLUMNS (v1.vname AS v1name, v2.vname AS v2name));

-- Errors
-- vl1 is not associated with property vprop2
SELECT src, src_vprop2, conn, dest FROM GRAPH_TABLE (g1 MATCH (a IS vl1)-[b IS el1]->(c IS vl2 | vl3) COLUMNS (a.vname AS src, a.vprop2 AS src_vprop2, b.ename AS conn, c.vname AS dest));
-- property ename is associated with edge labels but not with a vertex label
SELECT * FROM GRAPH_TABLE (g1 MATCH (src)-[conn]->(dest) COLUMNS (src.vname AS svname, src.ename AS sename));
-- vname is associated vertex labels but not with an edge label
SELECT * FROM GRAPH_TABLE (g1 MATCH (src)-[conn]->(dest) COLUMNS (conn.vname AS cvname, conn.ename AS cename));
-- el1 is associated with only edges, and cannot qualify a vertex
SELECT * FROM GRAPH_TABLE (g1 MATCH (src IS el1)-[conn]->(dest) COLUMNS (conn.ename AS cename));
SELECT * FROM GRAPH_TABLE (g1 MATCH (src IS el1 | vl1)-[conn]->(dest) COLUMNS (conn.ename AS cename));
-- star in COLUMNs is specified but not supported
SELECT * FROM GRAPH_TABLE (myshop MATCH (c IS customers WHERE c.address = 'US')-[IS customer_orders]->(o IS orders) COLUMNS (c.*));
-- star anywhere else is not allowed as a property reference
SELECT * FROM GRAPH_TABLE (myshop MATCH (c IS customers WHERE c.* IS NOT NULL)-[IS customer_orders]->(o IS orders) COLUMNS (c.name));
-- consecutive element patterns with same kind
SELECT * FROM GRAPH_TABLE (g1 MATCH ()() COLUMNS (1 as one));
SELECT * FROM GRAPH_TABLE (g1 MATCH -> COLUMNS (1 AS one));
SELECT * FROM GRAPH_TABLE (g1 MATCH ()-[]- COLUMNS (1 AS one));
SELECT * FROM GRAPH_TABLE (g1 MATCH ()-> ->() COLUMNS (1 AS one));


-- select all the properties across all the labels associated with a given type
-- of graph element
SELECT * FROM GRAPH_TABLE (g1 MATCH (src)-[conn]->(dest) COLUMNS (src.vname AS svname, conn.ename AS cename, dest.vname AS dvname, src.vprop1 AS svp1, src.vprop2 AS svp2, src.lprop1 AS slp1, dest.vprop1 AS dvp1, dest.vprop2 AS dvp2, dest.lprop1 AS dlp1, conn.eprop1 AS cep1, conn.lprop2 AS clp2));
-- three label disjunction
SELECT * FROM GRAPH_TABLE (g1 MATCH (src IS vl1 | vl2 | vl3)-[conn]->(dest) COLUMNS (src.vname AS svname, conn.ename AS cename, dest.vname AS dvname));
-- graph'ical query: find a vertex which is not connected to any other vertex as a source or a destination.
WITH all_connected_vertices AS (SELECT svn, dvn FROM GRAPH_TABLE (g1 MATCH (src)-[conn]->(dest) COLUMNS (src.vname AS svn, dest.vname AS dvn))),
    all_vertices AS (SELECT vn FROM GRAPH_TABLE (g1 MATCH (vertex) COLUMNS (vertex.vname AS vn)))
SELECT vn FROM all_vertices EXCEPT (SELECT svn FROM all_connected_vertices UNION SELECT dvn FROM all_connected_vertices) ORDER BY vn;
-- query all connections using a label shared by vertices and edges
SELECT sn, cn, dn FROM GRAPH_TABLE (g1 MATCH (src IS l1)-[conn IS l1]->(dest IS l1) COLUMNS (src.elname AS sn, conn.elname AS cn, dest.elname AS dn));

-- Tests for cyclic path patterns
CREATE TABLE e2_1 (
    id_2_1 int,
    id_2_2 int,
    id_1 int,
    ename varchar(10),
    eprop1 int
);

CREATE TABLE e3_2 (
    id_3 int,
    id_2_1 int,
    id_2_2 int,
    ename varchar(10),
    eprop1 int
);

ALTER PROPERTY GRAPH g1
    ADD EDGE TABLES (
        e2_1 KEY (id_2_1, id_2_2, id_1)
            SOURCE KEY (id_2_1, id_2_2) REFERENCES v2 (id1, id2)
            DESTINATION KEY (id_1) REFERENCES v1 (id)
            LABEL el1 PROPERTIES (eprop1, ename)
            LABEL l1 PROPERTIES (ename AS elname),
        e3_2 KEY (id_3, id_2_1, id_2_2)
            SOURCE KEY (id_3) REFERENCES v3 (id)
            DESTINATION KEY (id_2_1, id_2_2) REFERENCES v2 (id1, id2)
            LABEL el2 PROPERTIES (ename, eprop1 * 10 AS lprop2)
            LABEL l1 PROPERTIES (ename AS elname)
    );

INSERT INTO e1_2 VALUES (3, 1000, 3, 'e123', 10007);
INSERT INTO e2_1 VALUES (1000, 1, 2, 'e211', 10006);
INSERT INTO e2_1 VALUES (1000, 3, 3, 'e212', 10008);
INSERT INTO e3_2 VALUES (2002, 1000, 2, 'e321', 10009);

-- cyclic pattern using WHERE clause in graph pattern,
SELECT * FROM GRAPH_TABLE (g1 MATCH (a)->(b)->(c) WHERE a.vname = c.vname COLUMNS (a.vname AS self, b.vname AS through, a.vprop1 AS self_p1, b.vprop1 AS through_p1)) ORDER BY self, through;
-- cyclic pattern using element patterns with the same variable name
SELECT * FROM GRAPH_TABLE (g1 MATCH (a)->(b)->(a) COLUMNS (a.vname AS self, b.vname AS through, a.vprop1 AS self_p1, b.vprop1 AS through_p1)) ORDER BY self, through;
-- cyclic pattern with WHERE clauses in element patterns with the same variable name
SELECT * FROM GRAPH_TABLE (g1 MATCH (a WHERE a.vprop1 < 2000)->(b WHERE b.vprop1 > 20)->(a WHERE a.vprop1 > 20) COLUMNS (a.vname AS self, b.vname AS through, a.vprop1 AS self_p1, b.vprop1 AS through_p1)) ORDER BY self, through;
SELECT * FROM GRAPH_TABLE (g1 MATCH (a)->(b WHERE b.vprop1 > 20)->(a WHERE a.vprop1 between 20 and 2000) COLUMNS (a.vname AS self, b.vname AS through, a.vprop1 AS self_p1, b.vprop1 AS through_p1)) ORDER BY self, through;
SELECT * FROM GRAPH_TABLE (g1 MATCH (a WHERE a.vprop1 between 20 and 2000)->(b WHERE b.vprop1 > 20)->(a WHERE a.vprop1 between 20 and 2000) COLUMNS (a.vname AS self, b.vname AS through, a.vprop1 AS self_p1, b.vprop1 AS through_p1)) ORDER BY self, through;
-- labels and elements kinds of element patterns with the same variable name
SELECT * FROM GRAPH_TABLE (g1 MATCH (a IS l1)-[a IS l1]->(b IS l1) COLUMNS (a.ename AS aename, b.ename AS bename)) ORDER BY 1, 2; -- error
SELECT * FROM GRAPH_TABLE (g1 MATCH (a IS vl1)->(b)->(a IS vl2) WHERE a.vname <> b.vname COLUMNS (a.vname AS self, b.vname AS through, a.vprop1 AS self_p1, b.vprop1 AS through_p1)) ORDER BY self, through;  -- error
SELECT * FROM GRAPH_TABLE (g1 MATCH (a IS vl1)->(b)->(a) COLUMNS (a.vname AS self, b.vname AS through, a.vprop1 AS self_p1, b.vprop1 AS through_p1)) ORDER BY self, through;
SELECT * FROM GRAPH_TABLE (g1 MATCH (a)->(b)->(a IS vl1) COLUMNS (a.vname AS self, b.vname AS through, a.vprop1 AS self_p1, b.vprop1 AS through_p1)) ORDER BY self, through;

-- add loop to test edge patterns with same variable name
CREATE TABLE e3_3 (
    src_id int,
    dest_id int,
    ename varchar(10),
    eprop1 int
);

ALTER PROPERTY GRAPH g1
    ADD EDGE TABLES (
        e3_3 KEY (src_id, dest_id)
            SOURCE KEY (src_id) REFERENCES v3 (id)
            DESTINATION KEY (dest_id) REFERENCES v3 (id)
            LABEL el2 PROPERTIES (ename, eprop1 * 10 AS lprop2)
            LABEL l1 PROPERTIES (ename AS elname)
    );

INSERT INTO e3_3 VALUES (2003, 2003, 'e331', 10010);
SELECT * FROM GRAPH_TABLE (g1 MATCH (a)-[b]->(a)-[b]->(a) COLUMNS (a.vname AS self, b.ename AS loop_name));
SELECT * FROM GRAPH_TABLE (g1 MATCH (a)-[b]->(c)-[b]->(d) COLUMNS (a.vname AS aname, b.ename AS bname, c.vname AS cname, d.vname AS dname)); --error
-- the looping edge should be reported only once even when edge pattern with any direction is used
SELECT * FROM GRAPH_TABLE (g1 MATCH (a)-[c]-(a) COLUMNS (a.vname AS self, c.ename AS loop_name));
SELECT * FROM GRAPH_TABLE (g1 MATCH (a)-(a) COLUMNS (a.vname AS self));

-- test collation specified in the expression
INSERT INTO e3_3 VALUES (2003, 2003, 'E331', 10011);
SELECT * FROM GRAPH_TABLE (g1 MATCH (a)-[b]->(a)-[b]->(a) COLUMNS (a.vname AS self, b.ename AS loop_name)) ORDER BY loop_name COLLATE "C" ASC;
SELECT * FROM GRAPH_TABLE (g1 MATCH (a)-[b IS el2 WHERE b.ename > 'E331' COLLATE "C"]->(a)-[b]->(a) COLUMNS (a.vname AS self, b.ename AS loop_name));
SELECT * FROM GRAPH_TABLE (g1 MATCH (a)-[b]->(a)-[b]->(a) WHERE b.ename > 'E331' COLLATE "C" COLUMNS (a.vname AS self, b.ename AS loop_name));
SELECT * FROM GRAPH_TABLE (g1 MATCH (a)-[b]->(a)-[b]->(a) COLUMNS (a.vname AS self, b.ename AS loop_name)) WHERE loop_name > 'E331' COLLATE "C";

-- property graph with some of the elements, labels and properties same as the
-- previous one. Test whether components from the specified property graph are
-- used. Also test explicit collation specification in property.
CREATE PROPERTY GRAPH g2
    VERTEX TABLES (
        v1
            LABEL l1 PROPERTIES ('g2.' || vname COLLATE "C" AS elname),
        v2 KEY (id1, id2)
            LABEL l1 PROPERTIES ('g2.' || vname COLLATE "C" AS elname),
        v3
            LABEL l1 PROPERTIES ('g2.' || vname COLLATE "C" AS elname)
    )
    EDGE TABLES (
        e1_2 key (id_1, id_2_1, id_2_2)
            SOURCE KEY (id_1) REFERENCES v1 (id)
            DESTINATION KEY (id_2_1, id_2_2) REFERENCES v2 (id1, id2)
            LABEL l1 PROPERTIES ('g2.' || ename COLLATE "C" AS elname),
        e1_3
            SOURCE KEY (id_1) REFERENCES v1 (id)
            DESTINATION KEY (id_3) REFERENCES v3 (id)
            LABEL l1 PROPERTIES ('g2.' || ename COLLATE "C" AS elname),
        e2_3 KEY (id_2_1, id_2_2, id_3)
            SOURCE KEY (id_2_1, id_2_2) REFERENCES v2 (id1, id2)
            DESTINATION KEY (id_3) REFERENCES v3 (id)
            LABEL l1 PROPERTIES ('g2.' || ename COLLATE "C" AS elname),
        e3_3 KEY (src_id, dest_id)
            SOURCE KEY (src_id) REFERENCES v3 (id)
            DESTINATION KEY (src_id) REFERENCES v3 (id)
            LABEL l1 PROPERTIES ('g2.' || ename COLLATE "C" AS elname)
    );
SELECT sn, cn, dn FROM GRAPH_TABLE (g2 MATCH (src IS l1)-[conn IS l1]->(dest IS l1) COLUMNS (src.elname AS sn, conn.elname AS cn, dest.elname AS dn)) ORDER BY 1, 2, 3;
SELECT * FROM GRAPH_TABLE (g2 MATCH (a)-[b WHERE b.elname > 'g2.E331']->(a)-[b]->(a) COLUMNS (a.elname AS self, b.elname AS loop_name));
SELECT * FROM GRAPH_TABLE (g2 MATCH (a)-[b]->(a)-[b]->(a) WHERE b.elname > 'g2.E331' COLUMNS (a.elname AS self, b.elname AS loop_name));
SELECT * FROM GRAPH_TABLE (g2 MATCH (a)-[b]->(a)-[b]->(a) COLUMNS (a.elname AS self, b.elname AS loop_name)) WHERE loop_name > 'g2.E331';

-- prepared statements, any changes to the property graph should be reflected in
-- the already prepared statements
PREPARE cyclestmt AS SELECT * FROM GRAPH_TABLE (g1 MATCH (a IS l1)->(b IS l1)->(c IS l1) WHERE a.elname = c.elname COLUMNS (a.elname AS self, b.elname AS through)) ORDER BY self, through;
EXECUTE cyclestmt;
ALTER PROPERTY GRAPH g1 DROP EDGE TABLES (e3_2, e3_3);
EXECUTE cyclestmt;
ALTER PROPERTY GRAPH g1
    ADD EDGE TABLES (
        e3_2 KEY (id_3, id_2_1, id_2_2)
            SOURCE KEY (id_3) REFERENCES v3 (id)
            DESTINATION KEY (id_2_1, id_2_2) REFERENCES v2 (id1, id2)
            LABEL el2 PROPERTIES (ename, eprop1 * 10 AS lprop2)
            LABEL l1 PROPERTIES (ename AS elname)
    );
EXECUTE cyclestmt;
ALTER PROPERTY GRAPH g1 ALTER VERTEX TABLE v3 DROP LABEL l1;
EXECUTE cyclestmt;
ALTER PROPERTY GRAPH g1 ALTER VERTEX TABLE v3 ADD LABEL l1 PROPERTIES (vname AS elname);
EXECUTE cyclestmt;
ALTER PROPERTY GRAPH g1
    ADD EDGE TABLES (
        e3_3 KEY (src_id, dest_id)
            SOURCE KEY (src_id) REFERENCES v3 (id)
            DESTINATION KEY (src_id) REFERENCES v3 (id)
            LABEL l2 PROPERTIES (ename AS elname)
    );
PREPARE loopstmt AS SELECT * FROM GRAPH_TABLE (g1 MATCH (a)-[e IS l2]->(a) COLUMNS (e.elname AS loop)) ORDER BY loop COLLATE "C" ASC;
EXECUTE loopstmt;
ALTER PROPERTY GRAPH g1 ALTER EDGE TABLE e3_3 ALTER LABEL l2 DROP PROPERTIES (elname);
EXECUTE loopstmt; -- error
ALTER PROPERTY GRAPH g1 ALTER EDGE TABLE e3_3 ALTER LABEL l2 ADD PROPERTIES ((ename || '_new')::varchar(10) AS elname);
EXECUTE loopstmt;

-- inheritance and partitioning
CREATE TABLE pv (id int, val int);
CREATE TABLE cv1 () INHERITS (pv);
CREATE TABLE cv2 () INHERITS (pv);
INSERT INTO pv VALUES (1, 10);
INSERT INTO cv1 VALUES (2, 20);
INSERT INTO cv2 VALUES (3, 30);
CREATE TABLE pe (id int, src int, dest int, val int);
CREATE TABLE ce1 () INHERITS (pe);
CREATE TABLE ce2 () INHERITS (pe);
INSERT INTO pe VALUES (1, 1, 2, 100);
INSERT INTO ce1 VALUES (2, 2, 3, 200);
INSERT INTO ce2 VALUES (3, 3, 1, 300);
CREATE PROPERTY GRAPH g3
    NODE TABLES (
        pv KEY (id)
    )
    RELATIONSHIP TABLES (
        pe KEY (id)
            SOURCE KEY(src) REFERENCES pv(id)
            DESTINATION KEY(dest) REFERENCES pv(id)
    );
SELECT * FROM GRAPH_TABLE (g3 MATCH (s IS pv)-[e IS pe]->(d IS pv) COLUMNS (s.val, e.val, d.val)) ORDER BY 1, 2, 3;
-- temporary property graph
CREATE TEMPORARY PROPERTY GRAPH gtmp
    VERTEX TABLES (
        pv KEY (id)
    )
    EDGE TABLES (
        pe KEY (id)
            SOURCE KEY(src) REFERENCES pv(id)
            DESTINATION KEY(dest) REFERENCES pv(id)
    );
SELECT * FROM GRAPH_TABLE (gtmp MATCH (s IS pv)-[e IS pe]->(d IS pv) COLUMNS (s.val, e.val, d.val)) ORDER BY 1, 2, 3;

CREATE TABLE ptnv (id int PRIMARY KEY, val int) PARTITION BY LIST(id);
CREATE TABLE prtv1 PARTITION OF ptnv FOR VALUES IN (1, 2);
CREATE TABLE prtv2 PARTITION OF ptnv FOR VALUES IN (3);
INSERT INTO ptnv VALUES (1, 10), (2, 20), (3, 30);
CREATE TABLE ptne (id int PRIMARY KEY, src int REFERENCES ptnv(id), dest int REFERENCES ptnv(id), val int) PARTITION BY LIST(id);
CREATE TABLE ptne1 PARTITION OF ptne FOR VALUES IN (1, 2);
CREATE TABLE ptne2 PARTITION OF ptne FOR VALUES IN (3);
INSERT INTO ptne VALUES (1, 1, 2, 100), (2, 2, 3, 200), (3, 3, 1, 300);
CREATE PROPERTY GRAPH g4
    VERTEX TABLES (ptnv)
    EDGE TABLES (
        ptne
            SOURCE KEY (src) REFERENCES ptnv(id)
            DESTINATION KEY (dest) REFERENCES ptnv(id)
    );
SELECT * FROM GRAPH_TABLE (g4 MATCH (s IS ptnv)-[e IS ptne]->(d IS ptnv) COLUMNS (s.val, e.val, d.val)) ORDER BY 1, 2, 3;
-- edges from the same vertex in both directions connecting to other vertexes in the same table
SELECT * FROM GRAPH_TABLE (g4 MATCH (s)-[e]-(d) WHERE s.id = 3 COLUMNS (s.val, e.val, d.val)) ORDER BY 1, 2, 3;
SELECT * FROM GRAPH_TABLE (g4 MATCH (s WHERE s.id = 3)-[e]-(d) COLUMNS (s.val, e.val, d.val)) ORDER BY 1, 2, 3;

-- ruleutils reverse parsing
CREATE VIEW customers_us AS SELECT * FROM GRAPH_TABLE (myshop MATCH (c IS customers WHERE c.address = 'US')-[IS customer_orders | customer_wishlists ]->(l IS orders | wishlists)-[ IS list_items]->(p IS products) COLUMNS (c.name AS customer_name, p.name AS product_name)) ORDER BY customer_name, product_name;
SELECT pg_get_viewdef('customers_us'::regclass);

-- test view/graph nesting

CREATE VIEW customers_view AS SELECT customer_id, 'redacted' || customer_id AS name_redacted, address FROM customers;
SELECT * FROM customers;
SELECT * FROM customers_view;

CREATE PROPERTY GRAPH myshop2
    VERTEX TABLES (
        products,
        customers_view KEY (customer_id) LABEL customers,
        orders
    )
    EDGE TABLES (
        order_items KEY (order_items_id)
            SOURCE KEY (order_id) REFERENCES orders (order_id)
            DESTINATION KEY (product_no) REFERENCES products (product_no),
        customer_orders KEY (customer_orders_id)
            SOURCE KEY (customer_id) REFERENCES customers_view (customer_id)
            DESTINATION KEY (order_id) REFERENCES orders (order_id)
    );

CREATE VIEW customers_us_redacted AS SELECT * FROM GRAPH_TABLE (myshop2 MATCH (c IS customers WHERE c.address = 'US')-[IS customer_orders]->(o IS orders) COLUMNS (c.name_redacted AS customer_name_redacted));

SELECT * FROM customers_us_redacted;

-- GRAPH_TABLE in UDFs
CREATE FUNCTION out_degree(sname varchar) RETURNS varchar AS $$
DECLARE
    out_degree int;
BEGIN
    SELECT count(*) INTO out_degree FROM GRAPH_TABLE (g1 MATCH (src WHERE src.vname = sname)->() COLUMNS (src.vname));
    RETURN out_degree;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION direct_connections(sname varchar)
RETURNS TABLE (cname varchar, dname varchar)
AS $$
    SELECT cname, dname FROM GRAPH_TABLE (g1 MATCH (src WHERE src.vname = sname)-[conn]->(dst) COLUMNS (conn.ename AS cname, dst.vname AS dname));
$$ LANGUAGE SQL;

SELECT sname, out_degree(sname) FROM GRAPH_TABLE (g1 MATCH (src IS vl1) COLUMNS (src.vname AS sname));
SELECT sname, cname, dname FROM GRAPH_TABLE (g1 MATCH (src IS vl1) COLUMNS (src.vname AS sname)), LATERAL direct_connections(sname);

-- GRAPH_TABLE joined to a regular table
SELECT * FROM customers co, GRAPH_TABLE (myshop2 MATCH (cg IS customers WHERE cg.address = co.address)-[IS customer_orders]->(o IS orders) COLUMNS (cg.name_redacted AS customer_name_redacted)) WHERE co.customer_id = 1;

-- graph table in a subquery
SELECT * FROM customers co WHERE co.customer_id = (SELECT customer_id FROM GRAPH_TABLE (myshop2 MATCH (cg IS customers WHERE cg.address = 'US')-[IS customer_orders]->(o IS orders) COLUMNS (cg.customer_id)));

-- query within graph table
SELECT sname, dname FROM GRAPH_TABLE (g1 MATCH (src)->(dest) WHERE src.vprop1 > (SELECT max(v1.vprop1) FROM v1) COLUMNS(src.vname AS sname, dest.vname AS dname));
SELECT sname, dname FROM GRAPH_TABLE (g1 MATCH (src)->(dest) WHERE out_degree(src.vname) > (SELECT max(out_degree(nname)) FROM GRAPH_TABLE (g1 MATCH (node) COLUMNS (node.vname AS nname))) COLUMNS(src.vname AS sname, dest.vname AS dname));

-- leave the objects behind for pg_upgrade/pg_dump tests
