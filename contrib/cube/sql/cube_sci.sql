---
--- Testing cube output in scientific notation.  This was put into separate
--- test, because has platform-depending output.
---

SELECT '1e27'::cube AS cube;
SELECT '-1e27'::cube AS cube;
SELECT '1.0e27'::cube AS cube;
SELECT '-1.0e27'::cube AS cube;
SELECT '1e+27'::cube AS cube;
SELECT '-1e+27'::cube AS cube;
SELECT '1.0e+27'::cube AS cube;
SELECT '-1.0e+27'::cube AS cube;
SELECT '1e-7'::cube AS cube;
SELECT '-1e-7'::cube AS cube;
SELECT '1.0e-7'::cube AS cube;
SELECT '-1.0e-7'::cube AS cube;
SELECT '1e-300'::cube AS cube;
SELECT '-1e-300'::cube AS cube;
SELECT '1234567890123456'::cube AS cube;
SELECT '+1234567890123456'::cube AS cube;
SELECT '-1234567890123456'::cube AS cube;
