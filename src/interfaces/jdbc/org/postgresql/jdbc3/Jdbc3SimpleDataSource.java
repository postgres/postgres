package org.postgresql.jdbc3;

import org.postgresql.jdbc2.optional.SimpleDataSource;

import javax.naming.Reference;

/**
 * JDBC3 implementation of a non-pooling DataSource.
 *
 * @author Aaron Mulder (ammulder@alumni.princeton.edu)
 * @version $Revision: 1.1 $
 */
public class Jdbc3SimpleDataSource extends SimpleDataSource
{
    /**
     * Generates a JDBC3 object factory reference.
     */
    protected Reference createReference()
    {
        return new Reference(getClass().getName(), Jdbc3ObjectFactory.class.getName(), null);
    }

    /**
     * Gets a description of this DataSource.
     */
    public String getDescription()
    {
        return "JDBC3 Non-Pooling DataSource from " + org.postgresql.Driver.getVersion();
    }
}
