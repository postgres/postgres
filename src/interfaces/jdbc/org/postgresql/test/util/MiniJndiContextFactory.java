package org.postgresql.test.util;

import java.util.*;
import javax.naming.*;
import javax.naming.spi.InitialContextFactory;

/**
 * The ICF for a trivial JNDI implementation.  This is not meant to
 * be very useful, beyond testing JNDI features of the connection
 * pools.
 *
 * @author Aaron Mulder (ammulder@chariotsolutions.com)
 * @version $Revision: 1.1 $
 */
public class MiniJndiContextFactory implements InitialContextFactory
{
    public Context getInitialContext(Hashtable environment)
            throws NamingException
    {
        return new MiniJndiContext();
    }
}
