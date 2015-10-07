
-- encoding-sensitive tests for json

-- basic unicode input
SELECT '"\u"'::json;			-- ERROR, incomplete escape
SELECT '"\u00"'::json;			-- ERROR, incomplete escape
SELECT '"\u000g"'::json;		-- ERROR, g is not a hex digit
SELECT '"\u0000"'::json;		-- OK, legal escape
SELECT '"\uaBcD"'::json;		-- OK, uppercase and lower case both OK

-- handling of unicode surrogate pairs

select json '{ "a":  "\ud83d\ude04\ud83d\udc36" }' -> 'a' as correct_in_utf8;
select json '{ "a":  "\ud83d\ud83d" }' -> 'a'; -- 2 high surrogates in a row
select json '{ "a":  "\ude04\ud83d" }' -> 'a'; -- surrogates in wrong order
select json '{ "a":  "\ud83dX" }' -> 'a'; -- orphan high surrogate
select json '{ "a":  "\ude04X" }' -> 'a'; -- orphan low surrogate

--handling of simple unicode escapes

select json '{ "a":  "the Copyright \u00a9 sign" }' ->> 'a' as correct_in_utf8;
select json '{ "a":  "dollar \u0024 character" }' ->> 'a' as correct_everywhere;
select json '{ "a":  "null \u0000 escape" }' ->> 'a' as not_escaped;
