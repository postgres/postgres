package org.postgresql.jdbc1;


import java.sql.*;
import java.util.*;
import org.postgresql.core.Field;
import org.postgresql.util.PSQLException;

public class Jdbc1DatabaseMetaData extends AbstractJdbc1DatabaseMetaData implements java.sql.DatabaseMetaData
{
	public Jdbc1DatabaseMetaData(Jdbc1Connection conn)
	{
		super(conn);
	}

}
