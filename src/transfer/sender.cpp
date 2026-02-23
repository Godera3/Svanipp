#include "transfer/sender.h"
#include "transfer/protocol.h"
#include "net/socket_utils.h"
#include "crypto/sha256.h"
#include "console/console_ui.h"
#include "console/tui.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <cstring>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cerrno>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace std;

static int clamp_timeout_ms(int sec) {
    if (sec <= 0) return 0;
    return sec * 1000;
}

static bool set_socket_timeouts(socket_t sock, int recvMs, int sendMs) {
#ifdef _WIN32
    int rcv = recvMs;
    int snd = sendMs;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcv), sizeof(rcv)) != 0) {
        return false;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&snd), sizeof(snd)) != 0) {
        return false;
    }
    return true;
#else
    timeval rcv{};
    rcv.tv_sec = recvMs / 1000;
    rcv.tv_usec = (recvMs % 1000) * 1000;
    timeval snd{};
    snd.tv_sec = sendMs / 1000;
    snd.tv_usec = (sendMs % 1000) * 1000;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv)) != 0) return false;
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd)) != 0) return false;
    return true;
#endif
}

static bool set_nonblocking(socket_t sock, bool enabled) {
#ifdef _WIN32
    u_long mode = enabled ? 1 : 0;
    return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) return false;
    if (enabled) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    return fcntl(sock, F_SETFL, flags) == 0;
#endif
}

static int last_socket_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static bool is_timeout_error(int err) {
#ifdef _WIN32
    return err == WSAETIMEDOUT || err == WSAEWOULDBLOCK;
#else
    return err == ETIMEDOUT || err == EAGAIN || err == EWOULDBLOCK;
#endif
}

static bool is_retryable_error(const string& reason) {
    return reason == "connect timeout" ||
           reason == "connect failed" ||
           reason == "send timeout" ||
           reason == "connection lost" ||
           reason == "send header failed" ||
           reason == "send filename failed" ||
           reason == "send digest failed";
}

static bool connect_with_timeout(socket_t sock, const sockaddr_in& addr, int timeoutMs, string& reason) {
    if (!set_nonblocking(sock, true)) {
        reason = "connect failed";
        return false;
    }
    int r = ::connect(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (r == 0) {
        set_nonblocking(sock, false);
        return true;
    }

#ifdef _WIN32
    if (WSAGetLastError() != WSAEWOULDBLOCK) {
        set_nonblocking(sock, false);
        reason = "connect failed";
        return false;
    }
#else
    if (errno != EINPROGRESS) {
        set_nonblocking(sock, false);
        reason = "connect failed";
        return false;
    }
#endif

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int sel = select(static_cast<int>(sock + 1), nullptr, &wfds, nullptr, &tv);
    if (sel == 0) {
        set_nonblocking(sock, false);
        reason = "connect timeout";
        return false;
    }
    if (sel < 0) {
        set_nonblocking(sock, false);
        reason = "connect failed";
        return false;
    }

    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len) != 0 || so_error != 0) {
        set_nonblocking(sock, false);
        reason = "connect failed";
        return false;
    }

    set_nonblocking(sock, false);
    return true;
}

