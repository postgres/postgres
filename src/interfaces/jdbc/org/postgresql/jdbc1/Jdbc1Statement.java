package org.postgresql.jdbc1;


import java.sql.*;

/* $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/jdbc1/Attic/Jdbc1Statement.java,v 1.4 2003/02/04 09:20:10 barry Exp $
 * This class implements the java.sql.Statement interface for JDBC1.
 * However most of the implementation is really done in
 * org.postgresql.jdbc1.AbstractJdbc1Statement
 */
public class Jdbc1Statement extends org.postgresql.jdbc1.AbstractJdbc1Statement implements java.sql.Statement
{

	public Jdbc1Statement (Jdbc1Connection c)
	{
		super(c);
	}

	public java.sql.ResultSet createResultSet (org.postgresql.Field[] fields, java.util.Vector tuples, String status, int updateCount, long insertOID, boolean binaryCursor) throws SQLException
	{
		return new Jdbc1ResultSet(this, fields, tuples, status, updateCount, insertOID, binaryCursor);
	}
}
