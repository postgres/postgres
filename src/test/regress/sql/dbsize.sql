SELECT size, pg_size_pretty(size), pg_size_pretty(-1 * size) FROM
    (VALUES (10::bigint), (1000::bigint), (1000000::bigint),
            (1000000000::bigint), (1000000000000::bigint),
            (1000000000000000::bigint)) x(size);

SELECT size, pg_size_pretty(size), pg_size_pretty(-1 * size) FROM
    (VALUES (10::numeric), (1000::numeric), (1000000::numeric),
            (1000000000::numeric), (1000000000000::numeric),
            (1000000000000000::numeric),
            (10.5::numeric), (1000.5::numeric), (1000000.5::numeric),
            (1000000000.5::numeric), (1000000000000.5::numeric),
            (1000000000000000.5::numeric)) x(size);
