package utils;

/**
 * This little app checks to see what version of JVM is being used.
 * It does this by checking first the java.vm.version property, and
 * if that fails, it looks for certain classes that should be present.
 */
public class CheckVersion
{
    /**
     * Check for the existence of a class by attempting to load it
     */
    public static boolean checkClass(String c) {
	try {
	    Class.forName(c);
	} catch(Exception e) {
	    return false;
	}
	return true;
    }
    
    /**
     * This first checks java.vm.version for 1.1, 1.2 or 1.3.
     *
     * It writes jdbc1 to stdout for the 1.1.x VM.
     *
     * For 1.2 or 1.3, it checks for the existence of the javax.sql.DataSource
     * interface, and if found writes enterprise to stdout. If the interface
     * is not found, it writes jdbc2 to stdout.
     *
     * PS: It also looks for the existence of java.lang.Byte which appeared in
     * JDK1.1.0 incase java.vm.version is not heeded by some JVM's.
     *
     * If it can't work it out, it writes huho to stdout.
     *
     * The make file uses the written results to determine which rule to run.
     *
     * Bugs: This needs thorough testing.
     */
    public static void main(String args[])
    {
	String vmversion = System.getProperty("java.vm.version");
	
	// We are running a 1.1 JVM
	if(vmversion.startsWith("1.1")) {
	    System.out.println("jdbc1");
	    System.exit(0);
	}
	
	// We are running a 1.2 or 1.3 JVM
	if(vmversion.startsWith("1.2") ||
	   vmversion.startsWith("1.3") ||
	   checkClass("java.lang.Byte")
	   ) {
	    
	    // Check to see if we have the standard extensions. If so, then
	    // we want the enterprise edition, otherwise the jdbc2 driver.
	    if(checkClass("javax.sql.DataSource"))
		System.out.println("enterprise");
	    else
		System.out.println("jdbc2");
	    System.exit(0);
	}
	
	System.out.println("huho");
	System.exit(0);
    }
}
