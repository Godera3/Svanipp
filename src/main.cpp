#include "net/winsock_init.h"
#include "transfer/receiver.h"
#include "transfer/sender.h"
#include "discovery/discovery.h"
#include "console/console_ui.h"
#include "console/tui.h"


#include <cstdlib>
#include <thread>
#include <cstdint>
#include <iostream>
#include <string>
#include <filesystem>
#include <iomanip>
#include <vector>
#include <chrono>
#include <sstream>

using namespace std;

static void usage() {
    cout <<
        "svanipp v1.0.0\n"
        "Usage:\n"
    "  svanipp receive --port <p> --out <dir> [--overwrite] [--summary] [--no-color]\n"
        "    --overwrite: skip confirmation prompt on file collision (default: prompt user)\n"
        "    --summary: print cumulative totals after each file\n"
        "    --no-color: disable ANSI colors/icons\n"
        "    --no-tui: force classic logs even in terminal\n"
    "    --io-timeout <sec>: header/digest timeout (default: 5)\n"
    "    --idle-timeout <sec>: transfer idle timeout (default: 15)\n"
    "  svanipp send [--ip <ip> | --name <device>] [--no-color] [--no-tui] <file1> [file2 ...]\n"
    "    --connect-timeout <sec>: connect timeout (default: 3)\n"
    "    --io-timeout <sec>: header/digest timeout (default: 5)\n"
    "    --idle-timeout <sec>: transfer idle timeout (default: 15)\n"
    "    --retries <n>: retry count for transient failures (default: 3)\n"
        "    without --ip/--name: interactive device selection\n"
        "  svanipp discover [--no-color] [--no-tui]\n";
}

static bool argEq(const char* a, const char* b) {
    return string(a) == string(b);
}

