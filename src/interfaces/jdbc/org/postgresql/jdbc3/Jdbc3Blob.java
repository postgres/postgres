package org.postgresql.jdbc3;


import java.sql.*;

public class Jdbc3Blob extends org.postgresql.jdbc3.AbstractJdbc3Blob implements java.sql.Blob
{

	public Jdbc3Blob(org.postgresql.PGConnection conn, int oid) throws SQLException
	{
		super(conn, oid);
	}

}
