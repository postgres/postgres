package uk.org.retep.util;

import uk.org.retep.util.Logger;

import java.util.ArrayList;
import java.util.Hashtable;
import java.util.Iterator;
import java.util.Properties;

/**
 * This is a Singleton that stores global properties, command line arguments
 * etc.
 *
 * All tools are guranteed that this will exist.
 *
 * @author
 * @version 1.0
 */

public class Globals
{
  private static final Globals SINGLETON = new Globals();

  private Hashtable   global= new Hashtable();
  private Properties  props = new Properties();
  private ArrayList   args  = new ArrayList();

  private Globals()
  {
  }

  public static Globals getInstance()
  {
    return SINGLETON;
  }

  /**
   * Retrieves an object from the global pool
   * @param aKey key of the object
   * @return The object, null if not found
   */
  public Object get(Object aKey)
  {
    return global.get(aKey);
  }

  /**
   * Stores an object into the global pool
   * @param aKey key of the object
   * @param aObj the object to store
   * @return aObj
   */
  public Object put(Object aKey,Object aObj)
  {
    return global.put(aKey,aObj);
  }

  /**
   * Returns a Properties object of all properties
   */
  /*
  public Properties getProperties()
  {
    return props;
  }
  */

  /**
   * @param aProp a property supplied to the command line
   * @return property or NULL if not present
   */
  public String getProperty(String aProp)
  {
    return props.getProperty(aProp);
  }

  /**
   * @param aProp a property supplied to the command line
   * @param aDefault default to return if property was not supplied
   * @return property value
   */
  public String getProperty(String aProp,String aDefault)
  {
    return props.getProperty(aProp,aDefault);
  }

  /**
   * @param aID ID of the argument, 0 ... getArgumentCount()-1
   * @return argument
   */
  public String getArgument(int aID)
  {
    return (String) args.get(aID);
  }

  /**
   * Returns an array of String objects representing the arguments
   */
  public String[] getArguments()
  {
    return (String[]) args.toArray();
  }

  /**
   * Returns an Iterator of the arguments
   */
  public Iterator getArgumentIterator()
  {
    return args.iterator();
  }

  /**
   * @return number of arguments
   */
  public int getArgumentCount()
  {
    return args.size();
  }

  /**
   * Parses the command line arguments
   */
  public void parseArguments(String[] aArgs)
  throws Exception
  {
    for(int i=0;i<aArgs.length;i++) {
      String arg = aArgs[i];
      if(arg.startsWith("--") || arg.startsWith("-")) {
        if(arg.length()>1) {
          // Split the option at the first '=' char if any
          int s = arg.startsWith("--") ? 2 : 1 ;  // -- or -
          int e = arg.indexOf("=");
          String key,val;
          if(e>s) {
            // Format: -key=value
            key=arg.substring(s,e-1);
            val=arg.substring(e+1);
          } else if(e>-1 && e<=s) {
            // Can't have a property without a key!
            throw new Exception("Invalid option -=");
          } else {
            key=arg.substring(s);
            val=""; // can't be null
          }

          if(key.equals("d")) {
            // -d | --d is reserved to set the Logger level
            int level=0;
            if(!val.equals("")) {
              level=Integer.parseInt(val);
            }
            Logger.setLevel(level);
          } else {
            // Add all other properties into the Properties object
            props.put(key,val);
            Logger.log(Logger.INFO,"Argument",key,val);
          }

        } else {
          // Just a - on its own?
          System.out.println("Unknown option: -");
        }
      } else {
        // Add the argument to the array
        args.add(arg);
      }
    }
  }

}