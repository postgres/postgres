package org.postgresql.jdbc1;


import java.sql.*;

/* $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/jdbc1/Attic/Jdbc1Statement.java,v 1.1 2002/07/23 03:59:55 barry Exp $
 * This class implements the java.sql.Statement interface for JDBC1.
 * However most of the implementation is really done in 
 * org.postgresql.jdbc1.AbstractJdbc1Statement
 */
public class Jdbc1Statement extends org.postgresql.jdbc1.AbstractJdbc1Statement implements java.sql.Statement
{

	public Jdbc1Statement (Jdbc1Connection c)
	{
		connection = c;
	}

}
