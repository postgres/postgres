/**
 * This class is used by the makefile to determine which version of the
 * JDK is currently in use, and if it's using JDK1.1.x then it returns JDBC1
 * and if later, it returns JDBC2
 *
 * $Id: makeVersion.java,v 1.1 1999/01/17 04:51:49 momjian Exp $
 */
public class makeVersion
{
    public static void main(String[] args) {
	String key     = "java.version";
	String version = System.getProperty(key);
	
	//System.out.println(key+" = \""+version+"\"");
	
	// Tip: use print not println here as println breaks the make that
	// comes with CygWin-B20.1
	
	if(version.startsWith("1.0")) {
	    // This will trigger the unknown rule in the makefile
	    System.out.print("jdbc0");
	} else if(version.startsWith("1.1")) {
	    // This will trigger the building of the JDBC 1 driver
	    System.out.print("jdbc1");
	} else {
	    // This will trigger the building of the JDBC 2 driver
	    System.out.print("jdbc2");
	}
    }
}
