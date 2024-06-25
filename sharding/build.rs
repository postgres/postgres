extern crate cc;

fn main() {
    cc::Build::new()
        // .compiler("clang")
        .file("../src/bin/psql/common.c")
        .include("../src/include")
        .include("../src/interfaces/libpq")
        .compile("libsharding.a");
}
