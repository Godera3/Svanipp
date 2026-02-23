#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <mutex>
#include <cstdint>
#include <thread>

using namespace std;

namespace svanipp::console {

struct TuiTransfer {
    string path;
    string status;
    int pct = 0;
    double mbps = 0.0;
    int eta = -1;
    double sizeMb = 0.0;
    double doneMb = 0.0;
};

class TuiManager {
public:
    static TuiManager& instance();

    void init(bool noTui, bool noColor);
    bool enabled() const;

    void start();
    void stop();

    void set_mode(const string& mode);
    void set_local(const string& ip, uint16_t port);
    void set_totals(size_t totalFiles, uint64_t totalBytes);
    void set_stats(size_t okFiles, size_t failedFiles, uint64_t okBytes);
    void set_hint(const string& hint);
    void set_active_connections(int count);

    size_t add_transfer(const string& path, double sizeMb);
    size_t ensure_transfer(const string& path, double sizeMb);
    void update_transfer(size_t id,
                         const string& status,
                         int pct,
                         double mbps,
                         int eta,
                         double doneMb);
    void update_transfer_by_path(const string& path,
                                 const string& status,
                                 int pct,
                                 double mbps,
                                 int eta,
                                 double doneMb,
                                 double sizeMb);
    void mark_done_by_path(const string& path, const string& status, double doneMb, double sizeMb);
    void mark_done(size_t id, const string& status, double doneMb);

    bool quit_requested() const;
    void request_quit();

private:
    TuiManager() = default;

    void render_loop();
    void render_frame();
    string style(const string& text, const string& color) const;
    string truncate_middle(const string& s, size_t maxLen) const;
    void get_terminal_size(int& width, int& height) const;
    void hide_cursor();
    void show_cursor();
    void clear_screen();
    void check_input();

    bool enabled_ = false;
    bool useColor_ = false;
    bool running_ = false;
    bool quitRequested_ = false;

    string mode_ = "";
    string localIp_ = "";
    uint16_t localPort_ = 0;

    size_t totalFiles_ = 0;
    uint64_t totalBytes_ = 0;
    size_t okFiles_ = 0;
    size_t failedFiles_ = 0;
    uint64_t okBytes_ = 0;
    int activeConnections_ = 0;

    string hint_ = "Ctrl+C to stop";

    chrono::steady_clock::time_point startTime_{};
    vector<TuiTransfer> transfers_;

    mutable mutex mutex_;
    thread renderThread_;
};

} // namespace svanipp::console
