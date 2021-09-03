--
-- Test hash functions
--
-- When the salt is 0, the extended hash function should produce a result
-- whose low 32 bits match the standard hash function.  When the salt is
-- not 0, we should get a different result.
--

SELECT v as value, hashint2(v)::bit(32) as standard,
       hashint2extended(v, 0)::bit(32) as extended0,
       hashint2extended(v, 1)::bit(32) as extended1
FROM   (VALUES (0::int2), (1::int2), (17::int2), (42::int2)) x(v)
WHERE  hashint2(v)::bit(32) != hashint2extended(v, 0)::bit(32)
       OR hashint2(v)::bit(32) = hashint2extended(v, 1)::bit(32);

SELECT v as value, hashint4(v)::bit(32) as standard,
       hashint4extended(v, 0)::bit(32) as extended0,
       hashint4extended(v, 1)::bit(32) as extended1
FROM   (VALUES (0), (1), (17), (42), (550273), (207112489)) x(v)
WHERE  hashint4(v)::bit(32) != hashint4extended(v, 0)::bit(32)
       OR hashint4(v)::bit(32) = hashint4extended(v, 1)::bit(32);

SELECT v as value, hashint8(v)::bit(32) as standard,
       hashint8extended(v, 0)::bit(32) as extended0,
       hashint8extended(v, 1)::bit(32) as extended1
FROM   (VALUES (0), (1), (17), (42), (550273), (207112489)) x(v)
WHERE  hashint8(v)::bit(32) != hashint8extended(v, 0)::bit(32)
       OR hashint8(v)::bit(32) = hashint8extended(v, 1)::bit(32);

SELECT v as value, hashfloat4(v)::bit(32) as standard,
       hashfloat4extended(v, 0)::bit(32) as extended0,
       hashfloat4extended(v, 1)::bit(32) as extended1
FROM   (VALUES (0), (1), (17), (42), (550273), (207112489)) x(v)
WHERE  hashfloat4(v)::bit(32) != hashfloat4extended(v, 0)::bit(32)
       OR hashfloat4(v)::bit(32) = hashfloat4extended(v, 1)::bit(32);

SELECT v as value, hashfloat8(v)::bit(32) as standard,
       hashfloat8extended(v, 0)::bit(32) as extended0,
       hashfloat8extended(v, 1)::bit(32) as extended1
FROM   (VALUES (0), (1), (17), (42), (550273), (207112489)) x(v)
WHERE  hashfloat8(v)::bit(32) != hashfloat8extended(v, 0)::bit(32)
       OR hashfloat8(v)::bit(32) = hashfloat8extended(v, 1)::bit(32);

SELECT v as value, hashoid(v)::bit(32) as standard,
       hashoidextended(v, 0)::bit(32) as extended0,
       hashoidextended(v, 1)::bit(32) as extended1
FROM   (VALUES (0), (1), (17), (42), (550273), (207112489)) x(v)
WHERE  hashoid(v)::bit(32) != hashoidextended(v, 0)::bit(32)
       OR hashoid(v)::bit(32) = hashoidextended(v, 1)::bit(32);

SELECT v as value, hashchar(v)::bit(32) as standard,
       hashcharextended(v, 0)::bit(32) as extended0,
       hashcharextended(v, 1)::bit(32) as extended1
FROM   (VALUES (NULL::"char"), ('1'), ('x'), ('X'), ('p'), ('N')) x(v)
WHERE  hashchar(v)::bit(32) != hashcharextended(v, 0)::bit(32)
       OR hashchar(v)::bit(32) = hashcharextended(v, 1)::bit(32);

SELECT v as value, hashname(v)::bit(32) as standard,
       hashnameextended(v, 0)::bit(32) as extended0,
       hashnameextended(v, 1)::bit(32) as extended1
FROM   (VALUES (NULL), ('PostgreSQL'), ('eIpUEtqmY89'), ('AXKEJBTK'),
        ('muop28x03'), ('yi3nm0d73')) x(v)
WHERE  hashname(v)::bit(32) != hashnameextended(v, 0)::bit(32)
       OR hashname(v)::bit(32) = hashnameextended(v, 1)::bit(32);

SELECT v as value, hashtext(v)::bit(32) as standard,
       hashtextextended(v, 0)::bit(32) as extended0,
       hashtextextended(v, 1)::bit(32) as extended1
FROM   (VALUES (NULL), ('PostgreSQL'), ('eIpUEtqmY89'), ('AXKEJBTK'),
        ('muop28x03'), ('yi3nm0d73')) x(v)
WHERE  hashtext(v)::bit(32) != hashtextextended(v, 0)::bit(32)
       OR hashtext(v)::bit(32) = hashtextextended(v, 1)::bit(32);

