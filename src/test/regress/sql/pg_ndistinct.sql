-- Tests for type pg_ndistinct

-- Invalid inputs
SELECT 'null'::pg_ndistinct;
SELECT '{"a": 1}'::pg_ndistinct;
SELECT '[]'::pg_ndistinct;
SELECT '{}'::pg_ndistinct;
SELECT '[null]'::pg_ndistinct;
SELECT * FROM pg_input_error_info('null', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('{"a": 1}', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[]', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('{}', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[null]', 'pg_ndistinct');
-- Invalid keys
SELECT '[{"attributes_invalid" : [2,3], "ndistinct" : 4}]'::pg_ndistinct;
SELECT '[{"attributes" : [2,3], "invalid" : 3, "ndistinct" : 4}]'::pg_ndistinct;
SELECT '[{"attributes" : [2,3], "attributes" : [1,3], "ndistinct" : 4}]'::pg_ndistinct;
SELECT '[{"attributes" : [2,3], "ndistinct" : 4, "ndistinct" : 4}]'::pg_ndistinct;
SELECT * FROM pg_input_error_info('[{"attributes_invalid" : [2,3], "ndistinct" : 4}]', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "invalid" : 3, "ndistinct" : 4}]', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "attributes" : [1,3], "ndistinct" : 4}]', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "ndistinct" : 4, "ndistinct" : 4}]', 'pg_ndistinct');

-- Missing key
SELECT '[{"attributes" : [2,3]}]'::pg_ndistinct;
SELECT '[{"ndistinct" : 4}]'::pg_ndistinct;
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3]}]', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[{"ndistinct" : 4}]', 'pg_ndistinct');

-- Valid keys, too many attributes
SELECT '[{"attributes" : [1,2,3,4,5,6,7,8,9], "ndistinct" : 4}]'::pg_ndistinct;
SELECT * FROM pg_input_error_info('[{"attributes" : [1,2,3,4,5,6,7,8,9], "ndistinct" : 4}]', 'pg_ndistinct');

-- Special characters
SELECT '[{"\ud83d" : [1, 2], "ndistinct" : 4}]'::pg_ndistinct;
SELECT '[{"attributes" : [1, 2], "\ud83d" : 4}]'::pg_ndistinct;
SELECT '[{"attributes" : [1, 2], "ndistinct" : "\ud83d"}]'::pg_ndistinct;
SELECT '[{"attributes" : ["\ud83d", 2], "ndistinct" : 1}]'::pg_ndistinct;
SELECT * FROM pg_input_error_info('[{"\ud83d" : [1, 2], "ndistinct" : 4}]', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[{"attributes" : [1, 2], "\ud83d" : 4}]', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[{"attributes" : [1, 2], "ndistinct" : "\ud83d"}]', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[{"attributes" : ["\ud83d", 2], "ndistinct" : 1}]', 'pg_ndistinct');

-- Valid keys, invalid values
SELECT '[{"attributes" : null, "ndistinct" : 4}]'::pg_ndistinct;
SELECT '[{"attributes" : [], "ndistinct" : 1}]'::pg_ndistinct;
SELECT '[{"attributes" : [2], "ndistinct" : 4}]'::pg_ndistinct;
SELECT '[{"attributes" : [2,null], "ndistinct" : 4}]'::pg_ndistinct;
SELECT '[{"attributes" : [2,3], "ndistinct" : null}]'::pg_ndistinct;
SELECT '[{"attributes" : [2,"a"], "ndistinct" : 4}]'::pg_ndistinct;
SELECT '[{"attributes" : [2,3], "ndistinct" : "a"}]'::pg_ndistinct;
SELECT '[{"attributes" : [2,3], "ndistinct" : []}]'::pg_ndistinct;
SELECT '[{"attributes" : [2,3], "ndistinct" : [null]}]'::pg_ndistinct;
SELECT '[{"attributes" : [2,3], "ndistinct" : [1,null]}]'::pg_ndistinct;
SELECT '[{"attributes" : [2,3], "ndistinct" : {"a": 1}}]'::pg_ndistinct;
SELECT '[{"attributes" : [0,1], "ndistinct" : 1}]'::pg_ndistinct;
SELECT '[{"attributes" : [-7,-9], "ndistinct" : 1}]'::pg_ndistinct;
SELECT '[{"attributes" : 1, "ndistinct" : 4}]'::pg_ndistinct;
SELECT '[{"attributes" : "a", "ndistinct" : 4}]'::pg_ndistinct;
SELECT '[{"attributes" : {"a": 1}, "ndistinct" : 1}]'::pg_ndistinct;
SELECT '[{"attributes" : [1, {"a": 1}], "ndistinct" : 1}]'::pg_ndistinct;
SELECT * FROM pg_input_error_info('[{"attributes" : null, "ndistinct" : 4}]', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[{"attributes" : [], "ndistinct" : 1}]', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[{"attributes" : [2], "ndistinct" : 4}]', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,null], "ndistinct" : 4}]', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "ndistinct" : null}]', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,"a"], "ndistinct" : 4}]', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "ndistinct" : "a"}]', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "ndistinct" : []}]', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "ndistinct" : [null]}]', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "ndistinct" : [1,null]}]', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "ndistinct" : {"a": 1}}]', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[{"attributes" : 1, "ndistinct" : 4}]', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[{"attributes" : [-7,-9], "ndistinct" : 1}]', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[{"attributes" : 1, "ndistinct" : 4}]', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[{"attributes" : "a", "ndistinct" : 4}]', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[{"attributes" : {"a": 1}, "ndistinct" : 1}]', 'pg_ndistinct');
SELECT * FROM pg_input_error_info('[{"attributes" : [1, {"a": 1}], "ndistinct" : 1}]', 'pg_ndistinct');
-- Duplicated attributes
SELECT '[{"attributes" : [2,2], "ndistinct" : 4}]'::pg_ndistinct;
SELECT * FROM pg_input_error_info('[{"attributes" : [2,2], "ndistinct" : 4}]', 'pg_ndistinct');
-- Duplicated attribute lists.
SELECT '[{"attributes" : [2,3], "ndistinct" : 4},
         {"attributes" : [2,3], "ndistinct" : 4}]'::pg_ndistinct;
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "ndistinct" : 4},
         {"attributes" : [2,3], "ndistinct" : 4}]', 'pg_ndistinct');
-- Partially-covered attribute lists.
SELECT '[{"attributes" : [2,3], "ndistinct" : 4},
         {"attributes" : [2,-1], "ndistinct" : 4},
         {"attributes" : [2,3,-1], "ndistinct" : 4},
         {"attributes" : [1,3,-1,-2], "ndistinct" : 4}]'::pg_ndistinct;
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "ndistinct" : 4},
         {"attributes" : [2,-1], "ndistinct" : 4},
         {"attributes" : [2,3,-1], "ndistinct" : 4},
         {"attributes" : [1,3,-1,-2], "ndistinct" : 4}]', 'pg_ndistinct');

-- Valid inputs
-- Two attributes.
SELECT '[{"attributes" : [1,2], "ndistinct" : 4}]'::pg_ndistinct;
-- Three attributes.
SELECT '[{"attributes" : [2,-1], "ndistinct" : 1},
         {"attributes" : [3,-1], "ndistinct" : 2},
         {"attributes" : [2,3,-1], "ndistinct" : 3}]'::pg_ndistinct;
-- Three attributes with only two items.
SELECT '[{"attributes" : [2,-1], "ndistinct" : 1},
         {"attributes" : [2,3,-1], "ndistinct" : 3}]'::pg_ndistinct;
