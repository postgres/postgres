package org.postgresql.test.jdbc3;

import junit.framework.TestSuite;
import junit.framework.TestCase;
import junit.framework.Test;

import java.sql.*;

/*
 * Executes all known tests for JDBC3 
 */
public class Jdbc3TestSuite extends TestSuite
{

	/*
	 * The main entry point for JUnit
	 */
	public static TestSuite suite()
	{
	    //Currently there are no specific jdbc3 tests so just run the jdbc2 tests
            return org.postgresql.test.jdbc2.Jdbc2TestSuite.suite();
	}
}
