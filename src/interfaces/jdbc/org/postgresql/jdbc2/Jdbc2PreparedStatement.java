package org.postgresql.jdbc2;


import java.sql.*;

public class Jdbc2PreparedStatement extends org.postgresql.jdbc2.AbstractJdbc2Statement implements java.sql.PreparedStatement
{

	public Jdbc2PreparedStatement(Jdbc2Connection connection, String sql) throws SQLException
	{
		super(connection, sql);
	}

	public java.sql.ResultSet createResultSet (org.postgresql.Field[] fields, java.util.Vector tuples, String status, int updateCount, long insertOID, boolean binaryCursor) throws SQLException
	{
		return new Jdbc2ResultSet(this, fields, tuples, status, updateCount, insertOID, binaryCursor);
	}
}

