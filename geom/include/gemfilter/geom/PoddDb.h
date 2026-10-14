// gemfilter / geom : lightweight reader for Podd (Hall A analyzer) text databases.
//
// This is a faithful C++17 port of the parts of Podd's Database/Database.cxx that
// SBS-offline relies on (analyzer master, 2025):
//   * ReadDBline   : '#' comments, '\' continuation, "key =" followed by un-keyed
//                    lines up to the next blank line or next assignment (multi-line arrays)
//   * IsDBdate     : time-stamp tags "[ yyyy-mm-dd hh:mm:ss ]" anywhere on a line
//   * LoadDBvalue  : value of the LAST assignment of 'key' in the most recent time-stamp
//                    block not later than the requested date ("ignore" rule
//                    keydate > date || keydate < prevdate, exactly as in Podd)
//   * LoadDBarray  : whitespace-separated numeric arrays (strtod/strtoll semantics)
//   * LoadDatabase : search up the name tree ("bb.gem.m0.upitch" -> "bb.gem.upitch" -> ...)
//   * GetDBFileList: file resolution DB/<YYYYMMDD>/db_<name>.dat (latest dir <= run date),
//                    DB/DEFAULT/, DB/
// Not ported: text-variable substitution (gHaTextvars; not used by SBS-replay DB files),
// LoadDBmatrix, the "./db_<name>.dat in current directory" first candidate (off by
// default, can be enabled in DbDir).
//
// Time handling: Podd interprets DB time stamps and the run date in the same local
// time zone; we compare naive civil date-times (seconds since 1970-01-01 00:00:00 of the
// proleptic Gregorian calendar, no zone), which preserves ordering (DST edges aside).
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace gemfilter {
namespace geom {

using DbTime = int64_t;  // naive civil seconds, see above

DbTime civilSeconds(int year, int month, int day, int hour = 0, int min = 0, int sec = 0);
// Parse "YYYY-MM-DD", "YYYY-MM-DD HH:MM", "YYYY-MM-DD HH:MM:SS" (also 'T' separator).
std::optional<DbTime> parseDateTime(const std::string& s);
std::string formatDateTime(DbTime t);
// Mirrors Podd IsDBdate: text between the first '[' and the next ']' must be
// " %Y-%m-%d %H:%M:%S [%z] " (whitespace tolerant); year >= 1995.
std::optional<DbTime> parseDbDateTag(const std::string& line);
// A date "far in the future": selects the latest value of every key.
constexpr DbTime kLatest = INT64_C(4102444800);  // 2100-01-01

class DbError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// One resolved database value with provenance.
struct DbValue {
  std::string key;          // fully qualified key that matched
  std::string text;         // value text (comments stripped, continuation lines joined)
  DbTime block_time = 0;    // time stamp of the block that supplied the value (0 = none)
  int line = 0;             // 1-based physical line number of the assignment
  std::string file;         // file path / name
};

class DbFile {
 public:
  DbFile() = default;
  static DbFile load(const std::string& path);
  static DbFile fromString(const std::string& text, const std::string& name = "<string>");

  const std::string& path() const { return path_; }
  bool empty() const { return lines_.empty(); }

  // Podd LoadDBvalue: value valid at 'date' (std::nullopt if key not found).
  std::optional<DbValue> get(const std::string& key, DbTime date) const;
  // Podd LoadDatabase search semantics for one item: try prefix+name, then search up
  // the name tree according to 'search' (0 = no search, >0 = up to level 'search',
  // <0 = at most |search| levels up). 'prefix' must end in '.' (or be empty).
  std::optional<DbValue> find(const std::string& prefix, const std::string& name, DbTime date,
                              int search = 0) const;
  // All assignments of 'key' in file order, each tagged with the time stamp in force.
  std::vector<DbValue> history(const std::string& key) const;
  // All distinct time stamps that appear in the file (sorted, unique).
  std::vector<DbTime> timestamps() const;
  // Distinct keys that appear on the left of an assignment.
  std::vector<std::string> keys() const;

  // Conversions (throw DbError with key + text on failure; semantics of Podd's
  // convert_string: whole token must be consumed).
  static double toDouble(const DbValue& v);
  static long long toInt(const DbValue& v);
  static std::vector<double> toDoubles(const DbValue& v);
  static std::vector<long long> toInts(const DbValue& v);
  static std::vector<std::string> toTokens(const DbValue& v);

 private:
  struct Logical {
    std::string text;     // logical line (after ReadDBline processing)
    int line = 0;         // first physical line
    int status = 0;       // IsDBkey-style: 0 = no assignment, 1 = assignment
    std::string key;      // trimmed key if status==1 (may be empty: "= x")
    std::string value;    // trimmed value if status==1
  };
  std::vector<Logical> lines_;
  std::string path_;
  void parse(const std::vector<std::string>& physical);
};

// Typed lookups relative to an object prefix ("bb.gem.m0."), with provenance log.
class DbReader {
 public:
  DbReader(const DbFile& file, DbTime date, std::string prefix);
  const std::string& prefix() const { return prefix_; }
  DbTime date() const { return date_; }

  std::optional<DbValue> raw(const std::string& name, int search = 0) const;
  bool has(const std::string& name, int search = 0) const { return raw(name, search).has_value(); }

  double getDouble(const std::string& name, int search = 0) const;  // throws if missing
  double getDouble(const std::string& name, double def, int search) const;
  long long getInt(const std::string& name, int search = 0) const;
  long long getInt(const std::string& name, long long def, int search) const;
  std::vector<double> getDoubles(const std::string& name, int search = 0, bool required = true) const;
  std::vector<long long> getInts(const std::string& name, int search = 0, bool required = true) const;
  std::vector<std::string> getTokens(const std::string& name, int search = 0, bool required = true) const;

  // Every value successfully read through this reader (key, block time, line).
  const std::vector<DbValue>& used() const { return used_; }

 private:
  const DbFile* file_;
  DbTime date_;
  std::string prefix_;
  mutable std::vector<DbValue> used_;
  DbValue require(const std::string& name, int search) const;
};

// Podd database directory resolution (GetDBFileList).
struct DbDir {
  std::string root;              // e.g. ".../SBS-replay/DB"
  bool search_cwd = false;       // Podd also tries ./db_<name>.dat first; off by default
  explicit DbDir(std::string r = "DB") : root(std::move(r)) {}
  // Candidate paths in Podd order for "bb.gem" -> db_bb.gem.dat.
  std::vector<std::string> candidates(const std::string& name, DbTime date) const;
  // First existing candidate; throws DbError if none.
  std::string resolve(const std::string& name, DbTime date) const;
  DbFile open(const std::string& name, DbTime date) const { return DbFile::load(resolve(name, date)); }
};

}  // namespace geom
}  // namespace gemfilter