int svanipp::run_sender(const string& ip,
                        uint16_t port,
                        const string& filePath,
                        const string& relPath,
                        uint64_t& bytesSent,
                        string& error,
                        int connectTimeoutSec,
                        int ioTimeoutSec,
                        int idleTimeoutSec,
                        int retries) {
    namespace fs = filesystem;
    bytesSent = 0;
    error.clear();
    auto& ui = svanipp::console::ConsoleUI::instance();
    auto& tui = svanipp::console::TuiManager::instance();

    const int connectTimeoutMs = clamp_timeout_ms(connectTimeoutSec);
    const int ioTimeoutMs = clamp_timeout_ms(ioTimeoutSec);
    const int idleTimeoutMs = clamp_timeout_ms(idleTimeoutSec);
    const int maxAttempts = max(1, retries + 1);

    fs::path p = fs::u8path(filePath);
    if (!fs::exists(p)) {
        ui.log(svanipp::console::Style::Fail, "File not found: " + filePath);
        error = "file not found";
        return 1;
    }

    const string filename = relPath;
    const uint64_t fileSize = static_cast<uint64_t>(fs::file_size(p));
    const double fileSizeMb = fileSize / (1024.0 * 1024.0);
    if (tui.enabled()) {
        tui.ensure_transfer(filename, fileSizeMb);
    }

    ifstream in(p, ios::binary);
    if (!in) {
        ui.log(svanipp::console::Style::Fail, "Cannot open file: " + filePath);
        error = "open failed";
        return 1;
    }

    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        if (attempt > 0) {
            int backoffMs = (attempt == 1) ? 200 : (attempt == 2) ? 500 : 1000;
            ui.log(svanipp::console::Style::Warn,
                   "Retrying " + filename + " (attempt " + to_string(attempt + 1) + "/" +
                   to_string(maxAttempts) + ") after " + to_string(backoffMs) + " ms");
            this_thread::sleep_for(chrono::milliseconds(backoffMs));
        }

        if (tui.enabled()) {
            tui.update_transfer_by_path(filename, "SENDING", 0, 0.0, -1, 0.0, fileSizeMb);
        }

        in.clear();
        in.seekg(0, ios::beg);
        svanipp::crypto::Sha256 hasher;

        socket_t sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) {
            ui.log(svanipp::console::Style::Fail, "socket() failed");
            error = "socket failed";
            return 1;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(port));
        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
            ui.log(svanipp::console::Style::Fail, "Invalid IP: " + ip);
            closesocket(sock);
            error = "invalid ip";
            return 1;
        }

        string connectReason;
        if (!connect_with_timeout(sock, addr, connectTimeoutMs, connectReason)) {
            ui.progress_end(true);
            ui.log(svanipp::console::Style::Fail, "Connect failed: " + connectReason);
            closesocket(sock);
            error = connectReason;
            if (attempt + 1 < maxAttempts && is_retryable_error(error)) continue;
            return 1;
        }

        set_socket_timeouts(sock, ioTimeoutMs, ioTimeoutMs);

        svanipp::proto::HeaderFixed hf{};
        memcpy(hf.magic, svanipp::proto::MAGIC, 8);
        hf.version = hton_u16(svanipp::proto::VERSION);
        hf.filename_len = hton_u16(static_cast<uint16_t>(filename.size()));
        hf.file_size = hton_u64(fileSize);

        if (!sendAll(sock, &hf, sizeof(hf))) {
            int err = last_socket_error();
            error = is_timeout_error(err) ? "send timeout" : "send header failed";
            ui.progress_end(true);
            ui.log(svanipp::console::Style::Fail, "Send header failed: " + filename);
            closesocket(sock);
            if (attempt + 1 < maxAttempts && is_retryable_error(error)) continue;
            return 1;
        }
        if (!sendAll(sock, filename.data(), filename.size())) {
            int err = last_socket_error();
            error = is_timeout_error(err) ? "send timeout" : "send filename failed";
            ui.progress_end(true);
            ui.log(svanipp::console::Style::Fail, "Send filename failed: " + filename);
            closesocket(sock);
            if (attempt + 1 < maxAttempts && is_retryable_error(error)) continue;
            return 1;
        }

        set_socket_timeouts(sock, ioTimeoutMs, idleTimeoutMs);

        const size_t BUF = 64 * 1024;
        vector<char> buf(BUF);

        using clock = chrono::steady_clock;
        auto start_time = clock::now();
        uint64_t sent = 0;
        bool failed = false;
        while (in) {
            in.read(buf.data(), static_cast<streamsize>(BUF));
            streamsize n = in.gcount();
            if (n <= 0) break;
            hasher.update(buf.data(), static_cast<size_t>(n));

            if (!sendAll(sock, buf.data(), static_cast<size_t>(n))) {
                int err = last_socket_error();
                error = is_timeout_error(err) ? "send timeout" : "connection lost";
                failed = true;
                break;
            }

            sent += static_cast<uint64_t>(n);
            if (fileSize > 0) {
                int pct = static_cast<int>((sent * 100ULL) / fileSize);
                auto now = clock::now();
                double elapsed = chrono::duration<double>(now - start_time).count();
                double mbps = (elapsed > 0.0) ? (sent / (1024.0 * 1024.0)) / elapsed : 0.0;
                double bps = (elapsed > 0.0) ? (static_cast<double>(sent) / elapsed) : 0.0;
                int eta = (bps > 0.0) ? static_cast<int>((fileSize - sent) / bps) : -1;
                string line = ui.make_status_line("Send", filename, pct, mbps, eta);
                ui.progress_update(line, pct);
                if (tui.enabled()) {
                    tui.update_transfer_by_path(filename, "SENDING", pct, mbps, eta,
                                                sent / (1024.0 * 1024.0), fileSizeMb);
                }
            }
        }

        if (!failed) {
            uint8_t digest[32];
            hasher.final(digest);
            set_socket_timeouts(sock, ioTimeoutMs, ioTimeoutMs);
            if (!sendAll(sock, digest, sizeof(digest))) {
                int err = last_socket_error();
                error = is_timeout_error(err) ? "send timeout" : "send digest failed";
                failed = true;
            }
        }

        if (failed || sent != fileSize) {
            ui.progress_end(true);
            if (error.empty()) error = "connection lost";
            ui.log(svanipp::console::Style::Fail, "Send failed: " + filename + " (" + error + ")");
            if (tui.enabled()) {
                tui.mark_done_by_path(filename, "FAIL", sent / (1024.0 * 1024.0), fileSizeMb);
            }
            closesocket(sock);
            bytesSent = sent;
            if (attempt + 1 < maxAttempts && is_retryable_error(error)) continue;
            return 1;
        }

        ui.progress_end(true);
        double elapsed = chrono::duration<double>(clock::now() - start_time).count();
        double mb = sent / (1024.0 * 1024.0);
        ostringstream okMsg;
        okMsg << "Sent " << filename << " (" << fixed << setprecision(2) << mb << " MB, "
              << fixed << setprecision(2) << elapsed << " s)";
        ui.log(svanipp::console::Style::Ok, okMsg.str());
        if (tui.enabled()) {
            tui.mark_done_by_path(filename, "OK", sent / (1024.0 * 1024.0), fileSizeMb);
        }
        closesocket(sock);
        bytesSent = sent;
        return 0;
    }

    return 1;
}