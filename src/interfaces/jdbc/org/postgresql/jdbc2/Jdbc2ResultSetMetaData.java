package org.postgresql.jdbc2;

public class Jdbc2ResultSetMetaData extends AbstractJdbc2ResultSetMetaData implements java.sql.ResultSetMetaData
{
	public Jdbc2ResultSetMetaData(java.util.Vector rows, org.postgresql.Field[] fields)
	{
	    super(rows, fields);
	}
}

