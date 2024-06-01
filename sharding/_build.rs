use std::env;
use std::path::PathBuf;

fn main() {
    // Tell cargo to look for shared libraries in the specified directory
    // Adjust the path according to where your PostgreSQL libraries are located
    println!("cargo:rustc-link-search=native=../src/interfaces/libpq");
    println!("cargo:rustc-link-lib=pq"); // Link to the libpq library

    // Define the path to the header file relative to the current directory
    let header_path = "../src/interfaces/libpq/libpq-fe.h";

    // The bindgen::Builder is the main entry point to bindgen, and lets you build up options for the resulting bindings.
    let bindings = bindgen::Builder::default()
        .header(header_path)
        .clang_arg("-I../src/include")
        .clang_arg("-I../src/include/common")   // Include path for common headers
        .clang_arg("-I../src/include/port")     // Include path for platform-specific headers
        .clang_arg("-include")
        .clang_arg("c.h")                       // Include c.h for core PostgreSQL definitions
        .clang_arg("-include")
        .clang_arg("stdbool.h")                 // Include stdbool.h for bool type
        .clang_arg("-include")
        .clang_arg("stdint.h")                  // Include stdint.h for standard integer types
        .clang_arg("-Dbool=_Bool")              // Define bool if not already defined
        .clang_arg("-Duint32=uint32_t")         // Define uint32 as uint32_t
        .clang_arg("-Duint64=uint64_t")         // Define uint64 as uint64_t
        .clang_arg("-Dint64=int64_t")           // Define int64 as int64_t
        .generate()
        .expect("Unable to generate bindings");

    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Couldn't write bindings!");
}
