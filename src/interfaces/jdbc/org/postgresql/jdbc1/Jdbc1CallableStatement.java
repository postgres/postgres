package org.postgresql.jdbc1;


import java.sql.*;

public class Jdbc1CallableStatement extends AbstractJdbc1Statement implements java.sql.CallableStatement
{

	public Jdbc1CallableStatement(Jdbc1Connection connection, String sql) throws SQLException
	{
		super(connection, sql);
	}
}

