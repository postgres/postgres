package org.postgresql.test.jdbc2.optional;

import org.postgresql.test.JDBC2Tests;
import org.postgresql.jdbc2.optional.SimpleDataSource;

/**
 * Performs the basic tests defined in the superclass.	Just adds the
 * configuration logic.
 *
 * @author Aaron Mulder (ammulder@chariotsolutions.com)
 * @version $Revision: 1.2 $
 */
public class SimpleDataSourceTest extends BaseDataSourceTest
{
	/**
	 * Constructor required by JUnit
	 */
	public SimpleDataSourceTest(String name)
	{
		super(name);
	}

	/**
	 * Creates and configures a new SimpleDataSource.
	 */
	protected void initializeDataSource()
	{
		if (bds == null)
		{
			bds = new SimpleDataSource();
			String db = JDBC2Tests.getURL();
			if (db.indexOf('/') > -1)
			{
				db = db.substring(db.lastIndexOf('/') + 1);
			}
			else if (db.indexOf(':') > -1)
			{
				db = db.substring(db.lastIndexOf(':') + 1);
			}
			bds.setDatabaseName(db);
			bds.setUser(JDBC2Tests.getUser());
			bds.setPassword(JDBC2Tests.getPassword());
		}
	}
}
