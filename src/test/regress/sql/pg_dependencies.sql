-- Tests for type pg_distinct

-- Invalid inputs
SELECT 'null'::pg_dependencies;
SELECT '{"a": 1}'::pg_dependencies;
SELECT '[]'::pg_dependencies;
SELECT '{}'::pg_dependencies;
SELECT '[null]'::pg_dependencies;
SELECT * FROM pg_input_error_info('null', 'pg_dependencies');
SELECT * FROM pg_input_error_info('{"a": 1}', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('{}', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[null]', 'pg_dependencies');

-- Invalid keys
SELECT '[{"attributes_invalid" : [2,3], "dependency" : 4}]'::pg_dependencies;
SELECT '[{"attributes" : [2,3], "invalid" : 3, "dependency" : 4}]'::pg_dependencies;
SELECT * FROM pg_input_error_info('[{"attributes_invalid" : [2,3], "dependency" : 4}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "invalid" : 3, "dependency" : 4}]', 'pg_dependencies');

-- Missing keys
SELECT '[{"attributes" : [2,3], "dependency" : 4}]'::pg_dependencies;
SELECT '[{"attributes" : [2,3], "degree" : 1.000}]'::pg_dependencies;
SELECT '[{"attributes" : [2,3], "dependency" : 4}]'::pg_dependencies;
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "dependency" : 4}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "degree" : 1.000}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "dependency" : 4}]', 'pg_dependencies');

-- Valid keys, too many attributes
SELECT '[{"attributes" : [1,2,3,4,5,6,7,8], "dependency" : 4, "degree": 1.000}]'::pg_dependencies;
SELECT * FROM pg_input_error_info('[{"attributes" : [1,2,3,4,5,6,7,8], "dependency" : 4, "degree": 1.000}]', 'pg_dependencies');

-- Special characters
SELECT '[{"attributes" : ["\ud83d",3], "dependency" : 4, "degree": 0.250}]'::pg_dependencies;
SELECT '[{"attributes" : [2,3], "dependency" : "\ud83d", "degree": 0.250}]'::pg_dependencies;
SELECT '[{"attributes" : [2,3], "dependency" : 4, "degree": "\ud83d"}]'::pg_dependencies;
SELECT '[{"\ud83d" : [2,3], "dependency" : 4, "degree": 0.250}]'::pg_dependencies;
SELECT * FROM pg_input_error_info('[{"attributes" : ["\ud83d",3], "dependency" : 4, "degree": 0.250}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "dependency" : "\ud83d", "degree": 0.250}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "dependency" : 4, "degree": "\ud83d"}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"\ud83d" : [2,3], "dependency" : 4, "degree": 0.250}]', 'pg_dependencies');

-- Valid keys, invalid values
SELECT '[{"attributes" : null, "dependency" : 4, "degree": 1.000}]'::pg_dependencies;
SELECT '[{"attributes" : [2,null], "dependency" : 4, "degree": 1.000}]'::pg_dependencies;
SELECT '[{"attributes" : [2,3], "dependency" : null, "degree": 1.000}]'::pg_dependencies;
SELECT '[{"attributes" : [2,"a"], "dependency" : 4, "degree": 1.000}]'::pg_dependencies;
SELECT '[{"attributes" : [2,3], "dependency" : "a", "degree": 1.000}]'::pg_dependencies;
SELECT '[{"attributes" : [2,3], "dependency" : [], "degree": 1.000}]'::pg_dependencies;
SELECT '[{"attributes" : [2,3], "dependency" : [null], "degree": 1.000}]'::pg_dependencies;
SELECT '[{"attributes" : [2,3], "dependency" : [1,null], "degree": 1.000}]'::pg_dependencies;
SELECT '[{"attributes" : 1, "dependency" : 4, "degree": 1.000}]'::pg_dependencies;
SELECT '[{"attributes" : "a", "dependency" : 4, "degree": 1.000}]'::pg_dependencies;
SELECT '[{"attributes" : [2,3], "dependency" : 4, "degree": NaN}]'::pg_dependencies;
SELECT * FROM pg_input_error_info('[{"attributes" : null, "dependency" : 4, "degree": 1.000}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,null], "dependency" : 4, "degree": 1.000}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "dependency" : null, "degree": 1.000}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,"a"], "dependency" : 4, "degree": 1.000}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "dependency" : "a", "degree": 1.000}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "dependency" : [], "degree": 1.000}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "dependency" : [null], "degree": 1.000}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "dependency" : [1,null], "degree": 1.000}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : 1, "dependency" : 4, "degree": 1.000}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : "a", "dependency" : 4, "degree": 1.000}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "dependency" : 4, "degree": NaN}]', 'pg_dependencies');

SELECT '[{"attributes": [], "dependency": 2, "degree": 1}]' ::pg_dependencies;
SELECT '[{"attributes" : {"a": 1}, "dependency" : 4, "degree": "1.2"}]'::pg_dependencies;
SELECT * FROM pg_input_error_info('[{"attributes": [], "dependency": 2, "degree": 1}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : {"a": 1}, "dependency" : 4, "degree": "1.2"}]', 'pg_dependencies');

