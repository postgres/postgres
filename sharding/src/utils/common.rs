use users::get_current_username;

pub fn get_username_dinamically() -> String {
    match get_current_username() {
        Some(username) => username.to_string_lossy().to_string(),
        None => panic!("Failed to get current username"),
    }
}
