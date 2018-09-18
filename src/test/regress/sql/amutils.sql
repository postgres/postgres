--
-- Test index AM property-reporting functions
--

select prop,
       pg_indexam_has_property(a.oid, prop) as "AM",
       pg_index_has_property('onek_hundred'::regclass, prop) as "Index",
       pg_index_column_has_property('onek_hundred'::regclass, 1, prop) as "Column"
  from pg_am a,
       unnest(array['asc', 'desc', 'nulls_first', 'nulls_last',
                    'orderable', 'distance_orderable', 'returnable',
                    'search_array', 'search_nulls',
                    'clusterable', 'index_scan', 'bitmap_scan',
                    'backward_scan',
                    'can_order', 'can_unique', 'can_multi_col',
                    'can_exclude', 'can_include',
                    'bogus']::text[])
         with ordinality as u(prop,ord)
 where a.amname = 'btree'
 order by ord;

select prop,
       pg_indexam_has_property(a.oid, prop) as "AM",
       pg_index_has_property('gcircleind'::regclass, prop) as "Index",
       pg_index_column_has_property('gcircleind'::regclass, 1, prop) as "Column"
  from pg_am a,
       unnest(array['asc', 'desc', 'nulls_first', 'nulls_last',
                    'orderable', 'distance_orderable', 'returnable',
                    'search_array', 'search_nulls',
                    'clusterable', 'index_scan', 'bitmap_scan',
                    'backward_scan',
                    'can_order', 'can_unique', 'can_multi_col',
                    'can_exclude', 'can_include',
                    'bogus']::text[])
         with ordinality as u(prop,ord)
 where a.amname = 'gist'
 order by ord;

select prop,
       pg_index_column_has_property('onek_hundred'::regclass, 1, prop) as btree,
       pg_index_column_has_property('hash_i4_index'::regclass, 1, prop) as hash,
       pg_index_column_has_property('gcircleind'::regclass, 1, prop) as gist,
       pg_index_column_has_property('sp_radix_ind'::regclass, 1, prop) as spgist_radix,
       pg_index_column_has_property('sp_quad_ind'::regclass, 1, prop) as spgist_quad,
       pg_index_column_has_property('botharrayidx'::regclass, 1, prop) as gin,
       pg_index_column_has_property('brinidx'::regclass, 1, prop) as brin
  from unnest(array['asc', 'desc', 'nulls_first', 'nulls_last',
                    'orderable', 'distance_orderable', 'returnable',
                    'search_array', 'search_nulls',
                    'bogus']::text[])
         with ordinality as u(prop,ord)
 order by ord;

select prop,
       pg_index_has_property('onek_hundred'::regclass, prop) as btree,
       pg_index_has_property('hash_i4_index'::regclass, prop) as hash,
       pg_index_has_property('gcircleind'::regclass, prop) as gist,
       pg_index_has_property('sp_radix_ind'::regclass, prop) as spgist,
       pg_index_has_property('botharrayidx'::regclass, prop) as gin,
       pg_index_has_property('brinidx'::regclass, prop) as brin
  from unnest(array['clusterable', 'index_scan', 'bitmap_scan',
                    'backward_scan',
                    'bogus']::text[])
         with ordinality as u(prop,ord)
 order by ord;

select amname, prop, pg_indexam_has_property(a.oid, prop) as p
  from pg_am a,
       unnest(array['can_order', 'can_unique', 'can_multi_col',
                    'can_exclude', 'can_include', 'bogus']::text[])
         with ordinality as u(prop,ord)
 where amtype = 'i'
 order by amname, ord;

--
-- additional checks for pg_index_column_has_property
--
CREATE TEMP TABLE foo (f1 int, f2 int, f3 int, f4 int);

CREATE INDEX fooindex ON foo (f1 desc, f2 asc, f3 nulls first, f4 nulls last);

select col, prop, pg_index_column_has_property(o, col, prop)
  from (values ('fooindex'::regclass)) v1(o),
       (values (1,'orderable'),(2,'asc'),(3,'desc'),
               (4,'nulls_first'),(5,'nulls_last'),
               (6, 'bogus')) v2(idx,prop),
       generate_series(1,4) col
 order by col, idx;

CREATE INDEX foocover ON foo (f1) INCLUDE (f2,f3);

select col, prop, pg_index_column_has_property(o, col, prop)
  from (values ('foocover'::regclass)) v1(o),
       (values (1,'orderable'),(2,'asc'),(3,'desc'),
               (4,'nulls_first'),(5,'nulls_last'),
               (6,'distance_orderable'),(7,'returnable'),
               (8, 'bogus')) v2(idx,prop),
       generate_series(1,3) col
 order by col, idx;
