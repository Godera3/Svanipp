#include "transfer/receiver.h"
#include "transfer/protocol.h"
#include "net/socket_utils.h"
#include "crypto/sha256.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <iomanip>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <cstring>

using namespace std;

static string sanitizeFilename(string name) {
    // minimal Windows-safe sanitization
    for (char& c : name) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
            c == '\\' || c == '|' || c == '?' || c == '*') {
            c = '_';
        }
    }
    if (name.empty()) name = "file.bin";
    return name;
}

static filesystem::path uniquePath(filesystem::path p) {
    namespace fs = filesystem;

    if (!fs::exists(p)) return p;

    fs::path dir = p.parent_path();
    string stem = p.stem().string();
    string ext  = p.extension().string();

    for (int i = 1; i < 10000; ++i) {
        fs::path candidate = dir / (stem + " (" + to_string(i) + ")" + ext);
        if (!fs::exists(candidate)) return candidate;
    }

    // fallback if somehow everything exists
    return dir / (stem + " (copy)" + ext);
}

static string truncate_middle(const string& s, size_t maxLen) {
    if (s.size() <= maxLen) return s;
    if (maxLen <= 3) return s.substr(0, maxLen);
    size_t keepFront = (maxLen - 3) / 2;
    size_t keepBack  = maxLen - 3 - keepFront;
    return s.substr(0, keepFront) + "..." + s.substr(s.size() - keepBack);
}

static void print_status_line(const string& line, size_t& lastLen) {
    cout << '\r' << line;
    if (line.size() < lastLen) {
        cout << string(lastLen - line.size(), ' ');
    }
    lastLen = line.size();
    cout.flush();
}




int svanipp::run_receiver(uint16_t port, const string& outDir, bool overwrite) {
    namespace fs = filesystem;

    fs::path outPath = fs::path(outDir);
    error_code ec;
    fs::create_directories(outPath, ec);

    socket_t listenSock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        cerr << "socket() failed\n";
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    int yes = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));

    if (::bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        cerr << "bind() failed\n";
        closesocket(listenSock);
        return 1;
    }

    if (::listen(listenSock, 1) == SOCKET_ERROR) {
        cerr << "listen() failed\n";
        closesocket(listenSock);
        return 1;
    }

    cout << "Listening on 0.0.0.0:" << port << ", saving to " << outPath.string() << "\n";

    for (;;) {
        sockaddr_in client{};
        int clientLen = sizeof(client);
        socket_t clientSock = ::accept(listenSock, reinterpret_cast<sockaddr*>(&client), &clientLen);
        if (clientSock == INVALID_SOCKET) {
            cerr << "accept() failed\n";
            continue;
        }

        // Read fixed header
        svanipp::proto::HeaderFixed hf{};
        if (!recvExact(clientSock, &hf, sizeof(hf))) {
            cerr << "Failed to read header\n";
            closesocket(clientSock);
            continue;
        }

        // Validate magic
        if (memcmp(hf.magic, svanipp::proto::MAGIC, 8) != 0) {
            cerr << "Bad MAGIC (not a svanipp stream)\n";
            closesocket(clientSock);
            continue;
        }

        const uint16_t version = ntoh_u16(hf.version);
        const uint16_t nameLen = ntoh_u16(hf.filename_len);
        const uint64_t fileSize = ntoh_u64(hf.file_size);

        if (version != svanipp::proto::VERSION) {
            cerr << "Unsupported version: " << version << "\n";
            closesocket(clientSock);
            continue;
        }
        if (nameLen == 0 || nameLen > 4096) {
            cerr << "Invalid filename length\n";
            closesocket(clientSock);
            continue;
        }

        // Read filename
        vector<char> nameBuf(nameLen);
        if (!recvExact(clientSock, nameBuf.data(), nameBuf.size())) {
            cerr << "Failed to read filename\n";
            closesocket(clientSock);
            continue;
        }
        string relPath(nameBuf.begin(), nameBuf.end());
        // Sanitize relative path: reject absolute, drive letters, .., leading slashes
        fs::path rel(relPath);
        bool bad = rel.is_absolute() || rel.has_root_name() || rel.string().find("..") != string::npos;
        if (bad) {
            cerr << "Rejected unsafe path: " << relPath << "\n";
            closesocket(clientSock);
            continue;
        }
        // Remove leading slashes
        while (!rel.empty() && rel.string().front() == '\\') rel = rel.string().substr(1);
        // Compose final path
        fs::path saveAs = outPath / rel;
        // Create parent directories
        fs::create_directories(saveAs.parent_path(), ec);
        if (!overwrite) {
            saveAs = uniquePath(saveAs);
        }
        ofstream out(saveAs, ios::binary);
        if (!out) {
            cerr << "Cannot open output file: " << saveAs.string() << "\n";
            closesocket(clientSock);
            continue;
        }

        // Stream file bytes
        const size_t BUF = 64 * 1024;
        vector<char> buf(BUF);

        svanipp::crypto::Sha256 hasher;
        uint64_t remaining = fileSize;
        uint64_t received = 0;

        using clock = chrono::steady_clock;
        auto start_time = clock::now();
        auto last_update = start_time;
        int last_pct = -1;
        size_t last_len = 0;

        while (remaining > 0) {
            const size_t want = (remaining > BUF) ? BUF : static_cast<size_t>(remaining);
            int r = ::recv(clientSock, buf.data(), static_cast<int>(want), 0);
            if (r <= 0) {
                cerr << "\nConnection lost while receiving\n";
                break;
            }
            hasher.update(buf.data(), static_cast<size_t>(r));
            out.write(buf.data(), r);
            remaining -= static_cast<uint64_t>(r);
            received += static_cast<uint64_t>(r);

            // Progress: truncate relPath, erase using previous length, update every 150ms or percent change
            if (fileSize > 0) {
                int pct = static_cast<int>((received * 100ULL) / fileSize);
                auto now = clock::now();
                auto since_last = chrono::duration_cast<chrono::milliseconds>(now - last_update).count();
                if (pct != last_pct || since_last >= 150) {
                    string fname = truncate_middle(relPath, 32);
                    ostringstream oss;
                    oss << "Receiving " << fname << " ... " << pct << "%";
                    string msg = oss.str();
                    print_status_line(msg, last_len);
                    last_update = now;
                    last_pct = pct;
                }
            }
        }
        // clear progress line after transfer and print newline
        print_status_line("", last_len);
        cout << "\n";

        uint8_t gotDigest[32];
        if (received == fileSize) {
            if (!recvExact(clientSock, gotDigest, sizeof(gotDigest))) {
                cerr << "Failed to read SHA-256 digest\n";
                closesocket(clientSock);
                continue;
            }

            uint8_t calcDigest[32];
            hasher.final(calcDigest);

            if (memcmp(gotDigest, calcDigest, 32) != 0) {
                cerr << "SHA-256 MISMATCH (file integrity failed)\n";
                closesocket(clientSock);
                continue;
            }
        }

        double mb = received / (1024.0 * 1024.0);
        cout << "\nSaved: " << saveAs.string()
              << " (" << fixed << setprecision(2) << mb << " MB)\n";

        closesocket(clientSock);
        // loop back and accept next client; listenSock remains open
    }

    return 0;
}