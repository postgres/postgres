package org.postgresql.jdbc3;


import java.sql.*;

/* $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/jdbc3/Attic/Jdbc3Statement.java,v 1.1 2002/08/14 20:35:40 barry Exp $
 * This class implements the java.sql.Statement interface for JDBC3.
 * However most of the implementation is really done in 
 * org.postgresql.jdbc3.AbstractJdbc3Statement or one of it's parents
 */
public class Jdbc3Statement extends org.postgresql.jdbc3.AbstractJdbc3Statement implements java.sql.Statement
{
   
	public Jdbc3Statement (Jdbc3Connection c)
	{
		super(c);
	}
    
}