SELECT '[{"dependency" : 4, "degree": "1.2"}]'::pg_dependencies;
SELECT '[{"attributes" : [1,2,3,4,5,6,7], "dependency" : 0, "degree": "1.2"}]'::pg_dependencies;
SELECT '[{"attributes" : [1,2,3,4,5,6,7], "dependency" : -9, "degree": "1.2"}]'::pg_dependencies;
SELECT '[{"attributes": [1,2], "dependency": 2, "degree": 1}]' ::pg_dependencies;
SELECT '[{"attributes" : [1, {}], "dependency" : 1, "degree": "1.2"}]'::pg_dependencies;
SELECT '[{"attributes" : [1,2], "dependency" : {}, "degree": 1.0}]'::pg_dependencies;
SELECT '[{"attributes" : [1,2], "dependency" : 3, "degree": {}}]'::pg_dependencies;
SELECT '[{"attributes" : [1,2], "dependency" : 1, "degree": "a"}]'::pg_dependencies;
SELECT * FROM pg_input_error_info('[{"dependency" : 4, "degree": "1.2"}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : [1,2,3,4,5,6,7], "dependency" : 0, "degree": "1.2"}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : [1,2,3,4,5,6,7], "dependency" : -9, "degree": "1.2"}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes": [1,2], "dependency": 2, "degree": 1}]' , 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : [1, {}], "dependency" : 1, "degree": "1.2"}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : [1,2], "dependency" : {}, "degree": 1.0}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : [1,2], "dependency" : 3, "degree": {}}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : [1,2], "dependency" : 1, "degree": "a"}]', 'pg_dependencies');

-- Funky degree values, which do not fail.
SELECT '[{"attributes" : [2], "dependency" : 4, "degree": "NaN"}]'::pg_dependencies;
SELECT '[{"attributes" : [2], "dependency" : 4, "degree": "-inf"}]'::pg_dependencies;
SELECT '[{"attributes" : [2], "dependency" : 4, "degree": "inf"}]'::pg_dependencies;
SELECT '[{"attributes" : [2], "dependency" : 4, "degree": "-inf"}]'::pg_dependencies::text::pg_dependencies;

-- Duplicated keys
SELECT '[{"attributes" : [2,3], "attributes": [1,2], "dependency" : 4, "degree": 1.000}]'::pg_dependencies;
SELECT '[{"attributes" : [2,3], "dependency" : 4, "dependency": 4, "degree": 1.000}]'::pg_dependencies;
SELECT '[{"attributes" : [2,3], "dependency": 4, "degree": 1.000, "degree": 1.000}]'::pg_dependencies;
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "attributes": [1,2], "dependency" : 4, "degree": 1.000}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "dependency" : 4, "dependency": 4, "degree": 1.000}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "dependency": 4, "degree": 1.000, "degree": 1.000}]', 'pg_dependencies');

-- Invalid attnums
SELECT '[{"attributes" : [0,2], "dependency" : 4, "degree": 0.500}]'::pg_dependencies;
SELECT '[{"attributes" : [-7,-9], "dependency" : 4, "degree": 0.500}]'::pg_dependencies;
SELECT * FROM pg_input_error_info('[{"attributes" : [0,2], "dependency" : 4, "degree": 0.500}]', 'pg_dependencies');
SELECT * FROM pg_input_error_info('[{"attributes" : [-7,-9], "dependency" : 4, "degree": 0.500}]', 'pg_dependencies');

-- Duplicated attributes
SELECT '[{"attributes" : [2,2], "dependency" : 4, "degree": 0.500}]'::pg_dependencies;
SELECT * FROM pg_input_error_info('[{"attributes" : [2,2], "dependency" : 4, "degree": 0.500}]', 'pg_dependencies');

-- Duplicated attribute lists.
SELECT '[{"attributes" : [2,3], "dependency" : 4, "degree": 1.000},
         {"attributes" : [2,3], "dependency" : 4, "degree": 1.000}]'::pg_dependencies;
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "dependency" : 4, "degree": 1.000},
         {"attributes" : [2,3], "dependency" : 4, "degree": 1.000}]', 'pg_dependencies');

-- Valid inputs
SELECT '[{"attributes" : [2,3], "dependency" : 4, "degree": 0.250},
         {"attributes" : [2,-1], "dependency" : 4, "degree": 0.500},
         {"attributes" : [2,3,-1], "dependency" : 4, "degree": 0.750},
         {"attributes" : [2,3,-1,-2], "dependency" : 4, "degree": 1.000}]'::pg_dependencies;
SELECT * FROM pg_input_error_info('[{"attributes" : [2,3], "dependency" : 4, "degree": 0.250},
         {"attributes" : [2,-1], "dependency" : 4, "degree": 0.500},
         {"attributes" : [2,3,-1], "dependency" : 4, "degree": 0.750},
         {"attributes" : [2,3,-1,-2], "dependency" : 4, "degree": 1.000}]', 'pg_dependencies');
-- Partially-covered attribute lists, possible as items with a degree of 0
-- are discarded.
SELECT '[{"attributes" : [2,3], "dependency" : 4, "degree": 1.000},
         {"attributes" : [1,-1], "dependency" : 4, "degree": 1.000},
         {"attributes" : [2,3,-1], "dependency" : 4, "degree": 1.000},
         {"attributes" : [2,3,-1,-2], "dependency" : 4, "degree": 1.000}]'::pg_dependencies;
