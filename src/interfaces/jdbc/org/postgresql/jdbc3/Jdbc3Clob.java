package org.postgresql.jdbc3;


public class Jdbc3Clob extends org.postgresql.jdbc3.AbstractJdbc3Clob implements java.sql.Clob
{

	public Jdbc3Clob(org.postgresql.PGConnection conn, int oid) throws java.sql.SQLException
	{
		super(conn, oid);
	}

}
