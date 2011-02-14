/* contrib/xml2/xml2--unpackaged--1.0.sql */

ALTER EXTENSION xml2 ADD function xslt_process(text,text);
ALTER EXTENSION xml2 ADD function xslt_process(text,text,text);
ALTER EXTENSION xml2 ADD function xpath_table(text,text,text,text,text);
ALTER EXTENSION xml2 ADD function xpath_nodeset(text,text,text);
ALTER EXTENSION xml2 ADD function xpath_nodeset(text,text);
ALTER EXTENSION xml2 ADD function xpath_list(text,text);
ALTER EXTENSION xml2 ADD function xpath_list(text,text,text);
ALTER EXTENSION xml2 ADD function xpath_bool(text,text);
ALTER EXTENSION xml2 ADD function xpath_number(text,text);
ALTER EXTENSION xml2 ADD function xpath_nodeset(text,text,text,text);
ALTER EXTENSION xml2 ADD function xpath_string(text,text);
ALTER EXTENSION xml2 ADD function xml_encode_special_chars(text);
ALTER EXTENSION xml2 ADD function xml_valid(text);
ALTER EXTENSION xml2 ADD function xml_is_well_formed(text);
