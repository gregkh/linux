// Copyright (c) Meta Platforms, Inc. and affiliates.

// This software may be used and distributed according to the terms of the
// GNU General Public License version 2.
extern crate bindgen;

use std::env;
use std::path::PathBuf;

use glob::glob;
use libbpf_cargo::SkeletonBuilder;

const HEADER_PATH: &str = "src/bpf/intf.h";
const SKEL_NAME: &str = "bpf";

fn bindgen_bpf_intf() {
    // Tell cargo to invalidate the built crate whenever the wrapper changes
    println!("cargo:rerun-if-changed={}", HEADER_PATH);

    // The bindgen::Builder is the main entry point
    // to bindgen, and lets you build up options for
    // the resulting bindings.
    let bindings = bindgen::Builder::default()
        // Should run clang with the same -I options as BPF compilation.
        .clang_args(env::var("BPF_CFLAGS").unwrap().split_whitespace())
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
        .write_to_file(out_path.join("bpf_intf.rs"))
        .expect("Couldn't write bindings!");
}

fn gen_bpf_skel() {
    let bpf_cflags = env::var("BPF_CFLAGS").unwrap();
    let bpf_clang = env::var("BPF_CLANG").unwrap();

    let src = format!("./src/bpf/main.bpf.c");
    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    let obj = out_path.join(format!("{}.bpf.o", SKEL_NAME));
    let skel_path = out_path.join(format!("{}_skel.rs", SKEL_NAME));

    SkeletonBuilder::new()
        .source(&src)
        .obj(&obj)
        .clang(bpf_clang)
        .clang_args(bpf_cflags)
        .build_and_generate(&skel_path)
        .unwrap();

    // Trigger rebuild if any .[hc] files are changed in the directory.
    for path in glob("src/bpf/*.[hc]").unwrap().filter_map(Result::ok) {
        println!("cargo:rerun-if-changed={}", path.to_str().unwrap());
    }
}

fn main() {
    bindgen_bpf_intf();
    gen_bpf_skel();
}
