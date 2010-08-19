--
-- CREATE_OPERATOR
--

CREATE OPERATOR ## ( 
   leftarg = path,
   rightarg = path,
   procedure = path_inter,
   commutator = ## 
);

CREATE OPERATOR <% (
   leftarg = point,
   rightarg = widget,
   procedure = pt_in_widget,
   commutator = >% ,
   negator = >=% 
);

CREATE OPERATOR @#@ (
   rightarg = int8,		-- left unary 
   procedure = numeric_fac 
);

CREATE OPERATOR #@# (
   leftarg = int8,		-- right unary
   procedure = numeric_fac
);

CREATE OPERATOR #%# ( 
   leftarg = int8,		-- right unary 
   procedure = numeric_fac 
);

-- Test comments
COMMENT ON OPERATOR ###### (int4, NONE) IS 'bad right unary';
