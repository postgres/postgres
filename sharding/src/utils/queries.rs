pub fn query_is_insert(query: &str) -> bool {
    query.to_uppercase().starts_with("INSERT")
}
