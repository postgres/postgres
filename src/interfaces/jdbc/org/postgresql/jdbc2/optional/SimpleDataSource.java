package org.postgresql.jdbc2.optional;

import javax.sql.DataSource;
import java.io.Serializable;
import java.io.ObjectOutputStream;
import java.io.ObjectInputStream;
import java.io.IOException;

/**
 * Simple DataSource which does not perform connection pooling.  In order to use
 * the DataSource, you must set the property databaseName.	The settings for
 * serverName, portNumber, user, and password are optional.  Note: these properties
 * are declared in the superclass.
 *
 * @author Aaron Mulder (ammulder@chariotsolutions.com)
 * @version $Revision: 1.2.6.1 $
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

	private void writeObject(ObjectOutputStream out) throws IOException
	{
		writeBaseObject(out);
	}

	private void readObject(ObjectInputStream in) throws IOException, ClassNotFoundException
	{
		readBaseObject(in);
	}
}
