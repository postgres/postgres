package postgresql;

import java.lang.*;
import java.sql.*;
import java.util.*;
import postgresql.*;

/**
 * postgresql.PG_Object is a class used to describe unknown types 
 * An unknown type is any type that is unknown by JDBC Standards
 *
 * @version 1.0 15-APR-1997
 * @author <A HREF="mailto:adrian@hottub.org">Adrian Hall</A>
 */
public class PG_Object
{
	public String	type;
	public String	value;

	/**
	 *	Constructor for the PostgreSQL generic object
	 *
	 * @param type a string describing the type of the object
	 * @param value a string representation of the value of the object
	 */
	public PG_Object(String type, String value)
	{
		this.type = type;
		this.value = value;
	}
}
