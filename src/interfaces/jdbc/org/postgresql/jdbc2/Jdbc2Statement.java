package org.postgresql.jdbc2;


import java.sql.*;

/* $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/jdbc2/Attic/Jdbc2Statement.java,v 1.4 2003/02/04 09:20:10 barry Exp $
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

	public java.sql.ResultSet createResultSet (org.postgresql.Field[] fields, java.util.Vector tuples, String status, int updateCount, long insertOID, boolean binaryCursor) throws SQLException
	{
		return new Jdbc2ResultSet(this, fields, tuples, status, updateCount, insertOID, binaryCursor);
	}
}
