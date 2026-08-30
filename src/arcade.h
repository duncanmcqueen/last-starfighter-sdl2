#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

// Cabinet meta-state that lives outside the combat simulation: the persistent
// high-score table and the diagnostics menu labels.
//
// arrays/StrftrScores.dat is plain CRLF text: twelve blank lines, ten
// three-character name lines, one blank line, ten score lines written as
// plain decimals, and a trailing CRLF. The game reads the shipped table and
// mirrors any new entry to "starfighter-scores.dat" beside the executable's
// working directory so the supplied release tree stays untouched.
class HighScoreTable final {
  public:
    struct Entry {
        std::string initials = "AAA";
        int score = 0;
    };

    static constexpr std::size_t kEntries = 10;

    // Parses the shipped text format. Missing or malformed files yield the
    // shipped default table so the cabinet always has something to show.
    static HighScoreTable load(const std::filesystem::path& path);

    // True when a finished game would place in the table.
    bool qualifies(int score) const;

    // Inserts an entry, dropping the previous tenth place. Returns the rank
    // (0-based) the entry took.
    std::size_t insert(const std::string& initials, int score);

    // Writes the same CRLF text layout back out.
    void save(const std::filesystem::path& path) const;

    const std::array<Entry, kEntries>& entries() const { return entries_; }

  private:
    std::array<Entry, kEntries> entries_{};
};

// The label the diagnostics menu prints for a dipswitch index, or an empty
// string where the index has no known label.
std::string diagnostic_label(std::size_t index);

// The twenty-one shipped dipswitch values as a printable block, used by the
// diagnostics screen.
std::vector<std::string> diagnostic_rows(const std::array<int, 21>& values,
                                         std::size_t selected);
