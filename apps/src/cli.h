// Minimal command-line helper shared by gemfilter_file and gemfilter_et (private header).
#pragma once
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace gemfilter {
namespace cli {

class Args {
 public:
  Args(int argc, char** argv, std::vector<std::string> flags = {}) : flags_(std::move(flags)) {
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a.rfind("--", 0) == 0 || (a.size() == 2 && a[0] == '-')) {
        auto eq = a.find('=');
        if (eq != std::string::npos) {
          kv_[a.substr(0, eq)] = a.substr(eq + 1);
        } else if (i + 1 < argc && !is_flag_name(a) && argv[i + 1][0] != '-') {
          kv_[a] = argv[++i];
        } else {
          kv_[a] = "";
        }
      } else {
        pos_.push_back(a);
      }
    }
  }
  bool has(const std::string& a, const std::string& b = "") const {
    return kv_.count(a) || (!b.empty() && kv_.count(b));
  }
  std::string str(const std::string& a, const std::string& b, const std::string& def) const {
    auto it = kv_.find(a);
    if (it == kv_.end() && !b.empty()) it = kv_.find(b);
    return it == kv_.end() ? def : it->second;
  }
  long long num(const std::string& a, const std::string& b, long long def) const {
    const std::string s = str(a, b, "");
    if (s.empty()) return def;
    return std::stoll(s, nullptr, 0);
  }
  double real(const std::string& a, const std::string& b, double def) const {
    const std::string s = str(a, b, "");
    return s.empty() ? def : std::stod(s);
  }
  const std::vector<std::string>& positional() const { return pos_; }

 private:
  bool is_flag_name(const std::string& a) const {
    for (const auto& f : flags_)
      if (f == a) return true;
    return false;
  }
  std::vector<std::string> flags_;  // options that never take a value
  std::map<std::string, std::string> kv_;
  std::vector<std::string> pos_;
};

inline std::vector<uint16_t> parse_u16_list(const std::string& s) {
  std::vector<uint16_t> v;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, ','))
    if (!item.empty()) v.push_back(static_cast<uint16_t>(std::stoul(item, nullptr, 0)));
  return v;
}

}  // namespace cli
}  // namespace gemfilter
