package org.postgresql.jdbc2;


import java.sql.*;

/* $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/jdbc2/Attic/Jdbc2Statement.java,v 1.3 2002/09/06 21:23:06 momjian Exp $
 * This class implements the java.sql.Statement interface for JDBC2.
 * However most of the implementation is really done in
 * org.postgresql.jdbc2.AbstractJdbc2Statement or one of it's parents
 */
public class Jdbc2Statement extends org.postgresql.jdbc2.AbstractJdbc2Statement implements java.sql.Statement
{

	public Jdbc2Statement (Jdbc2Connection c)
	{
		super(c);
	}

}
