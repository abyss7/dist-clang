namespace dist_clang {
namespace base {

// Bash is too old on a common MacOS and fails some tests (e.g.
// RunWithEnvironment), use zsh instead.
extern const char* const sh;
const char* const sh = "/bin/zsh";

}  // namespace base
}  // namespace dist_clang
