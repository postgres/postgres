package org.postgresql.jdbc3;


import java.sql.*;

public class Jdbc3CallableStatement extends org.postgresql.jdbc3.AbstractJdbc3Statement implements java.sql.CallableStatement
{

	public Jdbc3CallableStatement(Jdbc3Connection connection, String sql) throws SQLException
	{
		super(connection, sql);
	}

}

