// Minimal self-contained unit-test harness (no external dependency).
//   GF_TEST(name) { GF_CHECK(cond); GF_REQUIRE(cond); GF_CHECK_EQ(a, b); }
//   int main(int argc, char** argv) { return gf::run_all(argc, argv); }
#pragma once
#include <cstdio>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace gf {
struct TestCase { const char* name; std::function<void()> fn; };
inline std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
inline int& failures() { static int f = 0; return f; }
struct Registrar { Registrar(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };
struct RequireFailed {};
inline void report(const char* file, int line, const std::string& what) {
  std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, what.c_str());
  ++failures();
}
inline int run_all(int argc, char** argv) {
  int nfail_tests = 0, nrun = 0;
  for (auto& t : registry()) {
    if (argc > 1 && std::strstr(t.name, argv[1]) == nullptr) continue;
    const int before = failures();
    std::fprintf(stderr, "[ RUN  ] %s\n", t.name);
    try { t.fn(); } catch (RequireFailed&) {
    } catch (std::exception& e) { report(__FILE__, __LINE__, std::string("exception: ") + e.what()); }
    ++nrun;
    const bool ok = failures() == before;
    if (!ok) ++nfail_tests;
    std::fprintf(stderr, "[ %s ] %s\n", ok ? " OK " : "FAIL", t.name);
  }
  std::fprintf(stderr, "%d test(s), %d failed\n", nrun, nfail_tests);
  return (nfail_tests == 0 && nrun > 0) ? 0 : 1;
}
}  // namespace gf

#define GF_CAT2(a, b) a##b
#define GF_CAT(a, b) GF_CAT2(a, b)
#define GF_TEST(name) \
  static void GF_CAT(gf_test_, name)(); \
  static gf::Registrar GF_CAT(gf_reg_, name)(#name, &GF_CAT(gf_test_, name)); \
  static void GF_CAT(gf_test_, name)()
#define GF_CHECK(c) do { if (!(c)) gf::report(__FILE__, __LINE__, #c); } while (0)
#define GF_REQUIRE(c) do { if (!(c)) { gf::report(__FILE__, __LINE__, "REQUIRE " #c); throw gf::RequireFailed{}; } } while (0)
#define GF_CHECK_EQ(a, b) do { auto _va = (a); auto _vb = (b); if (!(_va == _vb)) { std::ostringstream _os; \
  _os << #a " == " #b " (" << _va << " vs " << _vb << ")"; gf::report(__FILE__, __LINE__, _os.str()); } } while (0)
