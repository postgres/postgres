package uk.org.retep.dtu;

public class DConstants
{
  /**
   * A global version number
   */
  public static final String XML_VERSION_ID = "V7.1-2001-02-26";

  /**
   * XML Tag names
   */
  public static final String XML_DISPLAYNAME= "DISPLAYNAME";
  public static final String XML_FROM       = "FROM";
  public static final String XML_ID         = "ID";
  public static final String XML_MODULE     = "MODULE";
  public static final String XML_NODE       = "NODE";
  public static final String XML_TO         = "TO";
  public static final String XML_TRANSFORM  = "TRANSFORM";
  public static final String XML_TYPE       = "TYPE";
  public static final String XML_VERSION    = "VERSION";
  public static final String XML_X          = "X";
  public static final String XML_Y          = "Y";

  public static final int NOP       = 0;      // No operation or always run transform
  public static final int SUCCESS   = 1;      // Run transform only if DNode.OK
  public static final int ERROR     = 2;      // Run transform only if DNode.ERROR

  /**
   * Node types 20-39 reserved for Transformation types
   */
  public static final int TRANSFORMBASE = 20;

  /**
   * Node types 20-99 reserved for Internal Node implementations
   */
  public static final int INTERNALBASE = 50;

  /**
   * Node types 100+ are for user extensions
   */
  public static final int USERBASE  = 100;
}