--
-- OPERATOR DEFINITIONS
--
CREATE OPERATOR ## ( 
   leftarg = path,
   rightarg = path,
   procedure = path_inter,
   commutator = ## 
);

CREATE OPERATOR <% (
   leftarg = point,
   rightarg = circle,
   procedure = pt_in_circle,
   commutator = >=% 
);

CREATE OPERATOR @#@ (
   rightarg = int4,		-- left unary 
   procedure = int4fac 
);

CREATE OPERATOR #@# (
   leftarg = int4,		-- right unary
   procedure = int4fac 
);

CREATE OPERATOR #%# ( 
   leftarg = int4,		-- right unary 
   procedure = int4fac 
);

