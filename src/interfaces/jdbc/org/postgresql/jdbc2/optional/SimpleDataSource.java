package org.postgresql.jdbc2.optional;

import javax.sql.DataSource;
import java.io.Serializable;

/**
 * Simple DataSource which does not perform connection pooling.  In order to use
 * the DataSource, you must set the property databaseName.	The settings for
 * serverName, portNumber, user, and password are optional.  Note: these properties
 * are declared in the superclass.
 *
 * @author Aaron Mulder (ammulder@chariotsolutions.com)
 * @version $Revision: 1.2 $
 */
public class SimpleDataSource extends BaseDataSource implements Serializable, DataSource
{
	/**
	 * Gets a description of this DataSource.
	 */
	public String getDescription()
	{
		return "Non-Pooling DataSource from " + org.postgresql.Driver.getVersion();
	}
}
