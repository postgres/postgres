package org.postgresql.jdbc2;


import java.sql.*;

public class Jdbc2CallableStatement extends AbstractJdbc2Statement implements java.sql.CallableStatement
{

	public Jdbc2CallableStatement(Jdbc2Connection connection, String sql) throws SQLException
	{
		super(connection, sql);
	}

}

