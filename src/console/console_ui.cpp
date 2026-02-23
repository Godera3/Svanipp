#include "console/console_ui.h"
#include "console/tui.h"

#include <iostream>
#include <sstream>
#include <algorithm>
#include <iomanip>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

using namespace std;

namespace svanipp::console {

static bool stdout_is_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

static bool enable_vt_on_windows() {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return false;
    DWORD mode = 0;
    if (!GetConsoleMode(hOut, &mode)) return false;
    if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) return true;
    DWORD newMode = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (!SetConsoleMode(hOut, newMode)) return false;
    return true;
#else
    return true;
#endif
}

ConsoleUI& ConsoleUI::instance() {
    static ConsoleUI inst;
    return inst;
}

void ConsoleUI::init(bool noColor) {
    useColor_ = false;
    useIcons_ = false;

    if (!noColor && stdout_is_tty()) {
#ifdef _WIN32
        useColor_ = enable_vt_on_windows();
#else
        useColor_ = true;
#endif
    }
    useIcons_ = !noColor; // ASCII icons are safe in all terminals
}

void ConsoleUI::set_throttle_ms(int ms) {
    throttleMs_ = ms;
}

void ConsoleUI::log(Style style, const string& message) {
    if (svanipp::console::TuiManager::instance().enabled()) {
        return;
    }
    clear_progress(true);
    cout << style_prefix(style) << message << "\n";
}

void ConsoleUI::print_block(const vector<string>& lines) {
    if (svanipp::console::TuiManager::instance().enabled()) {
        return;
    }
    clear_progress(true);
    for (const auto& line : lines) {
        cout << line << "\n";
    }
}

void ConsoleUI::progress_update(const string& line, int pct) {
    if (svanipp::console::TuiManager::instance().enabled()) {
        return;
    }
    if (!should_update(pct)) return;
    progressActive_ = true;
    cout << '\r' << line;
    if (line.size() < lastLen_) {
        cout << string(lastLen_ - line.size(), ' ');
    }
    lastLen_ = line.size();
    cout.flush();
}

void ConsoleUI::progress_end(bool newline) {
    if (svanipp::console::TuiManager::instance().enabled()) {
        return;
    }
    clear_progress(newline);
}

string ConsoleUI::make_status_line(const string& dir,
                                        const string& path,
                                        int pct,
                                        double mbps,
                                        int etaSec) const {
    ostringstream oss;
    string shortPath = truncate_middle(path, 36);
    if (etaSec < 0) etaSec = 0;
    oss << dir << " | " << shortPath << " | "
        << pct << "% | "
        << fixed << setprecision(1) << mbps << " MB/s | ETA "
        << etaSec << "s";
    return oss.str();
}

string ConsoleUI::truncate_middle(const string& s, size_t maxLen) const {
    if (s.size() <= maxLen) return s;
    if (maxLen <= 3) return s.substr(0, maxLen);
    size_t keepFront = (maxLen - 3) / 2;
    size_t keepBack  = maxLen - 3 - keepFront;
    return s.substr(0, keepFront) + "..." + s.substr(s.size() - keepBack);
}

string ConsoleUI::style_prefix(Style style) const {
    string prefix;
    switch (style) {
        case Style::Ok:   prefix = useIcons_ ? "[OK] " : "OK: "; break;
        case Style::Fail: prefix = useIcons_ ? "[X] "  : "FAIL: "; break;
        case Style::Warn: prefix = useIcons_ ? "[!] "  : "WARN: "; break;
        case Style::Info: prefix = useIcons_ ? "[i] "  : "INFO: "; break;
    }
    return colorize(style, prefix);
}

string ConsoleUI::colorize(Style style, const string& text) const {
    if (!useColor_) return text;
    const char* color = "";
    switch (style) {
        case Style::Ok:   color = "\x1b[32m"; break; // green
        case Style::Fail: color = "\x1b[31m"; break; // red
        case Style::Warn: color = "\x1b[33m"; break; // yellow
        case Style::Info: color = "\x1b[36m"; break; // cyan
    }
    return string(color) + text + "\x1b[0m";
}

bool ConsoleUI::should_update(int pct) {
    auto now = chrono::steady_clock::now();
    if (lastPct_ == -1) {
        lastPct_ = pct;
        lastUpdate_ = now;
        return true;
    }
    auto sinceLast = chrono::duration_cast<chrono::milliseconds>(now - lastUpdate_).count();
    if (pct != lastPct_ || sinceLast >= throttleMs_) {
        lastPct_ = pct;
        lastUpdate_ = now;
        return true;
    }
    return false;
}

void ConsoleUI::clear_progress(bool newline) {
    if (!progressActive_) return;
    cout << '\r' << string(lastLen_, ' ') << '\r';
    if (newline) cout << "\n";
    cout.flush();
    progressActive_ = false;
    lastLen_ = 0;
    lastPct_ = -1;
}

} // namespace svanipp::console
