#include "net/winsock_init.h"
#include "transfer/receiver.h"
#include "transfer/sender.h"
#include "discovery/discovery.h"


#include <cstdlib>
#include <thread>
#include <cstdint>
#include <iostream>
#include <string>
#include <filesystem>
#include <iomanip>

using namespace std;

static void usage() {
    cout <<
        "svanipp v1\n"
        "Usage:\n"
        "  svanipp receive --port <p> --out <dir> [--overwrite]\n"
        "  svanipp send --ip <ip> <file1> [file2 ...]  # send one or more files\n"
        "  svanipp discover\n";
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

        for (int i = 2; i < argc; ++i) {
            if (argEq(argv[i], "--port") && i + 1 < argc) {
                port = static_cast<uint16_t>(stoi(argv[++i]));
            } else if (argEq(argv[i], "--out") && i + 1 < argc) {
                outDir = argv[++i];
            } else if (argEq(argv[i], "--overwrite")) {
                overwrite = true;
            } else {
                usage();
                return 1;
            }
        }

        string devName = "SvanippDevice";
        if (const char* cn = getenv("COMPUTERNAME")) devName = cn;

        thread([port, devName] {
            svanipp::discovery::run_responder(port, devName);
        }).detach();


        return svanipp::run_receiver(port, outDir, overwrite);
    }

    // ===== SEND =====
    if (cmd == "send") {
        string ip;
        string name;
        uint16_t port = 39000;
        vector<string> files;

        for (int i = 2; i < argc; ++i) {
            if (argEq(argv[i], "--ip") && i + 1 < argc) {
                ip = argv[++i];
            } else if (argEq(argv[i], "--name") && i + 1 < argc) {
                name = argv[++i];
            } else if (argEq(argv[i], "--port") && i + 1 < argc) {
                port = static_cast<uint16_t>(stoi(argv[++i]));
            } else {
                files.push_back(argv[i]);
            }
        }

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
            cout << "Discovering devices...\n";
            auto devices = svanipp::discovery::run_scan(1500);
            if (devices.empty()) {
                cerr << "No Svanipp devices found.\n";
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
                cerr << "Invalid input.\n";
                return 1;
            }
            
            if (choice < 1 || choice > static_cast<int>(devices.size())) {
                cerr << "Out of range.\n";
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
                cerr << "Device not found: " << name << "\n";
                cerr << "Tip: run `svanipp discover` to see available devices.\n";
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
            fs::path p(arg);
            if (!fs::exists(p)) {
                cerr << "File/folder not found: " << arg << "\n";
                continue;
            }
            if (fs::is_directory(p)) {
                // Recursively add all files under this folder, including folder name in relative path
                auto abs_p = fs::absolute(p);
                auto folderName = abs_p.filename().string();
                for (auto& entry : fs::recursive_directory_iterator(abs_p)) {
                    if (!entry.is_regular_file()) continue;
                    auto abs = entry.path();
                    auto rel = fs::relative(abs, abs_p.parent_path());
                    sendList.emplace_back(abs.string(), rel.string());
                    totalSize += fs::file_size(abs);
                }
            } else {
                auto abs = fs::absolute(p);
                sendList.emplace_back(abs.string(), p.filename().string());
                totalSize += fs::file_size(abs);
            }
        }
        if (sendList.empty()) {
            cerr << "No files to send.\n";
            return 1;
        }
        cout << "Preparing to send " << sendList.size() << " files, total size "
              << fixed << setprecision(2) << (totalSize / (1024.0 * 1024.0)) << " MB\n";
        int result = 0;
        for (const auto& f : sendList) {
            int r = svanipp::run_sender(ip, port, f.absPath, f.relPath);
            if (r != 0) result = r;
        }
        return result;
    }

    // ===== DISCOVER =====
    if (cmd == "discover") {
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