SET search_path = public;

DROP FUNCTION xslt_process(text,text);

DROP FUNCTION xslt_process(text,text,text);

DROP FUNCTION xpath_table(text,text,text,text,text);

DROP FUNCTION xpath_nodeset(text,text,text);

DROP FUNCTION xpath_nodeset(text,text);

DROP FUNCTION xpath_list(text,text);

DROP FUNCTION xpath_list(text,text,text);

DROP FUNCTION xpath_bool(text,text);

DROP FUNCTION xpath_number(text,text);

DROP FUNCTION xpath_nodeset(text,text,text,text);

DROP FUNCTION xpath_string(text,text);

DROP FUNCTION xml_encode_special_chars(text);

-- deprecated old name for xml_is_well_formed
DROP FUNCTION xml_valid(text);

DROP FUNCTION xml_is_well_formed(text);