SELECT v as value, hashoidvector(v)::bit(32) as standard,
       hashoidvectorextended(v, 0)::bit(32) as extended0,
       hashoidvectorextended(v, 1)::bit(32) as extended1
FROM   (VALUES (NULL::oidvector), ('0 1 2 3 4'), ('17 18 19 20'),
        ('42 43 42 45'), ('550273 550273 570274'),
        ('207112489 207112499 21512 2155 372325 1363252')) x(v)
WHERE  hashoidvector(v)::bit(32) != hashoidvectorextended(v, 0)::bit(32)
       OR hashoidvector(v)::bit(32) = hashoidvectorextended(v, 1)::bit(32);

SELECT v as value, hash_aclitem(v)::bit(32) as standard,
       hash_aclitem_extended(v, 0)::bit(32) as extended0,
       hash_aclitem_extended(v, 1)::bit(32) as extended1
FROM   (SELECT DISTINCT(relacl[1]) FROM pg_class LIMIT 10) x(v)
WHERE  hash_aclitem(v)::bit(32) != hash_aclitem_extended(v, 0)::bit(32)
       OR hash_aclitem(v)::bit(32) = hash_aclitem_extended(v, 1)::bit(32);

SELECT v as value, hashmacaddr(v)::bit(32) as standard,
       hashmacaddrextended(v, 0)::bit(32) as extended0,
       hashmacaddrextended(v, 1)::bit(32) as extended1
FROM   (VALUES (NULL::macaddr), ('08:00:2b:01:02:04'), ('08:00:2b:01:02:04'),
        ('e2:7f:51:3e:70:49'), ('d6:a9:4a:78:1c:d5'),
        ('ea:29:b1:5e:1f:a5')) x(v)
WHERE  hashmacaddr(v)::bit(32) != hashmacaddrextended(v, 0)::bit(32)
       OR hashmacaddr(v)::bit(32) = hashmacaddrextended(v, 1)::bit(32);

SELECT v as value, hashinet(v)::bit(32) as standard,
       hashinetextended(v, 0)::bit(32) as extended0,
       hashinetextended(v, 1)::bit(32) as extended1
FROM   (VALUES (NULL::inet), ('192.168.100.128/25'), ('192.168.100.0/8'),
        ('172.168.10.126/16'), ('172.18.103.126/24'), ('192.188.13.16/32')) x(v)
WHERE  hashinet(v)::bit(32) != hashinetextended(v, 0)::bit(32)
       OR hashinet(v)::bit(32) = hashinetextended(v, 1)::bit(32);

SELECT v as value, hash_numeric(v)::bit(32) as standard,
       hash_numeric_extended(v, 0)::bit(32) as extended0,
       hash_numeric_extended(v, 1)::bit(32) as extended1
FROM   (VALUES (0), (1.149484958), (17.149484958), (42.149484958),
        (149484958.550273), (2071124898672)) x(v)
WHERE  hash_numeric(v)::bit(32) != hash_numeric_extended(v, 0)::bit(32)
       OR hash_numeric(v)::bit(32) = hash_numeric_extended(v, 1)::bit(32);

SELECT v as value, hashmacaddr8(v)::bit(32) as standard,
       hashmacaddr8extended(v, 0)::bit(32) as extended0,
       hashmacaddr8extended(v, 1)::bit(32) as extended1
FROM   (VALUES (NULL::macaddr8), ('08:00:2b:01:02:04:36:49'),
        ('08:00:2b:01:02:04:f0:e8'), ('e2:7f:51:3e:70:49:16:29'),
        ('d6:a9:4a:78:1c:d5:47:32'), ('ea:29:b1:5e:1f:a5')) x(v)
WHERE  hashmacaddr8(v)::bit(32) != hashmacaddr8extended(v, 0)::bit(32)
       OR hashmacaddr8(v)::bit(32) = hashmacaddr8extended(v, 1)::bit(32);

SELECT v as value, hash_array(v)::bit(32) as standard,
       hash_array_extended(v, 0)::bit(32) as extended0,
       hash_array_extended(v, 1)::bit(32) as extended1
FROM   (VALUES ('{0}'::int4[]), ('{0,1,2,3,4}'), ('{17,18,19,20}'),
        ('{42,34,65,98}'), ('{550273,590027, 870273}'),
        ('{207112489, 807112489}')) x(v)
WHERE  hash_array(v)::bit(32) != hash_array_extended(v, 0)::bit(32)
       OR hash_array(v)::bit(32) = hash_array_extended(v, 1)::bit(32);

