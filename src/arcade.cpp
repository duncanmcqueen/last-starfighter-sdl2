// Arcade implements the high-score table codec and the diagnostics-menu
// label lookup. The high-score file format is a block of blank lines, ten
// name lines, one blank line, then ten score lines; the parser tolerates
// minor variations in the blank-line count by walking the non-empty runs.
// The diagnostic labels are the exact strings the diagnostics menu prints
// for each dipswitch setting.
#include "arcade.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace {

// Number of blank lines written at the top of a saved high-score file.
constexpr std::size_t kBlankLeadingLines = 12;
// High-score files use Windows CRLF line endings.
constexpr char kNewline[3] = {'\r', '\n', '\0'};
// The high-score file holds at most this many name/score entries.
constexpr std::size_t kEntries = 10;

std::vector<std::string> split_crlf(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : text) {
        if (c == '\n') {
            if (!current.empty() && current.back() == '\r') {
                current.pop_back();
            }
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

} // namespace

HighScoreTable HighScoreTable::load(const std::filesystem::path& path) {
    HighScoreTable table;
    std::ifstream in(path);
    if (!in) {
        // A missing file yields an empty table rather than throwing, so the
        // "no high scores yet" screen shows when none have been saved.
        return table;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::vector<std::string> lines = split_crlf(buffer.str());
    std::vector<std::string> names;
    std::vector<std::string> scores;
    // The layout is a block of blank lines, ten names, one blank line, then
    // ten scores. Walk the non-empty runs so minor blank-count differences
    // still parse.
    std::size_t index = 0;
    while (index < lines.size() && lines[index].empty()) {
        ++index;
    }
    while (index < lines.size() && !lines[index].empty() && names.size() < 10) {
        names.push_back(lines[index++]);
    }
    while (index < lines.size() && lines[index].empty()) {
        ++index;
    }
    while (index < lines.size() && !lines[index].empty() &&
           scores.size() < 10) {
        scores.push_back(lines[index++]);
    }
    for (std::size_t i = 0; i < kEntries; ++i) {
        Entry entry;
        if (i < names.size()) {
            entry.initials = names[i];
        }
        if (i < scores.size()) {
            try {
                entry.score = std::stoi(scores[i]);
            } catch (const std::exception&) {
                entry.score = 0;
            }
        }
        table.entries_[i] = entry;
    }
    return table;
}

bool HighScoreTable::qualifies(int score) const {
    return score > 0 &&
           score > entries_[kEntries - 1].score;
}

std::size_t HighScoreTable::insert(const std::string& initials, int score) {
    Entry entry{initials, score};
    std::vector<Entry> entries(entries_.begin(), entries_.end());
    auto position = std::upper_bound(
        entries.begin(), entries.end(), entry,
        [](const Entry& lhs, const Entry& rhs) { return lhs.score > rhs.score; });
    const auto rank = static_cast<std::size_t>(position - entries.begin());
    entries.insert(position, entry);
    entries.resize(kEntries);
    std::copy(entries.begin(), entries.end(), entries_.begin());
    return rank;
}

void HighScoreTable::save(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return;
    }
    std::string text;
    for (std::size_t i = 0; i < kBlankLeadingLines; ++i) {
        text += kNewline;
    }
    for (const Entry& entry : entries_) {
        text += entry.initials + kNewline;
    }
    text += kNewline;
    for (const Entry& entry : entries_) {
        text += std::to_string(entry.score) + kNewline;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string diagnostic_label(std::size_t index) {
    switch (index) {
    case 6: return "Coin to start: ";
    case 7: return "Coin to continue: ";
    case 8: return "Shields at start and continue: ";
    case 9: return "Recharge in play: ";
    case 10: return "Recharge between levels: ";
    case 11: return "Second weapon recharge in play: ";
    case 12: return "Second weapon between levels: ";
    case 13: return "Close Shots: ";
    case 14: return "Bullseye: ";
    case 17: return "Risk DB Malfunction: ";
    case 19: return "Turret acceleration: ";
    case 20: return "Max Speed: ";
    default: return "";
    }
}

std::vector<std::string> diagnostic_rows(const std::array<int, 21>& values,
                                         std::size_t selected) {
    std::vector<std::string> rows;
    rows.reserve(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        std::string label = diagnostic_label(i);
        if (label.empty()) {
            label = "Switch " + std::to_string(i) + ": ";
        }
        if (i == selected) {
            label = "> " + label;
        } else {
            label = "  " + label;
        }
        rows.push_back(label + std::to_string(values[i]));
    }
    return rows;
}
