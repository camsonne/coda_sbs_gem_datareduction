// gemfilter / geom : Podd database reader (see PoddDb.h for the semantics ported).
#include "gemfilter/geom/PoddDb.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace gemfilter {
namespace geom {

// ---------------------------------------------------------------- time helpers
namespace {
// Howard Hinnant's days_from_civil
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}
void civilFromDays(int64_t z, int& y, int& m, int& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int>(yy + (m <= 2));
}

// strptime-like numeric field: optional leading white space, then 1..maxdig digits.
bool readNum(const char*& p, int maxdig, long& out) {
  while (*p == ' ' || *p == '\t') ++p;
  if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
  long v = 0;
  int n = 0;
  while (n < maxdig && std::isdigit(static_cast<unsigned char>(*p))) {
    v = v * 10 + (*p - '0');
    ++p;
    ++n;
  }
  out = v;
  return true;
}
void skipWs(const char*& p) {
  while (*p && std::isspace(static_cast<unsigned char>(*p))) ++p;
}

// Parse " %Y-%m-%d %H:%M:%S [%z] " (whole string must be consumed).
std::optional<DbTime> parseStamp(const std::string& ts, bool require_time) {
  const char* p = ts.c_str();
  long Y, M, D, h = 0, mi = 0, s = 0;
  skipWs(p);
  if (!readNum(p, 4, Y) || *p++ != '-') return std::nullopt;
  if (!readNum(p, 2, M) || *p++ != '-') return std::nullopt;
  if (!readNum(p, 2, D)) return std::nullopt;
  if (M < 1 || M > 12 || D < 1 || D > 31) return std::nullopt;
  skipWs(p);
  if (*p == 'T') ++p;
  if (*p) {
    if (!readNum(p, 2, h)) return std::nullopt;
    if (*p++ != ':') return std::nullopt;
    if (!readNum(p, 2, mi)) return std::nullopt;
    if (*p == ':') {
      ++p;
      if (!readNum(p, 2, s)) return std::nullopt;
    } else if (require_time) {
      return std::nullopt;
    }
    if (h > 23 || mi > 59 || s > 61) return std::nullopt;
    skipWs(p);
    // optional numeric zone (%z). Parsed for syntax only; see header note.
    if (*p == '+' || *p == '-') {
      ++p;
      int nd = 0;
      while (std::isdigit(static_cast<unsigned char>(*p)) || *p == ':') {
        if (*p != ':') ++nd;
        ++p;
      }
      if (nd != 2 && nd != 4) return std::nullopt;
      skipWs(p);
    } else if (*p == 'Z') {
      ++p;
      skipWs(p);
    }
  } else if (require_time) {
    return std::nullopt;
  }
  if (*p) return std::nullopt;
  if (Y < 1995) return std::nullopt;  // Podd: tm_year < 95 is invalid
  return civilSeconds(static_cast<int>(Y), static_cast<int>(M), static_cast<int>(D),
                      static_cast<int>(h), static_cast<int>(mi), static_cast<int>(s));
}

std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}
}  // namespace

DbTime civilSeconds(int year, int month, int day, int hour, int min, int sec) {
  return daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 +
         hour * 3600 + min * 60 + sec;
}

std::optional<DbTime> parseDateTime(const std::string& s) {
  std::string t = s;
  std::replace(t.begin(), t.end(), 'T', ' ');
  return parseStamp(t, false);
}

std::string formatDateTime(DbTime t) {
  int64_t days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
  int64_t rem = t - days * 86400;
  int y, m, d;
  civilFromDays(days, y, m, d);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", y, m, d,
                static_cast<int>(rem / 3600), static_cast<int>((rem % 3600) / 60),
                static_cast<int>(rem % 60));
  return buf;
}

std::optional<DbTime> parseDbDateTag(const std::string& line) {
  // Mirrors Podd IsDBdate bracket checks.
  auto lbrk = line.find('[');
  if (lbrk == std::string::npos || line.size() < 12 || lbrk >= line.size() - 12) return std::nullopt;
  auto rbrk = line.find(']', lbrk);
  if (rbrk == std::string::npos || rbrk <= lbrk + 11) return std::nullopt;
  return parseStamp(line.substr(lbrk + 1, rbrk - lbrk - 1), true);
}

