// Copyright (c) Meta Platforms, Inc. and affiliates.

// This software may be used and distributed according to the terms of the
// GNU General Public License version 2.
extern crate bindgen;

use std::env;
use std::fs::create_dir_all;
use std::path::Path;
use std::path::PathBuf;

use glob::glob;
use libbpf_cargo::SkeletonBuilder;

const HEADER_PATH: &str = "src/bpf/layered.h";

fn bindgen_layered() {
    // Tell cargo to invalidate the built crate whenever the wrapper changes
    println!("cargo:rerun-if-changed={}", HEADER_PATH);

    // The bindgen::Builder is the main entry point
    // to bindgen, and lets you build up options for
    // the resulting bindings.
    let bindings = bindgen::Builder::default()
        // The input header we would like to generate
        // bindings for.
        .header(HEADER_PATH)
        // Tell cargo to invalidate the built crate whenever any of the
        // included header files changed.
        .parse_callbacks(Box::new(bindgen::CargoCallbacks))
        // Finish the builder and generate the bindings.
        .generate()
        // Unwrap the Result and panic on failure.
        .expect("Unable to generate bindings");

    // Write the bindings to the $OUT_DIR/bindings.rs file.
    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("layered_sys.rs"))
        .expect("Couldn't write bindings!");
}

fn gen_bpf_sched(name: &str) {
    let bpf_cflags = env::var("SCX_RUST_BPF_CFLAGS").unwrap();
    let clang = env::var("SCX_RUST_CLANG").unwrap();
    eprintln!("{}", clang);
    let outpath = format!("./src/bpf/.output/{}.skel.rs", name);
    let skel = Path::new(&outpath);
    let src = format!("./src/bpf/{}.bpf.c", name);
    let obj = format!("./src/bpf/.output/{}.bpf.o", name);
    SkeletonBuilder::new()
        .source(src.clone())
	.obj(obj)
        .clang(clang)
        .clang_args(bpf_cflags)
        .build_and_generate(skel)
        .unwrap();

    // Trigger rebuild if any .[hc] files are changed in the directory.
    for path in glob("./src/bpf/*.[hc]").unwrap().filter_map(Result::ok) {
        println!("cargo:rerun-if-changed={}", path.to_str().unwrap());
    }
}

fn main() {
    bindgen_layered();
    // It's unfortunate we cannot use `OUT_DIR` to store the generated skeleton.
    // Reasons are because the generated skeleton contains compiler attributes
    // that cannot be `include!()`ed via macro. And we cannot use the `#[path = "..."]`
    // trick either because you cannot yet `concat!(env!("OUT_DIR"), "/skel.rs")` inside
    // the path attribute either (see https://github.com/rust-lang/rust/pull/83366).
    //
    // However, there is hope! When the above feature stabilizes we can clean this
    // all up.
    create_dir_all("./src/bpf/.output").unwrap();
    gen_bpf_sched("layered");
}
