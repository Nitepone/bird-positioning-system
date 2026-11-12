//! Records `git describe` as the build's version (see scripts/git_describe.rs).

include!("../../scripts/git_describe.rs");

fn main() {
    emit_git_describe();
}
