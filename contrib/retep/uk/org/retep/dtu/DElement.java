package uk.org.retep.dtu;

import uk.org.retep.xml.core.XMLFactory;
import uk.org.retep.xml.core.XMLFactoryException;

import java.io.IOException;

public interface DElement
{
  /**
   * Fetch the unique ID of this Element
   */
  public int getID();

  /**
   * Sets the unique id - normally set by DCollection
   */
  public void setID(int id);

  /**
   * @return the type of the Element
   */
  public int getType();

  /**
   * Set's the element type
   */
  public void setType(int aType);

  public void saveXML(XMLFactory aFactory) throws IOException, XMLFactoryException;
}