-- array hashing with non-hashable element type
SELECT v as value, hash_array(v)::bit(32) as standard
FROM   (VALUES ('{0}'::money[])) x(v);
SELECT v as value, hash_array_extended(v, 0)::bit(32) as extended0
FROM   (VALUES ('{0}'::money[])) x(v);

SELECT v as value, hashbpchar(v)::bit(32) as standard,
       hashbpcharextended(v, 0)::bit(32) as extended0,
       hashbpcharextended(v, 1)::bit(32) as extended1
FROM   (VALUES (NULL), ('PostgreSQL'), ('eIpUEtqmY89'), ('AXKEJBTK'),
        ('muop28x03'), ('yi3nm0d73')) x(v)
WHERE  hashbpchar(v)::bit(32) != hashbpcharextended(v, 0)::bit(32)
       OR hashbpchar(v)::bit(32) = hashbpcharextended(v, 1)::bit(32);

SELECT v as value, time_hash(v)::bit(32) as standard,
       time_hash_extended(v, 0)::bit(32) as extended0,
       time_hash_extended(v, 1)::bit(32) as extended1
FROM   (VALUES (NULL::time), ('11:09:59'), ('1:09:59'), ('11:59:59'),
        ('7:9:59'), ('5:15:59')) x(v)
WHERE  time_hash(v)::bit(32) != time_hash_extended(v, 0)::bit(32)
       OR time_hash(v)::bit(32) = time_hash_extended(v, 1)::bit(32);

SELECT v as value, timetz_hash(v)::bit(32) as standard,
       timetz_hash_extended(v, 0)::bit(32) as extended0,
       timetz_hash_extended(v, 1)::bit(32) as extended1
FROM   (VALUES (NULL::timetz), ('00:11:52.518762-07'), ('00:11:52.51762-08'),
        ('00:11:52.62-01'), ('00:11:52.62+01'), ('11:59:59+04')) x(v)
WHERE  timetz_hash(v)::bit(32) != timetz_hash_extended(v, 0)::bit(32)
       OR timetz_hash(v)::bit(32) = timetz_hash_extended(v, 1)::bit(32);

SELECT v as value, interval_hash(v)::bit(32) as standard,
       interval_hash_extended(v, 0)::bit(32) as extended0,
       interval_hash_extended(v, 1)::bit(32) as extended1
FROM   (VALUES (NULL::interval),
        ('5 month 7 day 46 minutes'), ('1 year 7 day 46 minutes'),
        ('1 year 7 month 20 day 46 minutes'), ('5 month'),
        ('17 year 11 month 7 day 9 hours 46 minutes 5 seconds')) x(v)
WHERE  interval_hash(v)::bit(32) != interval_hash_extended(v, 0)::bit(32)
       OR interval_hash(v)::bit(32) = interval_hash_extended(v, 1)::bit(32);

SELECT v as value, timestamp_hash(v)::bit(32) as standard,
       timestamp_hash_extended(v, 0)::bit(32) as extended0,
       timestamp_hash_extended(v, 1)::bit(32) as extended1
FROM   (VALUES (NULL::timestamp), ('2017-08-22 00:09:59.518762'),
        ('2015-08-20 00:11:52.51762-08'),
        ('2017-05-22 00:11:52.62-01'),
        ('2013-08-22 00:11:52.62+01'), ('2013-08-22 11:59:59+04')) x(v)
WHERE  timestamp_hash(v)::bit(32) != timestamp_hash_extended(v, 0)::bit(32)
       OR timestamp_hash(v)::bit(32) = timestamp_hash_extended(v, 1)::bit(32);

SELECT v as value, uuid_hash(v)::bit(32) as standard,
       uuid_hash_extended(v, 0)::bit(32) as extended0,
       uuid_hash_extended(v, 1)::bit(32) as extended1
FROM   (VALUES (NULL::uuid), ('a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11'),
        ('5a9ba4ac-8d6f-11e7-bb31-be2e44b06b34'),
        ('99c6705c-d939-461c-a3c9-1690ad64ed7b'),
        ('7deed3ca-8d6f-11e7-bb31-be2e44b06b34'),
        ('9ad46d4f-6f2a-4edd-aadb-745993928e1e')) x(v)
WHERE  uuid_hash(v)::bit(32) != uuid_hash_extended(v, 0)::bit(32)
       OR uuid_hash(v)::bit(32) = uuid_hash_extended(v, 1)::bit(32);

SELECT v as value, pg_lsn_hash(v)::bit(32) as standard,
       pg_lsn_hash_extended(v, 0)::bit(32) as extended0,
       pg_lsn_hash_extended(v, 1)::bit(32) as extended1
