package org.postgresql.jdbc1;

public class Jdbc1ResultSetMetaData extends AbstractJdbc1ResultSetMetaData implements java.sql.ResultSetMetaData
{
	public Jdbc1ResultSetMetaData(java.util.Vector rows, org.postgresql.Field[] fields)
	{
	    super(rows, fields);
	}

}

