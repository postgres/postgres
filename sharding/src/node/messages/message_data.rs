
use crate::{node::{messages::node_info::NodeInfo, tables_id_info::TablesIdInfo}, utils::common::ConvertToString};

/// Enum used to represent the data returned by `get_data`
#[derive(Debug, Clone)]
pub struct MessageData {
    pub payload: Option<f64>,
    pub node_info: Option<NodeInfo>,
    pub query: Option<String>,
    pub max_ids: Option<TablesIdInfo>,
}

impl MessageData {
    // - Constructors -

    pub fn new_payload(payload: f64, max_ids: TablesIdInfo) -> Self {
        MessageData {
            payload: Some(payload),
            node_info: None,
            query: None,
            max_ids: Some(max_ids),
        }
    }

    pub fn new_node_info(node_info: NodeInfo) -> Self {
        MessageData {
            payload: None,
            node_info: Some(node_info),
            query: None,
            max_ids: None,
        }
    }

    pub fn new_query(query: String, sender_info: Option<NodeInfo>) -> Self {
        MessageData {
            payload: None,
            node_info: sender_info,
            query: Some(query),
            max_ids: None,
        }
    }

    pub fn new_query_response(query_response: String) -> Self {
        MessageData {
            payload: None,
            node_info: None,
            query: Some(query_response),
            max_ids: None,
        }
    }

    pub fn new_none() -> Self {
        MessageData {
            payload: None,
            node_info: None,
            query: None,
            max_ids: None,
        }
    }

    /// Returns the attributes as a string
    pub fn to_string(&self) -> String {
        let mut attributes_to_string = String::new();

        if let Some(payload) = self.payload {
            attributes_to_string.push_str(&payload.to_string());
            attributes_to_string.push(' ');
        }

        if let Some(node_info) = self.node_info.clone() {
            attributes_to_string.push_str(&node_info.to_string());
            attributes_to_string.push(' ');
        }

        if let Some(query) = self.query.clone() {
            attributes_to_string.push_str(&query);
            attributes_to_string.push(' ');
        }

        if let Some(max_ids) = self.max_ids.clone() {
            attributes_to_string.push_str(&max_ids.convert_to_string());
        }

        // if the last character is a space, remove it
        if attributes_to_string.ends_with(' ') {
            attributes_to_string.pop();
        }
        attributes_to_string
    }
}

impl PartialEq for MessageData {
    fn eq(&self, other: &Self) -> bool {
        self.payload == other.payload
            && self.node_info == other.node_info
            && self.query == other.query
            && self.max_ids == other.max_ids
    }
}

#[cfg(test)]
mod tests {
    use indexmap::IndexMap;

    use super::*;

    #[test]
    fn test_message_data_payload() {
        let mut max_ids = IndexMap::new();
        max_ids.insert("employees".to_string(), 3);
        max_ids.insert("departments".to_string(), 5);
        let message_data = MessageData::new_payload(1.0, max_ids);
        assert_eq!(message_data.to_string(), "1 employees:3,departments:5");
    }

    #[test]
    fn test_message_data_node_info() {
        let node_info = NodeInfo {
            ip: "1".to_string(),
            port: "2".to_string(),
        };

        let message_data = MessageData::new_node_info(node_info.clone());
        assert_eq!(message_data.to_string(), node_info.to_string());
    }

    #[test]
    fn test_message_data_query() {
        let message_data = MessageData::new_query(
            "SELECT * FROM table;".to_string(),
            Some(NodeInfo {
                ip: "1".to_string(),
                port: "2".to_string(),
            }),
        );
        assert_eq!(
            message_data.node_info,
            Some(NodeInfo {
                ip: "1".to_string(),
                port: "2".to_string()
            })
        );
        assert_eq!(message_data.query, Some("SELECT * FROM table;".to_string()));
    }

    #[test]
    fn test_message_data_none() {
        let message_data = MessageData::new_none();
        assert_eq!(message_data.to_string(), "");
    }
}
