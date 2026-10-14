// Minimal self-contained test harness for the GEM codec track (no external framework).
#pragma once
#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace gftest {
struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& registry() { static std::vector<Case> r; return r; }
struct Reg { Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };
struct Failure : std::exception {
  std::string msg;
  explicit Failure(std::string m) : msg(std::move(m)) {}
  const char* what() const noexcept override { return msg.c_str(); }
};
inline int g_checks = 0;
inline int run_all(int argc, char** argv) {
  int nfail = 0, nrun = 0;
  for (auto& c : registry()) {
    if (argc > 1 && std::string(argv[1]) != c.name) continue;
    ++nrun;
    try { c.fn(); std::printf("[ PASS ] %s\n", c.name); }
    catch (const std::exception& e) { ++nfail; std::printf("[ FAIL ] %s: %s\n", c.name, e.what()); }
  }
  std::printf("%d/%d test cases passed, %d checks\n", nrun - nfail, nrun, g_checks);
  return nfail == 0 && nrun > 0 ? 0 : 1;
}
}  // namespace gftest

#define GF_CAT2(a, b) a##b
#define GF_CAT(a, b) GF_CAT2(a, b)
#define TEST_CASE(name) static void name(); static gftest::Reg GF_CAT(reg_, name)(#name, name); static void name()
#define CHECK(cond) do { ++gftest::g_checks; if (!(cond)) throw gftest::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": CHECK(" #cond ") failed"); } while (0)
#define CHECK_EQ(a, b) do { ++gftest::g_checks; auto _va = (a); auto _vb = (b); if (!(_va == _vb)) throw gftest::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": CHECK_EQ(" #a ", " #b ") failed: " + std::to_string(_va) + " vs " + std::to_string(_vb)); } while (0)
#define CHECK_THROWS_CODE(expr, ExcType, codeval) do { ++gftest::g_checks; bool _thrown = false; \
  try { (void)(expr); } catch (const ExcType& _e) { _thrown = true; if (_e.code() != (codeval)) throw gftest::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": wrong error code: " + _e.what()); } \
  if (!_thrown) throw gftest::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": expected " #ExcType " from " #expr); } while (0)
#define TEST_MAIN() int main(int argc, char** argv) { return gftest::run_all(argc, argv); }
