package org.postgresql.jdbc2;


public class Jdbc2DatabaseMetaData extends AbstractJdbc2DatabaseMetaData implements java.sql.DatabaseMetaData
{
	public Jdbc2DatabaseMetaData(Jdbc2Connection conn)
	{
		super(conn);
	}

}