int main(int argc, char** argv) {
    WinsockInit wsa;
    if (!wsa.ok()) {
        cerr << "WSAStartup failed\n";
        return 1;
    }

    if (argc < 2) { usage(); return 1; }

    string cmd = argv[1];

    // ===== RECEIVE =====
    if (cmd == "receive") {
        uint16_t port = 39000;
        string outDir = "Downloads";
        bool overwrite = false;
        bool summary = false;
        bool noColor = false;
        bool noTui = false;
        int ioTimeoutSec = 5;
        int idleTimeoutSec = 15;

        for (int i = 2; i < argc; ++i) {
            if (argEq(argv[i], "--port") && i + 1 < argc) {
                port = static_cast<uint16_t>(stoi(argv[++i]));
            } else if (argEq(argv[i], "--out") && i + 1 < argc) {
                outDir = argv[++i];
            } else if (argEq(argv[i], "--overwrite")) {
                overwrite = true;
            } else if (argEq(argv[i], "--summary")) {
                summary = true;
            } else if (argEq(argv[i], "--no-color")) {
                noColor = true;
            } else if (argEq(argv[i], "--no-tui")) {
                noTui = true;
            } else if (argEq(argv[i], "--io-timeout") && i + 1 < argc) {
                ioTimeoutSec = stoi(argv[++i]);
            } else if (argEq(argv[i], "--idle-timeout") && i + 1 < argc) {
                idleTimeoutSec = stoi(argv[++i]);
            } else {
                usage();
                return 1;
            }
        }

        svanipp::console::ConsoleUI::instance().init(noColor);
        auto& tui = svanipp::console::TuiManager::instance();
        tui.init(noTui, noColor);
        if (tui.enabled()) {
            tui.set_mode("RECEIVE");
            tui.set_local("0.0.0.0", port);
            tui.set_hint("Ctrl+C to stop receiver");
            tui.start();
        }

        string devName = "SvanippDevice";
        if (const char* cn = getenv("COMPUTERNAME")) devName = cn;

        thread([port, devName] {
            svanipp::discovery::run_responder(port, devName);
        }).detach();


        int rr = svanipp::run_receiver(port, outDir, overwrite, summary, ioTimeoutSec, idleTimeoutSec);
        tui.stop();
        return rr;
    }

    // ===== SEND =====
    if (cmd == "send") {
        string ip;
        string name;
        uint16_t port = 39000;
        vector<string> files;
        bool noColor = false;
        bool noTui = false;
        int connectTimeoutSec = 3;
        int ioTimeoutSec = 5;
        int idleTimeoutSec = 15;
        int retries = 3;

        for (int i = 2; i < argc; ++i) {
            if (argEq(argv[i], "--ip") && i + 1 < argc) {
                ip = argv[++i];
            } else if (argEq(argv[i], "--name") && i + 1 < argc) {
                name = argv[++i];
            } else if (argEq(argv[i], "--port") && i + 1 < argc) {
                port = static_cast<uint16_t>(stoi(argv[++i]));
            } else if (argEq(argv[i], "--no-color")) {
                noColor = true;
            } else if (argEq(argv[i], "--no-tui")) {
                noTui = true;
            } else if (argEq(argv[i], "--connect-timeout") && i + 1 < argc) {
                connectTimeoutSec = stoi(argv[++i]);
            } else if (argEq(argv[i], "--io-timeout") && i + 1 < argc) {
                ioTimeoutSec = stoi(argv[++i]);
            } else if (argEq(argv[i], "--idle-timeout") && i + 1 < argc) {
                idleTimeoutSec = stoi(argv[++i]);
            } else if (argEq(argv[i], "--retries") && i + 1 < argc) {
                retries = stoi(argv[++i]);
            } else {
                files.push_back(argv[i]);
            }
        }

        auto& ui = svanipp::console::ConsoleUI::instance();
        ui.init(noColor);
        auto& tui = svanipp::console::TuiManager::instance();
        tui.init(noTui, noColor);

        if (files.empty()) {
            usage();
            return 1;
        }

        // If both ip and name provided, that's an error
        if (!ip.empty() && !name.empty()) {
            usage();
            return 1;
        }

        // If neither ip nor name provided, do interactive device selection
        if (ip.empty() && name.empty()) {
            ui.log(svanipp::console::Style::Info, "Discovering devices...");
            auto devices = svanipp::discovery::run_scan(1500);
            if (devices.empty()) {
                ui.log(svanipp::console::Style::Warn, "No Svanipp devices found.");
                return 1;
            }
            cout << "\nAvailable devices:\n";
            for (size_t i = 0; i < devices.size(); ++i) {
                cout << "  " << (i + 1) << ") " << devices[i].name << " (" << devices[i].ip << ":" << devices[i].port << ")\n";
            }
            cout << "\nSelect device (1-" << devices.size() << "): ";
            cout.flush();
            
            string input;
            getline(cin, input);
            
            int choice = 0;
            try {
                choice = stoi(input);
            } catch (...) {
                ui.log(svanipp::console::Style::Fail, "Invalid input.");
                return 1;
            }
            
            if (choice < 1 || choice > static_cast<int>(devices.size())) {
                ui.log(svanipp::console::Style::Fail, "Out of range.");
                return 1;
            }
            
            ip = devices[choice - 1].ip;
            port = devices[choice - 1].port;
        } else if (!name.empty()) {
            // Lookup device by name
            auto devices = svanipp::discovery::run_scan(1500);
            bool found = false;
            for (const auto& d : devices) {
                if (d.name == name) {
                    ip = d.ip;
                    port = d.port;
                    found = true;
                    break;
                }
            }
            if (!found) {
                ui.log(svanipp::console::Style::Fail, "Device not found: " + name);
                ui.log(svanipp::console::Style::Info, "Tip: run `svanipp discover` to see available devices.");
                return 1;
            }
        }

        // Expand folders recursively and compute relative paths
        namespace fs = filesystem;
        struct FileToSend {
            string absPath;
            string relPath;
            FileToSend(const string& a, const string& r) : absPath(a), relPath(r) {}
        };
        vector<FileToSend> sendList;
        uint64_t totalSize = 0;
        for (const auto& arg : files) {
            fs::path p = fs::u8path(arg);
            if (!fs::exists(p)) {
                cerr << "File/folder not found: " << arg << "\n";
                continue;
            }
            if (fs::is_directory(p)) {
                // Recursively add all files under this folder, including folder name in relative path
                auto abs_p = fs::absolute(p);
                for (auto& entry : fs::recursive_directory_iterator(abs_p)) {
                    if (!entry.is_regular_file()) continue;
                    auto abs = entry.path();
                    auto rel = fs::relative(abs, abs_p.parent_path());
                    sendList.emplace_back(abs.u8string(), rel.generic_u8string());
                    totalSize += fs::file_size(abs);
                }
            } else {
                auto abs = fs::absolute(p);
                sendList.emplace_back(abs.u8string(), p.filename().u8string());
                totalSize += fs::file_size(abs);
            }
        }
        if (sendList.empty()) {
            ui.log(svanipp::console::Style::Warn, "No files to send.");
            return 1;
        }
        if (tui.enabled()) {
            tui.set_mode("SEND");
            tui.set_local(ip.empty() ? "-" : ip, port);
            tui.set_totals(sendList.size(), totalSize);
            tui.set_hint("Ctrl+C to stop, Q to quit sender");
            tui.start();
            for (const auto& f : sendList) {
                tui.ensure_transfer(f.relPath, 0.0);
            }
        }
        {
            ostringstream prep;
            prep << "Preparing to send " << sendList.size() << " files, total size "
                 << fixed << setprecision(2) << (totalSize / (1024.0 * 1024.0)) << " MB";
            ui.log(svanipp::console::Style::Info, prep.str());
        }
        struct Failure {
            string path;
            string reason;
        };
        vector<Failure> failures;
        int result = 0;
        size_t successFiles = 0;
        uint64_t successBytes = 0;
        uint64_t attemptedBytes = 0;
        using clock = chrono::steady_clock;
        bool started = false;
        clock::time_point startTime{};
        for (const auto& f : sendList) {
            if (tui.enabled() && tui.quit_requested()) {
                failures.push_back({ f.relPath, "cancelled" });
                result = 1;
                continue;
            }
            if (!started) {
                startTime = clock::now();
                started = true;
            }
            uint64_t bytesSent = 0;
            string error;
            int r = svanipp::run_sender(ip,
                                        port,
                                        f.absPath,
                                        f.relPath,
                                        bytesSent,
                                        error,
                                        connectTimeoutSec,
                                        ioTimeoutSec,
                                        idleTimeoutSec,
                                        retries);
            if (r == 0) {
                successFiles++;
                successBytes += bytesSent;
            } else {
                result = r;
                if (error.empty()) {
                    error = "failed";
                }
                failures.push_back({ f.relPath, error });
            }

            attemptedBytes += bytesSent;
            auto now = clock::now();
            double elapsed = chrono::duration<double>(now - startTime).count();
            double attemptedMb = attemptedBytes / (1024.0 * 1024.0);
            double totalMb = totalSize / (1024.0 * 1024.0);
            double avgBps = (elapsed > 0.0) ? (static_cast<double>(attemptedBytes) / elapsed) : 0.0;
            double avgMbps = (elapsed > 0.0) ? (attemptedMb / elapsed) : 0.0;
            int eta = (avgBps > 0.0) ? static_cast<int>((totalSize - attemptedBytes) / avgBps) : -1;
            size_t doneCount = successFiles + failures.size();
            ostringstream overall;
            overall << "Overall: " << doneCount << "/" << sendList.size() << " files, "
                    << fixed << setprecision(2) << attemptedMb << "/" << totalMb << " MB, "
                    << fixed << setprecision(1) << avgMbps << " MB/s, ETA " << eta << "s";
            ui.log(svanipp::console::Style::Info, overall.str());
            if (tui.enabled()) {
                tui.set_stats(successFiles, failures.size(), successBytes);
            }
        }

        if (started) {
            auto endTime = clock::now();
            double elapsed = chrono::duration<double>(endTime - startTime).count();
            double mb = successBytes / (1024.0 * 1024.0);
            double mbps = (elapsed > 0.0) ? (mb / elapsed) : 0.0;
            vector<string> summaryLines;
            summaryLines.push_back("Summary");
            {
                ostringstream s1;
                s1 << "  Total files : " << sendList.size();
                summaryLines.push_back(s1.str());
            }
            {
                ostringstream s2;
                s2 << "  Succeeded   : " << successFiles;
                summaryLines.push_back(s2.str());
            }
            {
                ostringstream s3;
                s3 << "  Failed      : " << failures.size();
                summaryLines.push_back(s3.str());
            }
            {
                ostringstream s4;
                s4 << "  Bytes OK    : " << fixed << setprecision(2) << mb << " MB";
                summaryLines.push_back(s4.str());
            }
            {
                ostringstream s5;
                s5 << "  Time        : " << fixed << setprecision(2) << elapsed << " s";
                summaryLines.push_back(s5.str());
            }
            {
                ostringstream s6;
                s6 << "  Avg speed   : " << fixed << setprecision(1) << mbps << " MB/s";
                summaryLines.push_back(s6.str());
            }
            ui.print_block(summaryLines);

            if (!failures.empty()) {
                vector<string> failedLines;
                failedLines.push_back("Failed items:");
                for (const auto& f : failures) {
                    failedLines.push_back("  " + f.path + " (" + f.reason + ")");
                }
                ui.print_block(failedLines);
            }
        }

        tui.stop();

        return result;
    }

    // ===== DISCOVER =====
    if (cmd == "discover") {
        bool noColor = false;
        bool noTui = false;
        for (int i = 2; i < argc; ++i) {
            if (argEq(argv[i], "--no-color")) {
                noColor = true;
            } else if (argEq(argv[i], "--no-tui")) {
                noTui = true;
            } else {
                usage();
                return 1;
            }
        }
        svanipp::console::ConsoleUI::instance().init(noColor);
        svanipp::console::TuiManager::instance().init(noTui, noColor);
        auto devices = svanipp::discovery::run_scan(1500);
        if (devices.empty()) {
            cout << "No Svanipp devices found.\n";
            return 0;
        }
        for (const auto& d : devices) {
            cout << d.ip << "  " << d.name << "  " << d.port << "\n";
        }
        return 0;
    }

    usage();
    return 1;
}