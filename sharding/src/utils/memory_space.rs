use sysinfo::System;
use std::ffi::CString;
use libc::statvfs;

#[no_mangle]
pub extern "C" fn print_sysinfo() {
    println!("\n ** PRINTING SYSTEM INFO **\n");

    // Create a System object
    let mut sys = System::new_all();

    // Refresh system data
    sys.refresh_all();

    // Get the root directory information
    let path = CString::new("/").unwrap();
    let mut stat: statvfs = unsafe { std::mem::zeroed() };

    if unsafe { statvfs(path.as_ptr(), &mut stat) } == 0 {
        let total_space = ((stat.f_blocks as u64) * stat.f_frsize) / 1024;
        let available_space = ((stat.f_bavail as u64) * stat.f_frsize) / 1024;

        println!("Total space: {} KB", total_space);
        println!("Available space: {} KB", available_space);
    } else {
        println!("Failed to get disk space information.");
    }
}