package org.postgresql.jdbc1;


import java.sql.*;

public class Jdbc1CallableStatement extends AbstractJdbc1Statement implements java.sql.CallableStatement
{

	public Jdbc1CallableStatement(Jdbc1Connection connection, String sql) throws SQLException
	{
		super(connection, sql);
	}

	public java.sql.ResultSet createResultSet (org.postgresql.Field[] fields, java.util.Vector tuples, String status, int updateCount, long insertOID, boolean binaryCursor) throws SQLException
	{
		return new Jdbc1ResultSet(this, fields, tuples, status, updateCount, insertOID, binaryCursor);
	}
}

