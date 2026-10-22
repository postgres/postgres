--
-- SQL/JSON
--

SET pg_stat_statements.track_utility = FALSE;

CREATE TABLE pgss_sqljson_tab (js jsonb, t text, b bytea);

--
-- SQL/JSON query functions
--

-- JSON_VALUE ON EMPTY and ON ERROR behaviors
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT JSON_VALUE(js, '$.a') FROM pgss_sqljson_tab;
-- NULL ON EMPTY and NULL ON ERROR are the defaults
SELECT JSON_VALUE(js, '$.a' NULL ON EMPTY) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a' NULL ON EMPTY NULL ON ERROR) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a' ERROR ON EMPTY NULL ON ERROR) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a' ERROR ON EMPTY) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a' NULL ON EMPTY ERROR ON ERROR) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a' ERROR ON ERROR) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a' DEFAULT 1 ON EMPTY) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a' DEFAULT 2 ON EMPTY) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a' DEFAULT 1 ON ERROR) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a' DEFAULT 1 ON EMPTY DEFAULT 2 ON ERROR) FROM pgss_sqljson_tab;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C", calls;

-- JSON_VALUE RETURNING and PASSING
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT JSON_VALUE(js, '$.a') FROM pgss_sqljson_tab;
-- text is the default RETURNING type
SELECT JSON_VALUE(js, '$.a' RETURNING text) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a' RETURNING int) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a' RETURNING varchar(3)) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a' RETURNING varchar(4)) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a ? (@ > $x)' PASSING 1 AS x) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a ? (@ > $x)' PASSING 2 AS x) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a ? (@ > $y)' PASSING 1 AS y) FROM pgss_sqljson_tab;
SELECT JSON_VALUE(js, '$.a ? (@ > $x && @ < $y)'
  PASSING 1 AS x, 2 AS y) FROM pgss_sqljson_tab;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C", calls;

-- JSON_QUERY ON EMPTY and ON ERROR behaviors
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT JSON_QUERY(js, '$.a' EMPTY ARRAY ON EMPTY) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' EMPTY OBJECT ON EMPTY) FROM pgss_sqljson_tab;
-- EMPTY is a non-standard spelling of EMPTY ARRAY
SELECT JSON_QUERY(js, '$.a' EMPTY ON EMPTY) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' NULL ON EMPTY) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' ERROR ON EMPTY) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' DEFAULT '[]' ON EMPTY) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' EMPTY ON ERROR) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' EMPTY ARRAY ON ERROR) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' EMPTY OBJECT ON ERROR) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' ERROR ON ERROR) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' DEFAULT '[]' ON ERROR) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' EMPTY ARRAY ON EMPTY EMPTY OBJECT ON ERROR)
  FROM pgss_sqljson_tab;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C", calls;

-- JSON_QUERY WRAPPER, QUOTES, RETURNING, FORMAT JSON and PASSING
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT JSON_QUERY(js, '$.a') FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' WITHOUT WRAPPER) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' WITHOUT ARRAY WRAPPER) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' WITH WRAPPER) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' WITH ARRAY WRAPPER) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' WITH UNCONDITIONAL WRAPPER) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' WITH UNCONDITIONAL ARRAY WRAPPER) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' WITH CONDITIONAL WRAPPER) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' WITH CONDITIONAL ARRAY WRAPPER) FROM pgss_sqljson_tab;
-- KEEP QUOTES is the default
SELECT JSON_QUERY(js, '$.a' KEEP QUOTES) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' KEEP QUOTES ON SCALAR STRING) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' OMIT QUOTES) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' OMIT QUOTES ON SCALAR STRING) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' RETURNING jsonb) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' RETURNING json) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a' RETURNING text) FROM pgss_sqljson_tab;
-- FORMAT JSON in RETURNING is implied for JSON_QUERY
SELECT JSON_QUERY(js, '$.a' RETURNING text FORMAT JSON) FROM pgss_sqljson_tab;
SELECT JSON_QUERY(t, '$.a') FROM pgss_sqljson_tab;
SELECT JSON_QUERY(t FORMAT JSON, '$.a') FROM pgss_sqljson_tab;
SELECT JSON_QUERY(b FORMAT JSON, '$.a') FROM pgss_sqljson_tab;
SELECT JSON_QUERY(b FORMAT JSON ENCODING UTF8, '$.a') FROM pgss_sqljson_tab;
SELECT JSON_QUERY(b FORMAT JSON ENCODING UTF16, '$.a') FROM pgss_sqljson_tab;
SELECT JSON_QUERY(b FORMAT JSON ENCODING UTF32, '$.a') FROM pgss_sqljson_tab;
SELECT JSON_QUERY(js, '$.a ? (@ > $x)' PASSING 1 AS x) FROM pgss_sqljson_tab;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C", calls;

