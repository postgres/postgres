package org.postgresql.jdbc1;


import java.util.Vector;
import java.sql.*;
import org.postgresql.Field;
import org.postgresql.util.PSQLException;

/* $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/jdbc1/Attic/Jdbc1Connection.java,v 1.1 2002/07/23 03:59:55 barry Exp $
 * This class implements the java.sql.Connection interface for JDBC1.
 * However most of the implementation is really done in 
 * org.postgresql.jdbc1.AbstractJdbc1Connection
 */
public class Jdbc1Connection extends org.postgresql.jdbc1.AbstractJdbc1Connection implements java.sql.Connection
{

	public java.sql.Statement createStatement() throws SQLException
	{
		return new org.postgresql.jdbc1.Jdbc1Statement(this);
	}

	public java.sql.PreparedStatement prepareStatement(String sql) throws SQLException
	{
		return new org.postgresql.jdbc1.PreparedStatement(this, sql);
	}

//BJL TODO - merge callable statement logic from jdbc2 to jdbc1
	public java.sql.CallableStatement prepareCall(String sql) throws SQLException
	{
		throw new PSQLException("postgresql.con.call");
	}

	public java.sql.DatabaseMetaData getMetaData() throws SQLException
	{
		if (metadata == null)
			metadata = new org.postgresql.jdbc1.DatabaseMetaData(this);
		return metadata;
	}

	public java.sql.ResultSet getResultSet(java.sql.Statement stat, Field[] fields, Vector tuples, String status, int updateCount, long insertOID, boolean binaryCursor) throws SQLException
	{
		return new Jdbc1ResultSet(this, fields, tuples, status, updateCount, insertOID, binaryCursor);
	}

}


