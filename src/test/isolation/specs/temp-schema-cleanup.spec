# Test cleanup of objects in temporary schema.

setup {
    CREATE TABLE s1_temp_schema(oid oid);
    -- to help create a long function
    CREATE FUNCTION exec(p_foo text) RETURNS void LANGUAGE plpgsql AS $$BEGIN EXECUTE p_foo; END;$$;
}

teardown {
    DROP TABLE s1_temp_schema;
    DROP FUNCTION exec(text);
}

session "s1"
setup {
    CREATE TEMPORARY TABLE just_to_create_temp_schema();
    DROP TABLE just_to_create_temp_schema;
    INSERT INTO s1_temp_schema SELECT pg_my_temp_schema();
}

step s1_advisory {
    SELECT pg_advisory_lock('pg_namespace'::regclass::int8);
}

step s1_create_temp_objects {

    -- create function large enough to be toasted, to ensure we correctly clean those up, a prior bug
    -- https://postgr.es/m/CAOFAq3BU5Mf2TTvu8D9n_ZOoFAeQswuzk7yziAb7xuw_qyw5gw%40mail.gmail.com
    SELECT exec(format($outer$
        CREATE OR REPLACE FUNCTION pg_temp.long() RETURNS text LANGUAGE sql AS $body$ SELECT %L; $body$$outer$,
	(SELECT string_agg(g.i::text||':'||random()::text, '|') FROM generate_series(1, 100) g(i))));

    -- The above bug requires function removal to happen after a catalog
    -- invalidation. dependency.c sorts objects in descending oid order so
    -- that newer objects are deleted before older objects, so create a
    -- table after.
    CREATE TEMPORARY TABLE invalidate_catalog_cache();

    -- test non-temp function is dropped when depending on temp table
    CREATE TEMPORARY TABLE just_give_me_a_type(id serial primary key);

    CREATE FUNCTION uses_a_temp_type(just_give_me_a_type) RETURNS int LANGUAGE sql AS $$SELECT 1;$$;
}

step s1_discard_temp {
    DISCARD TEMP;
}

step s1_exit {
    SELECT pg_terminate_backend(pg_backend_pid());
}


session "s2"

step s2_advisory {
    SELECT pg_advisory_lock('pg_namespace'::regclass::int8);
}

step s2_check_schema {
    SELECT oid::regclass FROM pg_class WHERE relnamespace = (SELECT oid FROM s1_temp_schema);
    SELECT oid::regproc FROM pg_proc WHERE pronamespace = (SELECT oid FROM s1_temp_schema);
    SELECT oid::regproc FROM pg_type WHERE typnamespace = (SELECT oid FROM s1_temp_schema);
}


# Test temporary object cleanup during DISCARD.
permutation
    s1_create_temp_objects
    s1_discard_temp
    s2_check_schema

# Test temporary object cleanup during process exit.
#
# To check (in s2) if temporary objects (in s1) have properly been removed we
# need to wait for s1 to finish cleaning up. Luckily session level advisory
# locks are released only after temp table cleanup.
permutation
    s1_advisory
    s2_advisory
    s1_create_temp_objects
    s1_exit
    s2_check_schema

# Can't run further tests here, because s1's connection is dead
