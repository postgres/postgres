-- Tests for SASLprep

CREATE EXTENSION test_saslprep;

-- Incomplete UTF-8 sequence.
SELECT test_saslprep('\xef');

-- Range of ASCII characters.
SELECT
    CASE
      WHEN a = 0   THEN '<NUL>'
      WHEN a < 32  THEN '<CTL_' || a::text || '>'
      WHEN a = 127 THEN '<DEL>'
      ELSE chr(a) END AS dat,
    set_byte('\x00'::bytea, 0, a) AS byt,
    test_saslprep(set_byte('\x00'::bytea, 0, a)) AS saslprep
  FROM generate_series(0,127) AS a;

DROP EXTENSION test_saslprep;
