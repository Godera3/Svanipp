#include "console/tui.h"

#include <iostream>
#include <sstream>
#include <iomanip>
#include <thread>
#include <algorithm>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <conio.h>
#else
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <fcntl.h>
#endif

using namespace std;

namespace svanipp::console {

static TuiManager* g_tui_for_exit = nullptr;

static void tui_atexit_cleanup() {
    if (g_tui_for_exit) {
        g_tui_for_exit->stop();
    }
}

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
    return SetConsoleMode(hOut, newMode) != 0;
#else
    return true;
#endif
}

TuiManager& TuiManager::instance() {
    static TuiManager inst;
    return inst;
}

void TuiManager::init(bool noTui, bool noColor) {
    enabled_ = !noTui && stdout_is_tty();
    if (!enabled_) {
        useColor_ = false;
        return;
    }
#ifdef _WIN32
    useColor_ = !noColor && enable_vt_on_windows();
#else
    useColor_ = !noColor;
#endif
}

bool TuiManager::enabled() const { return enabled_; }

void TuiManager::start() {
    if (!enabled_ || running_) return;
    {
        lock_guard<mutex> lock(mutex_);
        running_ = true;
        quitRequested_ = false;
        startTime_ = chrono::steady_clock::now();
    }
    clear_screen();
    hide_cursor();
    g_tui_for_exit = this;
    atexit(tui_atexit_cleanup);
    renderThread_ = thread([this] { render_loop(); });
}

void TuiManager::stop() {
    if (!enabled_) return;
    {
        lock_guard<mutex> lock(mutex_);
        if (!running_) return;
        running_ = false;
    }
    if (renderThread_.joinable()) {
        renderThread_.join();
    }
    show_cursor();
    cout << "\x1b[0m\n";
    cout.flush();
}

void TuiManager::set_mode(const string& mode) {
    lock_guard<mutex> lock(mutex_);
    mode_ = mode;
}

void TuiManager::set_local(const string& ip, uint16_t port) {
    lock_guard<mutex> lock(mutex_);
    localIp_ = ip;
    localPort_ = port;
}

void TuiManager::set_totals(size_t totalFiles, uint64_t totalBytes) {
    lock_guard<mutex> lock(mutex_);
    totalFiles_ = totalFiles;
    totalBytes_ = totalBytes;
}

void TuiManager::set_stats(size_t okFiles, size_t failedFiles, uint64_t okBytes) {
    lock_guard<mutex> lock(mutex_);
    okFiles_ = okFiles;
    failedFiles_ = failedFiles;
    okBytes_ = okBytes;
}

void TuiManager::set_hint(const string& hint) {
    lock_guard<mutex> lock(mutex_);
    hint_ = hint;
}

void TuiManager::set_active_connections(int count) {
    lock_guard<mutex> lock(mutex_);
    activeConnections_ = count;
}

size_t TuiManager::add_transfer(const string& path, double sizeMb) {
    lock_guard<mutex> lock(mutex_);
    transfers_.push_back({path, "WAIT", 0, 0.0, -1, sizeMb, 0.0});
    return transfers_.size() - 1;
}

size_t TuiManager::ensure_transfer(const string& path, double sizeMb) {
    lock_guard<mutex> lock(mutex_);
    for (size_t i = 0; i < transfers_.size(); ++i) {
        if (transfers_[i].path == path && transfers_[i].status != "OK" && transfers_[i].status != "FAIL") {
            if (sizeMb > 0.0) transfers_[i].sizeMb = sizeMb;
            return i;
        }
    }
    transfers_.push_back({path, "WAIT", 0, 0.0, -1, sizeMb, 0.0});
    return transfers_.size() - 1;
}

void TuiManager::update_transfer(size_t id,
                                 const string& status,
                                 int pct,
                                 double mbps,
                                 int eta,
                                 double doneMb) {
    lock_guard<mutex> lock(mutex_);
    if (id >= transfers_.size()) return;
    transfers_[id].status = status;
    transfers_[id].pct = pct;
    transfers_[id].mbps = mbps;
    transfers_[id].eta = eta;
    transfers_[id].doneMb = doneMb;
}

void TuiManager::mark_done(size_t id, const string& status, double doneMb) {
    lock_guard<mutex> lock(mutex_);
    if (id >= transfers_.size()) return;
    transfers_[id].status = status;
    transfers_[id].pct = (status == "OK") ? 100 : transfers_[id].pct;
    transfers_[id].doneMb = doneMb;
    transfers_[id].eta = 0;
}

void TuiManager::update_transfer_by_path(const string& path,
                                         const string& status,
                                         int pct,
                                         double mbps,
                                         int eta,
                                         double doneMb,
                                         double sizeMb) {
    size_t id = ensure_transfer(path, sizeMb);
    update_transfer(id, status, pct, mbps, eta, doneMb);
}

void TuiManager::mark_done_by_path(const string& path, const string& status, double doneMb, double sizeMb) {
    size_t id = ensure_transfer(path, sizeMb);
    mark_done(id, status, doneMb);
}

