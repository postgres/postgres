package uk.org.retep.dtu;

import uk.org.retep.xml.core.XMLFactory;
import uk.org.retep.xml.core.XMLFactoryException;
import uk.org.retep.xml.parser.TagListener;
import uk.org.retep.util.Logger;

import java.io.IOException;
import java.io.Serializable;
import java.util.HashMap;
import java.util.Iterator;

/**
 * DModule represents a programatic module of steps used within the DTU
 */
public class DModule implements Serializable
{
  // The nodes and transitions between them
  protected DCollection nodes;

  protected String displayName;

  public static final String DEFAULT_DISPLAYNAME = "unnamed module";

  public DModule()
  {
    nodes=new DCollection();
    displayName=DEFAULT_DISPLAYNAME;
    Logger.log(Logger.DEBUG,"new DModule",this);
  }

  // Expensive!
  public DNode getNode(int id)
  {
    return (DNode) nodes.getElement(id);
  }

  public DNode addNode(DNode aNode)
  {
    Logger.log(Logger.DEBUG,"DModule.addNode",aNode);
    nodes.add(aNode);
    return aNode;
  }

  public void removeNode(DNode aNode)
  {
    Logger.log(Logger.DEBUG,"DModule.removeNode",aNode);
    nodes.remove(aNode);
  }

  public void clear()
  {
    Logger.log(Logger.DEBUG,"DModule.clear",this);
    nodes.clear();
  }

  public void setDisplayName(String aName)
  {
    Logger.log(Logger.DEBUG,"DModule.setDisplayName",aName);
    displayName = aName;
  }

  public String getDisplayName()
  {
    return displayName;
  }

  public Iterator iterator()
  {
    return nodes.iterator();
  }

  /**
   * Writes an XML representation of this module to an XMLFactory. The caller
   * must close the factory after use!
   */
  public synchronized void saveXML(XMLFactory aFactory)
  throws IOException, XMLFactoryException
  {
    Logger.log(Logger.DEBUG,"DModule.saveXML start",this);
    Iterator it;

    aFactory.startTag(DConstants.XML_MODULE);
    aFactory.addAttribute(DConstants.XML_DISPLAYNAME,displayName);
    aFactory.addAttribute(DConstants.XML_VERSION,DConstants.XML_VERSION_ID);

    // The nodes
    nodes.saveXML(aFactory);

    // The transforms
    //trans.saveXML(aFactory);

    aFactory.endTag(); // MODULE
    Logger.log(Logger.DEBUG,"DModule.saveXML end",this);
  }

}