FROM   (VALUES (NULL::pg_lsn), ('16/B374D84'), ('30/B374D84'),
        ('255/B374D84'), ('25/B379D90'), ('900/F37FD90')) x(v)
WHERE  pg_lsn_hash(v)::bit(32) != pg_lsn_hash_extended(v, 0)::bit(32)
       OR pg_lsn_hash(v)::bit(32) = pg_lsn_hash_extended(v, 1)::bit(32);

CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy');
SELECT v as value, hashenum(v)::bit(32) as standard,
       hashenumextended(v, 0)::bit(32) as extended0,
       hashenumextended(v, 1)::bit(32) as extended1
FROM   (VALUES ('sad'::mood), ('ok'), ('happy')) x(v)
WHERE  hashenum(v)::bit(32) != hashenumextended(v, 0)::bit(32)
       OR hashenum(v)::bit(32) = hashenumextended(v, 1)::bit(32);
DROP TYPE mood;

SELECT v as value, jsonb_hash(v)::bit(32) as standard,
       jsonb_hash_extended(v, 0)::bit(32) as extended0,
       jsonb_hash_extended(v, 1)::bit(32) as extended1
FROM   (VALUES (NULL::jsonb),
        ('{"a": "aaa bbb ddd ccc", "b": ["eee fff ggg"], "c": {"d": "hhh iii"}}'),
        ('{"foo": [true, "bar"], "tags": {"e": 1, "f": null}}'),
        ('{"g": {"h": "value"}}')) x(v)
WHERE  jsonb_hash(v)::bit(32) != jsonb_hash_extended(v, 0)::bit(32)
       OR jsonb_hash(v)::bit(32) = jsonb_hash_extended(v, 1)::bit(32);

SELECT v as value, hash_range(v)::bit(32) as standard,
       hash_range_extended(v, 0)::bit(32) as extended0,
       hash_range_extended(v, 1)::bit(32) as extended1
FROM   (VALUES (int4range(10, 20)), (int4range(23, 43)),
        (int4range(5675, 550273)),
        (int4range(550274, 1550274)), (int4range(1550275, 208112489))) x(v)
WHERE  hash_range(v)::bit(32) != hash_range_extended(v, 0)::bit(32)
       OR hash_range(v)::bit(32) = hash_range_extended(v, 1)::bit(32);

SELECT v as value, hash_multirange(v)::bit(32) as standard,
	   hash_multirange_extended(v, 0)::bit(32) as extended0,
	   hash_multirange_extended(v, 1)::bit(32) as extended1
FROM   (VALUES ('{[10,20)}'::int4multirange), ('{[23, 43]}'::int4multirange),
         ('{[5675, 550273)}'::int4multirange),
		 ('{[550274, 1550274)}'::int4multirange),
		 ('{[1550275, 208112489)}'::int4multirange)) x(v)
WHERE  hash_multirange(v)::bit(32) != hash_multirange_extended(v, 0)::bit(32)
       OR hash_multirange(v)::bit(32) = hash_multirange_extended(v, 1)::bit(32);

CREATE TYPE hash_test_t1 AS (a int, b text);
SELECT v as value, hash_record(v)::bit(32) as standard,
       hash_record_extended(v, 0)::bit(32) as extended0,
       hash_record_extended(v, 1)::bit(32) as extended1
FROM   (VALUES (row(1, 'aaa')::hash_test_t1, row(2, 'bbb'), row(-1, 'ccc'))) x(v)
WHERE  hash_record(v)::bit(32) != hash_record_extended(v, 0)::bit(32)
       OR hash_record(v)::bit(32) = hash_record_extended(v, 1)::bit(32);
DROP TYPE hash_test_t1;

-- record hashing with non-hashable field type
CREATE TYPE hash_test_t2 AS (a money, b text);
SELECT v as value, hash_record(v)::bit(32) as standard
FROM   (VALUES (row(1, 'aaa')::hash_test_t2)) x(v);
SELECT v as value, hash_record_extended(v, 0)::bit(32) as extended0
FROM   (VALUES (row(1, 'aaa')::hash_test_t2)) x(v);
DROP TYPE hash_test_t2;

--
-- Check special cases for specific data types
--
SELECT hashfloat4('0'::float4) = hashfloat4('-0'::float4) AS t;
SELECT hashfloat4('NaN'::float4) = hashfloat4(-'NaN'::float4) AS t;
SELECT hashfloat8('0'::float8) = hashfloat8('-0'::float8) AS t;
SELECT hashfloat8('NaN'::float8) = hashfloat8(-'NaN'::float8) AS t;
SELECT hashfloat4('NaN'::float4) = hashfloat8('NaN'::float8) AS t;