bool TuiManager::quit_requested() const {
    lock_guard<mutex> lock(mutex_);
    return quitRequested_;
}

void TuiManager::request_quit() {
    lock_guard<mutex> lock(mutex_);
    quitRequested_ = true;
}

void TuiManager::render_loop() {
    while (enabled()) {
        {
            lock_guard<mutex> lock(mutex_);
            if (!running_) break;
        }
        check_input();
        render_frame();
        this_thread::sleep_for(chrono::milliseconds(100));
    }
}

void TuiManager::render_frame() {
    lock_guard<mutex> lock(mutex_);
    if (!running_) return;

    int w = 80, h = 24;
    get_terminal_size(w, h);

    auto elapsedSec = chrono::duration_cast<chrono::seconds>(chrono::steady_clock::now() - startTime_).count();
    double okMb = okBytes_ / (1024.0 * 1024.0);
    double totalMb = totalBytes_ / (1024.0 * 1024.0);
    double avgMbps = elapsedSec > 0 ? okMb / static_cast<double>(elapsedSec) : 0.0;

    ostringstream out;
    out << "\x1b[H\x1b[2J";
    out << style("Svanipp v1.0.0", "36") << " | Mode: " << mode_
        << " | " << localIp_ << ":" << localPort_
        << " | Elapsed: " << elapsedSec << "s\n";
    out << string(static_cast<size_t>(max(0, w - 1)), '=') << "\n";
    out << "STATUS    PATH                                 PROG   SPEED     ETA   SIZE\n";
    out << string(static_cast<size_t>(max(0, w - 1)), '-') << "\n";

    int maxRows = max(1, h - 8);
    int start = max(0, static_cast<int>(transfers_.size()) - maxRows);
    for (int i = start; i < static_cast<int>(transfers_.size()); ++i) {
        const auto& t = transfers_[i];
        ostringstream row;
        row << t.status;
        string status = row.str();
        if (status.size() < 8) status += string(8 - status.size(), ' ');

        string path = truncate_middle(t.path, 35);
        if (path.size() < 35) path += string(35 - path.size(), ' ');

        ostringstream prog; prog << t.pct << "%";
        string progStr = prog.str();
        if (progStr.size() < 6) progStr = string(6 - progStr.size(), ' ') + progStr;

        ostringstream speed; speed << fixed << setprecision(1) << t.mbps << " MB/s";
        string speedStr = speed.str();
        if (speedStr.size() < 9) speedStr = string(9 - speedStr.size(), ' ') + speedStr;

        ostringstream eta; eta << (t.eta < 0 ? 0 : t.eta) << "s";
        string etaStr = eta.str();
        if (etaStr.size() < 5) etaStr = string(5 - etaStr.size(), ' ') + etaStr;

        ostringstream size; size << fixed << setprecision(2) << t.sizeMb << " MB";
        string sizeStr = size.str();

        out << status << "  " << path << "  " << progStr << "  " << speedStr << "  "
            << etaStr << "  " << sizeStr << "\n";
    }

    out << string(static_cast<size_t>(max(0, w - 1)), '=') << "\n";
    out << "Files: " << okFiles_ << "/" << totalFiles_
        << " | Failed: " << failedFiles_
        << " | Data: " << fixed << setprecision(2) << okMb << "/" << totalMb << " MB"
        << " | Avg: " << fixed << setprecision(1) << avgMbps << " MB/s"
        << " | Active: " << activeConnections_ << "\n";
    out << hint_ << "\n";

    cout << out.str();
    cout.flush();
}

string TuiManager::style(const string& text, const string& color) const {
    if (!useColor_) return text;
    return "\x1b[" + color + "m" + text + "\x1b[0m";
}

string TuiManager::truncate_middle(const string& s, size_t maxLen) const {
    if (s.size() <= maxLen) return s;
    if (maxLen <= 3) return s.substr(0, maxLen);
    size_t keepFront = (maxLen - 3) / 2;
    size_t keepBack = maxLen - 3 - keepFront;
    return s.substr(0, keepFront) + "..." + s.substr(s.size() - keepBack);
}

void TuiManager::get_terminal_size(int& width, int& height) const {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        return;
    }
#else
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        width = ws.ws_col;
        height = ws.ws_row;
        return;
    }
#endif
    width = 80;
    height = 24;
}

void TuiManager::hide_cursor() {
    cout << "\x1b[?25l";
    cout.flush();
}

void TuiManager::show_cursor() {
    cout << "\x1b[?25h";
    cout.flush();
}

void TuiManager::clear_screen() {
    cout << "\x1b[2J\x1b[H";
    cout.flush();
}

void TuiManager::check_input() {
#ifdef _WIN32
    if (_kbhit()) {
        int ch = _getch();
        if (ch == 'q' || ch == 'Q') {
            request_quit();
        }
    }
#else
    // Minimal non-blocking input check on POSIX could be added; omitted for now.
#endif
}

} // namespace svanipp::console