-- JSON_EXISTS ON ERROR behaviors and PASSING
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT JSON_EXISTS(js, 'strict $.a' FALSE ON ERROR) FROM pgss_sqljson_tab;
-- FALSE ON ERROR is the default
SELECT JSON_EXISTS(js, 'strict $.a') FROM pgss_sqljson_tab;
SELECT JSON_EXISTS(js, 'strict $.a' TRUE ON ERROR) FROM pgss_sqljson_tab;
SELECT JSON_EXISTS(js, 'strict $.a' UNKNOWN ON ERROR) FROM pgss_sqljson_tab;
SELECT JSON_EXISTS(js, 'strict $.a' ERROR ON ERROR) FROM pgss_sqljson_tab;
SELECT JSON_EXISTS(js, '$.a ? (@ > $x)' PASSING 1 AS x) FROM pgss_sqljson_tab;
SELECT JSON_EXISTS(js, '$.a ? (@ > $x)' PASSING 2 AS x) FROM pgss_sqljson_tab;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C", calls;

--
-- JSON_TABLE
--

-- regular columns: types, ON EMPTY and ON ERROR behaviors
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a')) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a text PATH '$.a')) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a varchar(3) PATH '$.a')) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a varchar(4) PATH '$.a')) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a' ERROR ON EMPTY)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a' DEFAULT 1 ON EMPTY)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a' DEFAULT 2 ON EMPTY)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a' ERROR ON ERROR)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a' DEFAULT 1 ON ERROR)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a'
    DEFAULT 1 ON EMPTY ERROR ON ERROR)) jt;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C", calls;

-- EXISTS, FORMAT JSON and FOR ORDINALITY columns
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int EXISTS PATH '$.a' FALSE ON ERROR)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int EXISTS PATH '$.a')) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int EXISTS PATH '$.a' TRUE ON ERROR)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int EXISTS PATH '$.a' UNKNOWN ON ERROR)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int EXISTS PATH '$.a' ERROR ON ERROR)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a jsonb PATH '$.a')) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a jsonb FORMAT JSON PATH '$.a')) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a jsonb FORMAT JSON PATH '$.a' WITH WRAPPER)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a text FORMAT JSON PATH '$.a' OMIT QUOTES)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a jsonb FORMAT JSON PATH '$.a'
    EMPTY ARRAY ON EMPTY)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a jsonb FORMAT JSON PATH '$.a'
    EMPTY OBJECT ON EMPTY)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a jsonb FORMAT JSON PATH '$.a'
    EMPTY ARRAY ON EMPTY EMPTY OBJECT ON ERROR)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a FOR ORDINALITY)) jt;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C", calls;

-- column names and paths
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a')) jt;
-- only the column name and the path differ
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (b int PATH '$.b')) jt;
-- without PATH, the path is derived from the column name
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (b int)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a', b int PATH '$.b')) jt;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C", calls;

-- NESTED PATH
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a', b int PATH '$.b')) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' COLUMNS (b int PATH '$'))) jt;
-- only the paths and the path name differ
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a',
    NESTED PATH '$.c[*]' AS p1 COLUMNS (b int PATH '$'))) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' COLUMNS (b int PATH '$'),
    NESTED PATH '$.c[*]' COLUMNS (c int PATH '$'))) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' COLUMNS (b int PATH '$',
      NESTED PATH '$.c[*]' COLUMNS (c int PATH '$')))) jt;
-- PATH is optional after NESTED
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a',
    NESTED '$.b[*]' COLUMNS (b int PATH '$'))) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a',
    NESTED '$.b[*]' AS p1 COLUMNS (b int PATH '$'))) jt;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C", calls;

-- PLAN without NESTED PATH, which is the same as no PLAN
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' AS p0 COLUMNS (a int PATH '$.a') PLAN (p0)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' AS p0 COLUMNS (a int PATH '$.a')) jt;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C", calls;

-- PLAN with one NESTED PATH
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' AS p0 COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' AS p1 COLUMNS (b int PATH '$'))) jt;
-- PLAN (p0 OUTER p1) is the default plan
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' AS p0 COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' AS p1 COLUMNS (b int PATH '$'))
    PLAN (p0 OUTER p1)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' AS p0 COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' AS p1 COLUMNS (b int PATH '$'))
    PLAN (p0 INNER p1)) jt;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C", calls;

-- PLAN with sibling NESTED PATH
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' AS p0 COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' AS p1 COLUMNS (b int PATH '$'),
    NESTED PATH '$.c[*]' AS p2 COLUMNS (c int PATH '$'))) jt;
