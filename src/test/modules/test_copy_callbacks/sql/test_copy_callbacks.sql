CREATE EXTENSION test_copy_callbacks;
CREATE TABLE public.test (a INT, b INT, c INT);
INSERT INTO public.test VALUES (1, 2, 3), (12, 34, 56), (123, 456, 789);
SELECT test_copy_to_callback('public.test'::pg_catalog.regclass);
