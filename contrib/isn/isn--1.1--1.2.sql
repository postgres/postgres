/* contrib/isn/isn--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION isn UPDATE TO '1.2'" to load this file. \quit

ALTER OPERATOR <= (ean13, ean13) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (ean13, ean13) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (ean13, isbn13) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (ean13, isbn13) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (isbn13, ean13) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (isbn13, ean13) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (ean13, ismn13) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (ean13, ismn13) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (ismn13, ean13) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (ismn13, ean13) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (ean13, issn13) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (ean13, issn13) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (ean13, isbn) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (ean13, isbn) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (ean13, ismn) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (ean13, ismn) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (ean13, issn) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (ean13, issn) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (ean13, upc) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (ean13, upc) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (isbn13, isbn13) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (isbn13, isbn13) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (isbn13, isbn) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (isbn13, isbn) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (isbn, isbn) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (isbn, isbn) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (isbn, isbn13) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (isbn, isbn13) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (isbn, ean13) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (isbn, ean13) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (ismn13, ismn13) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (ismn13, ismn13) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (ismn13, ismn) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (ismn13, ismn) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (ismn, ismn) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (ismn, ismn) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (ismn, ismn13) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (ismn, ismn13) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (ismn, ean13) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (ismn, ean13) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (issn13, issn13) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (issn13, issn13) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (issn13, issn) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (issn13, issn) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (issn13, ean13) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (issn13, ean13) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (issn, issn) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (issn, issn) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (issn, issn13) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (issn, issn13) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (issn, ean13) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (issn, ean13) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (upc, upc) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (upc, upc) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);

ALTER OPERATOR <= (upc, ean13) SET (
	RESTRICT = scalarlesel,
	JOIN = scalarlejoinsel);

ALTER OPERATOR >= (upc, ean13) SET (
	RESTRICT = scalargesel,
	JOIN = scalargejoinsel);