-- PLAN (p0 OUTER (p1 UNION p2)) is the default plan
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' AS p0 COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' AS p1 COLUMNS (b int PATH '$'),
    NESTED PATH '$.c[*]' AS p2 COLUMNS (c int PATH '$'))
    PLAN (p0 OUTER (p1 UNION p2))) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' AS p0 COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' AS p1 COLUMNS (b int PATH '$'),
    NESTED PATH '$.c[*]' AS p2 COLUMNS (c int PATH '$'))
    PLAN (p0 INNER (p1 UNION p2))) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' AS p0 COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' AS p1 COLUMNS (b int PATH '$'),
    NESTED PATH '$.c[*]' AS p2 COLUMNS (c int PATH '$'))
    PLAN (p0 OUTER (p1 CROSS p2))) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' AS p0 COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' AS p1 COLUMNS (b int PATH '$'),
    NESTED PATH '$.c[*]' AS p2 COLUMNS (c int PATH '$'))
    PLAN DEFAULT (INNER, CROSS)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' AS p0 COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' AS p1 COLUMNS (b int PATH '$'),
    NESTED PATH '$.c[*]' AS p2 COLUMNS (c int PATH '$'))
    PLAN DEFAULT (INNER)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' AS p0 COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' AS p1 COLUMNS (b int PATH '$'),
    NESTED PATH '$.c[*]' AS p2 COLUMNS (c int PATH '$'))
    PLAN DEFAULT (CROSS)) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' AS p0 COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' AS p1 COLUMNS (b int PATH '$'),
    NESTED PATH '$.c[*]' AS p2 COLUMNS (c int PATH '$'))
    PLAN DEFAULT (UNION, INNER)) jt;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C", calls;

-- PLAN with chained UNION and CROSS
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' AS p0 COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' AS p1 COLUMNS (b int PATH '$'),
    NESTED PATH '$.c[*]' AS p2 COLUMNS (c int PATH '$'),
    NESTED PATH '$.d[*]' AS p3 COLUMNS (d int PATH '$'))) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' AS p0 COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' AS p1 COLUMNS (b int PATH '$'),
    NESTED PATH '$.c[*]' AS p2 COLUMNS (c int PATH '$'),
    NESTED PATH '$.d[*]' AS p3 COLUMNS (d int PATH '$'))
    PLAN (p0 OUTER (p1 UNION p2 UNION p3))) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' AS p0 COLUMNS (a int PATH '$.a',
    NESTED PATH '$.b[*]' AS p1 COLUMNS (b int PATH '$'),
    NESTED PATH '$.c[*]' AS p2 COLUMNS (c int PATH '$'),
    NESTED PATH '$.d[*]' AS p3 COLUMNS (d int PATH '$'))
    PLAN (p0 OUTER (p1 CROSS p2 CROSS p3))) jt;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C", calls;

-- table-level ON ERROR and PASSING
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
-- EMPTY is a non-standard spelling of EMPTY ARRAY
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a') EMPTY ON ERROR) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a') EMPTY ARRAY ON ERROR) jt;
-- EMPTY ARRAY ON ERROR is the default
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a')) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*]' COLUMNS (a int PATH '$.a') ERROR ON ERROR) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*] ? (@.a > $x)' PASSING 1 AS x
    COLUMNS (a int PATH '$.a')) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*] ? (@.a > $x)' PASSING 2 AS x
    COLUMNS (a int PATH '$.a')) jt;
SELECT * FROM pgss_sqljson_tab,
  JSON_TABLE(js, '$[*] ? (@.a > $y)' PASSING 1 AS y
    COLUMNS (a int PATH '$.a')) jt;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C", calls;

--
-- SQL/JSON constructors and predicates
--

