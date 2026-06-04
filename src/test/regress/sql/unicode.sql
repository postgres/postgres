SELECT getdatabaseencoding() <> 'UTF8' AS skip_test \gset
\if :skip_test
\quit
\endif

SELECT U&'\0061\0308bc' <> U&'\00E4bc' COLLATE "C" AS sanity_check;

SELECT unicode_version() IS NOT NULL;
SELECT unicode_assigned(U&'abc');
SELECT unicode_assigned(U&'abc\+10FFFF');

SELECT normalize('');
SELECT normalize(U&'\0061\0308\24D1c') = U&'\00E4\24D1c' COLLATE "C" AS test_default;
SELECT normalize(U&'\0061\0308\24D1c', NFC) = U&'\00E4\24D1c' COLLATE "C" AS test_nfc;
SELECT normalize(U&'\00E4bc', NFC) = U&'\00E4bc' COLLATE "C" AS test_nfc_idem;
SELECT normalize(U&'\00E4\24D1c', NFD) = U&'\0061\0308\24D1c' COLLATE "C" AS test_nfd;
SELECT normalize(U&'\0061\0308\24D1c', NFKC) = U&'\00E4bc' COLLATE "C" AS test_nfkc;
SELECT normalize(U&'\00E4\24D1c', NFKD) = U&'\0061\0308bc' COLLATE "C" AS test_nfkd;

SELECT "normalize"('abc', 'def');  -- run-time error

SELECT U&'\00E4\24D1c' IS NORMALIZED AS test_default;
SELECT U&'\00E4\24D1c' IS NFC NORMALIZED AS test_nfc;

SELECT num, val,
    val IS NFC NORMALIZED AS NFC,
    val IS NFD NORMALIZED AS NFD,
    val IS NFKC NORMALIZED AS NFKC,
    val IS NFKD NORMALIZED AS NFKD
FROM
  (VALUES (1, U&'\00E4bc'),
          (2, U&'\0061\0308bc'),
          (3, U&'\00E4\24D1c'),
          (4, U&'\0061\0308\24D1c'),
          (5, '')) vals (num, val)
ORDER BY num;

SELECT is_normalized('abc', 'def');  -- run-time error

-- Hangul NFC recomposition tests
-- L+V -> LV composition (first and last)
SELECT normalize(U&'\1100\1161', NFC) = U&'\AC00' COLLATE "C" AS hangul_lv_first;
SELECT normalize(U&'\1112\1175', NFC) = U&'\D788' COLLATE "C" AS hangul_lv_last;
-- LV+T -> LVT composition
SELECT normalize(U&'\AC00\11A8', NFC) = U&'\AC01' COLLATE "C" AS hangul_lvt_first_t;
SELECT normalize(U&'\AC00\11C2', NFC) = U&'\AC1B' COLLATE "C" AS hangul_lvt_last_t;
SELECT normalize(U&'\D788\11A8', NFC) = U&'\D789' COLLATE "C" AS hangul_lvt_last_lv;
-- L+V+T -> LVT composition
SELECT normalize(U&'\1100\1161\11A8', NFC) = U&'\AC01' COLLATE "C" AS hangul_full_lvt;
SELECT normalize(U&'\1112\1175\11C2', NFC) = U&'\D7A3' COLLATE "C" AS hangul_full_lvt;
-- TBASE invalid T syllable
SELECT normalize(U&'\AC00\11A7', NFC) = U&'\AC00\11A7' COLLATE "C" AS hangul_tbase_not_combined;
SELECT normalize(U&'\1100\1161\11A7', NFC) = U&'\AC00\11A7' COLLATE "C" AS hangul_lv_tbase_separate;

-- Hangul NFD decomposition tests
SELECT normalize(U&'\AC00', NFD) = U&'\1100\1161' COLLATE "C" AS hangul_nfd_lv;
SELECT normalize(U&'\AC01', NFD) = U&'\1100\1161\11A8' COLLATE "C" AS hangul_nfd_lvt;
SELECT normalize(U&'\D7A3', NFD) = U&'\1112\1175\11C2' COLLATE "C" AS hangul_nfd_last;