// ---------------------------------------------------------------- line reading
namespace {

// Podd IsAssignment
bool isAssignment(const std::string& str) {
  auto pos = str.find('=');
  if (pos == std::string::npos) return false;
  size_t c = 0;
  while (c < str.size() && std::isspace(static_cast<unsigned char>(str[c]))) ++c;
  if (c == pos) return false;  // only whitespace before '=' or '=' first
  return str[pos - 1] != '!' && str[pos - 1] != '<' && str[pos - 1] != '>' &&
         (pos + 1 >= str.length() || str[pos + 1] != '=');
}

// Podd prepare_line
void prepareLine(std::string& linbuf, bool& comment, bool& continued, bool& leading_space,
                 bool& trailing_space) {
  if (linbuf.empty()) return;
  auto pos = linbuf.find('#');
  if (pos == 0) {
    comment = true;
    linbuf.clear();
    return;
  }
  const auto pos2 = linbuf.find('\\');
  pos = std::min(pos, pos2);
  if (pos != std::string::npos) {
    if (pos == pos2)
      continued = true;
    else
      comment = true;
    linbuf.erase(pos);
  }
  if (!linbuf.empty()) {
    if (std::isspace(static_cast<unsigned char>(linbuf.front()))) leading_space = true;
    if (std::isspace(static_cast<unsigned char>(linbuf.back()))) trailing_space = true;
    if (leading_space || trailing_space) linbuf = trim(linbuf);
  }
}

// Podd ReadDBline over an in-memory line array. Returns false at EOF with no data.
bool readDBline(const std::vector<std::string>& phys, size_t& ip, std::string& line, int& first) {
  line.clear();
  first = 0;
  bool maybe_continued = false, unfinished = true, got = false;
  bool hit_eof = false;
  while (unfinished) {
    size_t oldpos = ip;
    if (ip >= phys.size()) {
      hit_eof = true;
      break;
    }
    std::string linbuf = phys[ip++];
    got = true;
    bool continued = false, comment = false, trailing_space = false, leading_space = false;
    prepareLine(linbuf, comment, continued, leading_space, trailing_space);
    if (linbuf.empty()) {
      if (line.empty() || continued || comment) {
        // nothing to do, or skip empty continuation/comment inside a block
      } else {
        break;  // blank line ends continuation
      }
    }
    bool is_assignment = isAssignment(linbuf);
    if (maybe_continued && is_assignment) {
      ip = oldpos;  // rewind: this line starts the next logical line
      break;
    }
    if (line.empty() && !continued && is_assignment) maybe_continued = true;
    unfinished = (continued || maybe_continued);
    if (maybe_continued || (trailing_space && continued)) linbuf.push_back(' ');
    if (leading_space && !line.empty() && !std::isspace(static_cast<unsigned char>(line.back())))
      line.push_back(' ');
    if (line.empty() && !linbuf.empty()) first = static_cast<int>(oldpos) + 1;
    line.append(linbuf);
  }
  if (maybe_continued) {
    if (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
  }
  if (hit_eof && !got && line.empty()) return false;
  if (hit_eof && line.empty()) return false;
  return true;
}

// IsDBkey pre-analysis (key-independent part)
void analyseKey(const std::string& ln, int& status, std::string& key, std::string& value) {
  status = 0;
  auto eq = ln.find('=');
  if (eq == std::string::npos) return;
  if ((eq > 0 && (ln[eq - 1] == '!' || ln[eq - 1] == '<' || ln[eq - 1] == '>')) ||
      (eq + 1 < ln.size() && ln[eq + 1] == '='))
    return;
  status = 1;
  key = trim(ln.substr(0, eq));
  value = trim(ln.substr(eq + 1));
}

}  // namespace

// ---------------------------------------------------------------- DbFile
void DbFile::parse(const std::vector<std::string>& physical) {
  lines_.clear();
  size_t ip = 0;
  std::string line;
  int first = 0;
  while (readDBline(physical, ip, line, first)) {
    if (line.empty()) continue;
    Logical l;
    l.text = line;
    l.line = first;
    analyseKey(line, l.status, l.key, l.value);
    lines_.push_back(std::move(l));
  }
}

DbFile DbFile::load(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw DbError("cannot open database file " + path);
  std::vector<std::string> phys;
  std::string s;
  while (std::getline(in, s)) {
    std::replace(s.begin(), s.end(), '\t', ' ');  // Podd GetLine converts tabs
    phys.push_back(s);
  }
  DbFile f;
  f.path_ = path;
  f.parse(phys);
  return f;
}

DbFile DbFile::fromString(const std::string& text, const std::string& name) {
  std::vector<std::string> phys;
  std::istringstream in(text);
  std::string s;
  while (std::getline(in, s)) {
    std::replace(s.begin(), s.end(), '\t', ' ');
    phys.push_back(s);
  }
  DbFile f;
  f.path_ = name;
  f.parse(phys);
  return f;
}

std::optional<DbValue> DbFile::get(const std::string& key, DbTime date) const {
  // Port of Podd LoadDBvalue control flow.
  DbTime keydate = 0, prevdate = 0;
  bool found = false, do_ignore = false;
  DbValue out;
  for (const auto& l : lines_) {
    int status = 0;
    if (!do_ignore) status = (l.status == 0) ? 0 : ((l.key == key) ? 1 : -1);
    if (!do_ignore && status != 0) {
      if (status > 0) {
        found = true;
        prevdate = keydate;
        out.key = key;
        out.text = l.value;
        out.block_time = keydate;
        out.line = l.line;
      }
    } else if (auto d = parseDbDateTag(l.text)) {
      keydate = *d;
      do_ignore = (keydate > date || keydate < prevdate);
    }
  }
  if (!found) return std::nullopt;
  out.file = path_;
  return out;
}

namespace {
// Podd ChopPrefix: "L.vdc." -> "L."; returns remaining number of dots.
int chopPrefix(std::string& s) {
  auto len = s.size();
  if (len >= 2) {
    auto pos = s.rfind('.', len - 2);
    if (pos != std::string::npos) {
      s.erase(pos + 1);
      return static_cast<int>(std::count(s.begin(), s.end(), '.'));
    }
  }
  s.clear();
  return 0;
}
}  // namespace

std::optional<DbValue> DbFile::find(const std::string& prefix, const std::string& name, DbTime date,
                                    int search) const {
  if (auto v = get(prefix + name, date)) return v;
  if (search != 0 && !prefix.empty()) {
    std::string newprefix = prefix;
    int newlevel = chopPrefix(newprefix) + 1;
    if (search < 0 || newlevel >= search) {
      int newsearch = search < 0 ? search + 1 : search;
      return find(newprefix, name, date, newsearch);
    }
  }
  return std::nullopt;
}

std::vector<DbValue> DbFile::history(const std::string& key) const {
  std::vector<DbValue> out;
  DbTime keydate = 0;
  for (const auto& l : lines_) {
    if (l.status != 0) {
      if (l.key == key) out.push_back(DbValue{key, l.value, keydate, l.line, path_});
    } else if (auto d = parseDbDateTag(l.text)) {
      keydate = *d;
    }
  }
  return out;
}

std::vector<DbTime> DbFile::timestamps() const {
  std::set<DbTime> s;
  for (const auto& l : lines_)
    if (l.status == 0)
      if (auto d = parseDbDateTag(l.text)) s.insert(*d);
  return {s.begin(), s.end()};
}

std::vector<std::string> DbFile::keys() const {
  std::set<std::string> s;
  for (const auto& l : lines_)
    if (l.status != 0 && !l.key.empty()) s.insert(l.key);
  return {s.begin(), s.end()};
}

double DbFile::toDouble(const DbValue& v) {
  const char* p = v.text.c_str();
  char* end = nullptr;
  errno = 0;
  double d = std::strtod(p, &end);
  if (p == end || (end && *end && !std::isspace(static_cast<unsigned char>(*end))) || errno != 0)
    throw DbError("conversion error: " + v.key + " = \"" + v.text + "\" (" + v.file + ")");
  return d;
}

long long DbFile::toInt(const DbValue& v) {
  const char* p = v.text.c_str();
  char* end = nullptr;
  errno = 0;
  long long d = std::strtoll(p, &end, 10);
  if (p == end || (end && *end && !std::isspace(static_cast<unsigned char>(*end))) || errno != 0)
    throw DbError("conversion error: " + v.key + " = \"" + v.text + "\" (" + v.file + ")");
  return d;
}

std::vector<double> DbFile::toDoubles(const DbValue& v) {
  std::vector<double> out;
  const char* p = v.text.c_str();
  const char* endp = p + v.text.size();
  do {
    char* end = nullptr;
    errno = 0;
    double d = std::strtod(p, &end);
    if (p == end || (end && *end && !std::isspace(static_cast<unsigned char>(*end))) || errno != 0)
      throw DbError("array conversion error: " + v.key + " (" + v.file + " line " +
                    std::to_string(v.line) + ")");
    out.push_back(d);
    p = end;
  } while (p != endp);
  return out;
}

std::vector<long long> DbFile::toInts(const DbValue& v) {
  std::vector<long long> out;
  const char* p = v.text.c_str();
  const char* endp = p + v.text.size();
  do {
    char* end = nullptr;
    errno = 0;
    long long d = std::strtoll(p, &end, 10);
    if (p == end || (end && *end && !std::isspace(static_cast<unsigned char>(*end))) || errno != 0)
      throw DbError("integer array conversion error: " + v.key + " (" + v.file + " line " +
                    std::to_string(v.line) + ")");
    out.push_back(d);
    p = end;
  } while (p != endp);
  return out;
}

std::vector<std::string> DbFile::toTokens(const DbValue& v) {
  std::vector<std::string> out;
  std::istringstream in(v.text);
  std::string t;
  while (in >> t) out.push_back(t);
  return out;
}

// ---------------------------------------------------------------- DbReader
DbReader::DbReader(const DbFile& file, DbTime date, std::string prefix)
    : file_(&file), date_(date), prefix_(std::move(prefix)) {}

std::optional<DbValue> DbReader::raw(const std::string& name, int search) const {
  auto v = file_->find(prefix_, name, date_, search);
  if (v) used_.push_back(*v);
  return v;
}

DbValue DbReader::require(const std::string& name, int search) const {
  auto v = raw(name, search);
  if (!v)
    throw DbError("required key '" + prefix_ + name + "' not found in " + file_->path() +
                  " for date " + formatDateTime(date_));
  return *v;
}

double DbReader::getDouble(const std::string& name, int search) const {
  return DbFile::toDouble(require(name, search));
}
double DbReader::getDouble(const std::string& name, double def, int search) const {
  auto v = raw(name, search);
  return v ? DbFile::toDouble(*v) : def;
}
long long DbReader::getInt(const std::string& name, int search) const {
  return DbFile::toInt(require(name, search));
}
long long DbReader::getInt(const std::string& name, long long def, int search) const {
  auto v = raw(name, search);
  return v ? DbFile::toInt(*v) : def;
}
std::vector<double> DbReader::getDoubles(const std::string& name, int search, bool required) const {
  if (required) return DbFile::toDoubles(require(name, search));
  auto v = raw(name, search);
  return v ? DbFile::toDoubles(*v) : std::vector<double>{};
}
std::vector<long long> DbReader::getInts(const std::string& name, int search, bool required) const {
  if (required) return DbFile::toInts(require(name, search));
  auto v = raw(name, search);
  return v ? DbFile::toInts(*v) : std::vector<long long>{};
}
std::vector<std::string> DbReader::getTokens(const std::string& name, int search, bool required) const {
  if (required) return DbFile::toTokens(require(name, search));
  auto v = raw(name, search);
  return v ? DbFile::toTokens(*v) : std::vector<std::string>{};
}

// ---------------------------------------------------------------- DbDir
std::vector<std::string> DbDir::candidates(const std::string& name, DbTime date) const {
  std::vector<std::string> out;
  if (name.empty()) return out;
  std::string filename = name;
  if (filename.find('/') != std::string::npos) {
    out.push_back(filename);
    return out;
  }
  if (filename.rfind("db_", 0) != 0) filename = "db_" + filename;
  fs::path fname{filename};
  auto ext = fname.extension();
  if (ext == ".")
    fname.replace_extension(".dat");
  else if (ext != ".dat")
    fname += ".dat";

  fs::path dbdir{root};
  std::vector<std::string> time_dirs;
  bool have_default = false;
  std::error_code ec;
  if (fs::is_directory(dbdir, ec)) {
    for (const auto& de : fs::directory_iterator(dbdir, ec)) {
      if (!de.is_directory()) continue;
      std::string item = de.path().filename().string();
      if (item.size() == 8 && item.find_first_not_of("0123456789") == std::string::npos)
        time_dirs.push_back(item);
      else if (item == "DEFAULT")
        have_default = true;
    }
  }
  // requested date as YYYYMMDD
  int64_t days = date >= 0 ? date / 86400 : -((-date + 86399) / 86400);
  int y, m, d;
  civilFromDays(days, y, m, d);
  int req = y * 10000 + m * 100 + d;
  std::sort(time_dirs.begin(), time_dirs.end());
  std::string datedir;
  for (auto it = time_dirs.rbegin(); it != time_dirs.rend(); ++it) {
    if (req >= std::stoi(*it)) {
      datedir = *it;
      break;
    }
  }
  if (search_cwd) out.push_back(fname.string());
  if (!datedir.empty()) out.push_back((dbdir / datedir / fname).string());
  if (have_default) out.push_back((dbdir / "DEFAULT" / fname).string());
  out.push_back((dbdir / fname).string());
  return out;
}

std::string DbDir::resolve(const std::string& name, DbTime date) const {
  for (const auto& c : candidates(name, date)) {
    std::error_code ec;
    if (fs::is_regular_file(c, ec)) return c;
  }
  throw DbError("no database file for '" + name + "' under " + root);
}

}  // namespace geom
}  // namespace gemfilter