-- JSON_OBJECT and JSON_ARRAY
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT JSON_OBJECT('a' VALUE t) FROM pgss_sqljson_tab;
SELECT JSON_OBJECT('b' VALUE t) FROM pgss_sqljson_tab;
SELECT JSON_OBJECT('a': t) FROM pgss_sqljson_tab;
SELECT JSON_OBJECT('a' VALUE t, 'b' VALUE t) FROM pgss_sqljson_tab;
SELECT JSON_OBJECT('a' VALUE t NULL ON NULL) FROM pgss_sqljson_tab;
SELECT JSON_OBJECT('a' VALUE t ABSENT ON NULL) FROM pgss_sqljson_tab;
SELECT JSON_OBJECT('a' VALUE t WITH UNIQUE KEYS) FROM pgss_sqljson_tab;
SELECT JSON_OBJECT('a' VALUE t WITH UNIQUE) FROM pgss_sqljson_tab;
SELECT JSON_OBJECT('a' VALUE t WITHOUT UNIQUE KEYS) FROM pgss_sqljson_tab;
SELECT JSON_OBJECT('a' VALUE t WITHOUT UNIQUE) FROM pgss_sqljson_tab;
SELECT JSON_OBJECT('a' VALUE t RETURNING jsonb) FROM pgss_sqljson_tab;
SELECT JSON_OBJECT('a' VALUE t FORMAT JSON) FROM pgss_sqljson_tab;
SELECT JSON_OBJECT();
SELECT JSON_ARRAY(t) FROM pgss_sqljson_tab;
SELECT JSON_ARRAY(t, t) FROM pgss_sqljson_tab;
SELECT JSON_ARRAY(t ABSENT ON NULL) FROM pgss_sqljson_tab;
SELECT JSON_ARRAY(t NULL ON NULL) FROM pgss_sqljson_tab;
SELECT JSON_ARRAY(t RETURNING jsonb) FROM pgss_sqljson_tab;
SELECT JSON_ARRAY(t FORMAT JSON) FROM pgss_sqljson_tab;
SELECT JSON_ARRAY();
SELECT JSON_ARRAY(SELECT t FROM pgss_sqljson_tab);
SELECT JSON_ARRAY(SELECT t FROM pgss_sqljson_tab FORMAT JSON);
SELECT JSON_ARRAY(SELECT t FROM pgss_sqljson_tab RETURNING jsonb);
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C", calls;

-- JSON_OBJECTAGG and JSON_ARRAYAGG
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT JSON_OBJECTAGG(t VALUE t) FROM pgss_sqljson_tab;
SELECT JSON_OBJECTAGG(t VALUE t ABSENT ON NULL) FROM pgss_sqljson_tab;
SELECT JSON_OBJECTAGG(t VALUE t WITH UNIQUE KEYS) FROM pgss_sqljson_tab;
SELECT JSON_OBJECTAGG(t VALUE t RETURNING jsonb) FROM pgss_sqljson_tab;
SELECT JSON_OBJECTAGG(t VALUE t) FILTER (WHERE t IS NOT NULL)
  FROM pgss_sqljson_tab;
SELECT JSON_OBJECTAGG(t VALUE t) OVER () FROM pgss_sqljson_tab;
SELECT JSON_ARRAYAGG(t) FROM pgss_sqljson_tab;
SELECT JSON_ARRAYAGG(t NULL ON NULL) FROM pgss_sqljson_tab;
SELECT JSON_ARRAYAGG(t ORDER BY js) FROM pgss_sqljson_tab;
SELECT JSON_ARRAYAGG(t ORDER BY js DESC) FROM pgss_sqljson_tab;
SELECT JSON_ARRAYAGG(t RETURNING jsonb) FROM pgss_sqljson_tab;
SELECT JSON_ARRAYAGG(t) FILTER (WHERE t IS NOT NULL) FROM pgss_sqljson_tab;
SELECT JSON_ARRAYAGG(t) OVER () FROM pgss_sqljson_tab;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C", calls;

-- JSON, JSON_SCALAR, JSON_SERIALIZE and IS JSON
SELECT pg_stat_statements_reset() IS NOT NULL AS t;
SELECT JSON(t) FROM pgss_sqljson_tab;
SELECT JSON(t WITH UNIQUE KEYS) FROM pgss_sqljson_tab;
SELECT JSON(b FORMAT JSON ENCODING UTF8) FROM pgss_sqljson_tab;
SELECT JSON_SCALAR(t) FROM pgss_sqljson_tab;
SELECT JSON_SERIALIZE(js) FROM pgss_sqljson_tab;
SELECT JSON_SERIALIZE(js RETURNING bytea) FROM pgss_sqljson_tab;
SELECT JSON_SERIALIZE(js RETURNING bytea FORMAT JSON ENCODING UTF8)
  FROM pgss_sqljson_tab;
SELECT t IS JSON FROM pgss_sqljson_tab;
SELECT t IS JSON VALUE FROM pgss_sqljson_tab;
SELECT t IS JSON OBJECT FROM pgss_sqljson_tab;
SELECT t IS JSON ARRAY FROM pgss_sqljson_tab;
SELECT t IS JSON SCALAR FROM pgss_sqljson_tab;
SELECT t IS JSON WITH UNIQUE KEYS FROM pgss_sqljson_tab;
SELECT t IS JSON WITHOUT UNIQUE KEYS FROM pgss_sqljson_tab;
SELECT t IS NOT JSON FROM pgss_sqljson_tab;
SELECT calls, query FROM pg_stat_statements ORDER BY query COLLATE "C", calls;

DROP TABLE pgss_sqljson_tab;
