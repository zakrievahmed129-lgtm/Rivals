#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN

#include "ipc_receiver.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <vector>

namespace nah2 {

namespace {

constexpr std::size_t kMaxHttpHeaderBytes = 16 * 1024;
constexpr std::size_t kMaxHttpBodyBytes = 2 * 1024 * 1024;
constexpr std::size_t kMaxWsFrameBytes = 2 * 1024 * 1024;

// --- SHA-1 + Base64 autonomes pour le handshake RFC 6455 (Sec-WebSocket-Accept) ---

inline uint32_t RotL(uint32_t x, uint32_t n) {
    return (x << n) | (x >> (32 - n));
}

std::array<uint8_t, 20> Sha1Digest(std::string_view input) {
    uint32_t h0 = 0x67452301u;
    uint32_t h1 = 0xEFCDAB89u;
    uint32_t h2 = 0x98BADCFEu;
    uint32_t h3 = 0x10325476u;
    uint32_t h4 = 0xC3D2E1F0u;

    const uint64_t bitLen = static_cast<uint64_t>(input.size()) * 8ULL;
    std::vector<uint8_t> padded(input.begin(), input.end());
    padded.push_back(0x80);
    while ((padded.size() % 64) != 56) {
        padded.push_back(0x00);
    }
    for (int i = 7; i >= 0; --i) {
        padded.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xFF));
    }

    uint32_t w[80]{};
    for (std::size_t offset = 0; offset < padded.size(); offset += 64) {
        for (int i = 0; i < 16; ++i) {
            const std::size_t idx = offset + i * 4;
            w[i] = (static_cast<uint32_t>(padded[idx]) << 24) |
                   (static_cast<uint32_t>(padded[idx + 1]) << 16) |
                   (static_cast<uint32_t>(padded[idx + 2]) << 8) |
                   (static_cast<uint32_t>(padded[idx + 3]));
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = RotL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const uint32_t temp = RotL(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = RotL(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<uint8_t, 20> out{};
    const uint32_t hs[5] = { h0, h1, h2, h3, h4 };
    for (int i = 0; i < 5; ++i) {
        out[i * 4 + 0] = static_cast<uint8_t>((hs[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<uint8_t>((hs[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<uint8_t>((hs[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<uint8_t>(hs[i] & 0xFF);
    }
    return out;
}

std::string Base64Encode(const uint8_t* data, std::size_t len) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        const uint32_t b0 = data[i];
        const uint32_t b1 = (i + 1 < len) ? data[i + 1] : 0;
        const uint32_t b2 = (i + 2 < len) ? data[i + 2] : 0;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kTable[(triple >> 18) & 0x3F]);
        out.push_back(kTable[(triple >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? kTable[(triple >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? kTable[triple & 0x3F] : '=');
    }
    return out;
}

std::string ComputeWebSocketAccept(std::string_view clientKey) {
    static constexpr char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined;
    combined.reserve(clientKey.size() + sizeof(kGuid) - 1);
    combined.append(clientKey);
    combined.append(kGuid);
    const auto digest = Sha1Digest(combined);
    return Base64Encode(digest.data(), digest.size());
}

// --- Parseur JSON rapide mono-passe sans dependance externe ---

class JsonCursor {
public:
    explicit JsonCursor(std::string_view src) : src_(src) {}

    void SkipWs() {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    char Peek() {
        SkipWs();
        return (pos_ < src_.size()) ? src_[pos_] : '\0';
    }

    bool Consume(char expected) {
        SkipWs();
        if (pos_ < src_.size() && src_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool ParseString(std::string& out) {
        SkipWs();
        if (pos_ >= src_.size() || src_[pos_] != '"') {
            return false;
        }
        ++pos_;
        out.clear();
        while (pos_ < src_.size()) {
            const char c = src_[pos_++];
            if (c == '"') {
                return true;
            }
            if (c == '\\') {
                if (pos_ >= src_.size()) {
                    return false;
                }
                const char esc = src_[pos_++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u':
                        if (pos_ + 4 > src_.size()) {
                            return false;
                        }
                        pos_ += 4;
                        out.push_back('?');
                        break;
                    default:
                        out.push_back(esc);
                        break;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    template <typename T>
    bool ParseNumber(T& out) {
        SkipWs();
        if (pos_ >= src_.size()) {
            return false;
        }
        const char* begin = src_.data() + pos_;
        const char* end = src_.data() + src_.size();
        auto res = std::from_chars(begin, end, out);
        if (res.ec != std::errc{} || res.ptr == begin) {
            return false;
        }
        pos_ = static_cast<std::size_t>(res.ptr - src_.data());
        return true;
    }

    bool SkipValue() {
        const char c = Peek();
        if (c == '\0') {
            return false;
        }
        if (c == '"') {
            std::string tmp;
            return ParseString(tmp);
        }
        if (c == '{') {
            ++pos_;
            if (Consume('}')) return true;
            std::string key;
            while (true) {
                if (!ParseString(key) || !Consume(':') || !SkipValue()) {
                    return false;
                }
                if (Consume(',')) continue;
                return Consume('}');
            }
        }
        if (c == '[') {
            ++pos_;
            if (Consume(']')) return true;
            while (true) {
                if (!SkipValue()) {
                    return false;
                }
                if (Consume(',')) continue;
                return Consume(']');
            }
        }
        if (src_.substr(pos_).starts_with("true")) { pos_ += 4; return true; }
        if (src_.substr(pos_).starts_with("false")) { pos_ += 5; return true; }
        if (src_.substr(pos_).starts_with("null")) { pos_ += 4; return true; }
        double num = 0.0;
        return ParseNumber(num);
    }

    bool ParseVec3(Vec3& v) {
        if (!Consume('[')) return false;
        if (!ParseNumber(v.x) || !Consume(',')) return false;
        if (!ParseNumber(v.y) || !Consume(',')) return false;
        if (!ParseNumber(v.z)) return false;
        return Consume(']');
    }

    bool ParseVec2(float& x, float& y) {
        if (!Consume('[')) return false;
        if (!ParseNumber(x) || !Consume(',')) return false;
        if (!ParseNumber(y)) return false;
        return Consume(']');
    }

    bool ParseBoolOrInt(bool& out) {
        SkipWs();
        if (src_.substr(pos_).starts_with("true")) {
            pos_ += 4;
            out = true;
            return true;
        }
        if (src_.substr(pos_).starts_with("false")) {
            pos_ += 5;
            out = false;
            return true;
        }
        int32_t v = 0;
        if (!ParseNumber(v)) return false;
        out = (v != 0);
        return true;
    }

private:
    std::string_view src_;
    std::size_t pos_ = 0;
};

bool ParseCameraObject(JsonCursor& cur, CameraState& cam) {
    if (!cur.Consume('{')) return false;
    cam.mouse_behavior = 0;
    cam.typing_in_ui = false;
    cam.has_mouse_loc = false;
    cam.mouse_loc = {};
    cam.game_mouse_sens = 0.0f;

    if (cur.Consume('}')) return true;
    std::string key;
    while (true) {
        if (!cur.ParseString(key) || !cur.Consume(':')) return false;
        if (key == "p") {
            if (!cur.ParseVec3(cam.pos)) return false;
        } else if (key == "r") {
            if (!cur.ParseVec3(cam.right)) return false;
        } else if (key == "u") {
            if (!cur.ParseVec3(cam.up)) return false;
        } else if (key == "l") {
            if (!cur.ParseVec3(cam.look)) return false;
        } else if (key == "fov") {
            if (!cur.ParseNumber(cam.fov_deg)) return false;
        } else if (key == "vp") {
            if (!cur.ParseVec2(cam.viewport_w, cam.viewport_h)) return false;
        } else if (key == "mb") {
            uint32_t mb = 0;
            if (!cur.ParseNumber(mb)) return false;
            cam.mouse_behavior = static_cast<uint8_t>(mb);
        } else if (key == "ui") {
            if (!cur.ParseBoolOrInt(cam.typing_in_ui)) return false;
        } else if (key == "ml") {
            if (!cur.ParseVec2(cam.mouse_loc.x, cam.mouse_loc.y)) return false;
            cam.has_mouse_loc = true;
        } else if (key == "sens") {
            if (!cur.ParseNumber(cam.game_mouse_sens)) return false;
        } else {
            if (!cur.SkipValue()) return false;
        }

        if (cur.Consume(',')) continue;
        return cur.Consume('}');
    }
}

bool ParseJointsArray(JsonCursor& cur, EntityTelemetry& ent) {
    if (!cur.Consume('[')) return false;
    ent.joint_mask = 0;
    if (cur.Consume(']')) return true;

    while (true) {
        uint32_t slot = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (!cur.ParseNumber(slot) || !cur.Consume(',')) return false;
        if (!cur.ParseNumber(x) || !cur.Consume(',')) return false;
        if (!cur.ParseNumber(y) || !cur.Consume(',')) return false;
        if (!cur.ParseNumber(z)) return false;

        if (slot < kMaxRigJoints) {
            ent.joints[slot] = { x, y, z };
            ent.joint_mask |= static_cast<uint16_t>(1u << slot);
        }
        if (cur.Consume(',')) continue;
        return cur.Consume(']');
    }
}

bool ParseEntityObject(JsonCursor& cur, EntityTelemetry& ent) {
    if (!cur.Consume('{')) return false;
    ent.joint_mask = 0;
    ent.rig_type = 0;
    ent.kind = 0;
    ent.name.clear();
    ent.health = -1.0f;
    ent.max_health = -1.0f;
    ent.is_ally = false;
    ent.visible = -1;
    ent.has_vel = false;
    ent.vel = {};

    if (cur.Consume('}')) return true;
    std::string key;
    while (true) {
        if (!cur.ParseString(key) || !cur.Consume(':')) return false;
        if (key == "id") {
            if (!cur.ParseNumber(ent.id)) return false;
        } else if (key == "n") {
            if (!cur.ParseString(ent.name)) return false;
        } else if (key == "k") {
            uint32_t k = 0;
            if (!cur.ParseNumber(k)) return false;
            ent.kind = static_cast<uint8_t>(k);
        } else if (key == "rig") {
            uint32_t r = 0;
            if (!cur.ParseNumber(r)) return false;
            ent.rig_type = static_cast<uint8_t>(r);
        } else if (key == "p") {
            if (!cur.ParseVec3(ent.pos)) return false;
        } else if (key == "s") {
            if (!cur.ParseVec3(ent.size)) return false;
        } else if (key == "r") {
            if (!cur.ParseVec3(ent.right)) return false;
        } else if (key == "u") {
            if (!cur.ParseVec3(ent.up)) return false;
        } else if (key == "l") {
            if (!cur.ParseVec3(ent.look)) return false;
        } else if (key == "j") {
            if (!ParseJointsArray(cur, ent)) return false;
        } else if (key == "hp") {
            if (!cur.ParseNumber(ent.health)) return false;
        } else if (key == "mhp") {
            if (!cur.ParseNumber(ent.max_health)) return false;
        } else if (key == "ally") {
            if (!cur.ParseBoolOrInt(ent.is_ally)) return false;
        } else if (key == "vis") {
            int32_t v = -1;
            if (!cur.ParseNumber(v)) return false;
            ent.visible = static_cast<int8_t>(std::clamp(v, -1, 1));
        } else if (key == "v") {
            if (!cur.ParseVec3(ent.vel)) return false;
            ent.has_vel = true;
        } else {
            if (!cur.SkipValue()) return false;
        }

        if (cur.Consume(',')) continue;
        return cur.Consume('}');
    }
}

bool ParseEntitiesArray(JsonCursor& cur, std::vector<EntityTelemetry>& entities) {
    if (!cur.Consume('[')) return false;
    entities.clear();
    if (cur.Consume(']')) return true;

    while (true) {
        entities.emplace_back();
        if (!ParseEntityObject(cur, entities.back())) return false;
        if (cur.Consume(',')) continue;
        return cur.Consume(']');
    }
}

// --- Utilitaires HTTP / WebSocket ---

bool IEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool IContains(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;
    for (std::size_t i = 0; i <= haystack.size() - needle.size(); ++i) {
        if (IEquals(haystack.substr(i, needle.size()), needle)) {
            return true;
        }
    }
    return false;
}

std::string_view TrimWs(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

enum class ClientMode {
    Http,
    WebSocket,
};

struct TcpClient {
    SOCKET sock = INVALID_SOCKET;
    ClientMode mode = ClientMode::Http;
    std::string recv_buf;
    std::string ws_frag_buf;
    uint8_t ws_frag_opcode = 0;
    bool sent_continue = false;
};

void SendAll(SOCKET s, const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        const int n = ::send(s, data + sent, static_cast<int>(len - sent), 0);
        if (n <= 0) {
            break;
        }
        sent += static_cast<std::size_t>(n);
    }
}

void SendWsFrame(SOCKET s, uint8_t opcode, std::string_view payload) {
    std::string frame;
    frame.reserve(payload.size() + 10);
    frame.push_back(static_cast<char>(0x80u | (opcode & 0x0Fu)));
    if (payload.size() <= 125) {
        frame.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 65535) {
        frame.push_back(static_cast<char>(126));
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
        frame.push_back(static_cast<char>(payload.size() & 0xFF));
    } else {
        frame.push_back(static_cast<char>(127));
        const uint64_t sz = payload.size();
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<char>((sz >> (i * 8)) & 0xFF));
        }
    }
    frame.append(payload);
    SendAll(s, frame.data(), frame.size());
}

} // namespace

bool ParseTelemetryJson(std::string_view json, TelemetryFrame& outFrame) {
    JsonCursor cur(json);
    if (!cur.Consume('{')) {
        return false;
    }

    outFrame.seq = 0;
    outFrame.client_ts = 0.0;
    outFrame.entities.clear();
    bool hasCam = false;

    if (cur.Consume('}')) {
        return false;
    }

    std::string key;
    while (true) {
        if (!cur.ParseString(key) || !cur.Consume(':')) {
            return false;
        }
        if (key == "seq") {
            if (!cur.ParseNumber(outFrame.seq)) return false;
        } else if (key == "ts") {
            if (!cur.ParseNumber(outFrame.client_ts)) return false;
        } else if (key == "cam") {
            if (!ParseCameraObject(cur, outFrame.camera)) return false;
            hasCam = true;
        } else if (key == "ents") {
            if (!ParseEntitiesArray(cur, outFrame.entities)) return false;
        } else {
            if (!cur.SkipValue()) return false;
        }

        if (cur.Consume(',')) continue;
        if (!cur.Consume('}')) return false;
        return hasCam;
    }
}

uint64_t IpcReceiver::NowMicroseconds() {
    static LARGE_INTEGER freq = []() {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<uint64_t>((counter.QuadPart * 1000000ULL) / freq.QuadPart);
}

IpcReceiver::IpcReceiver(uint16_t port) : requested_port_(port) {}

IpcReceiver::~IpcReceiver() {
    Stop();
}

bool IpcReceiver::Start() {
    if (running_.load(std::memory_order_relaxed)) {
        return true;
    }

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return false;
    }

    SOCKET tcpSock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (tcpSock == INVALID_SOCKET) {
        WSACleanup();
        return false;
    }

    BOOL reuse = TRUE;
    setsockopt(tcpSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    u_long nonBlocking = 1;
    ioctlsocket(tcpSock, FIONBIO, &nonBlocking);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(requested_port_);

    if (::bind(tcpSock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        ::listen(tcpSock, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(tcpSock);
        WSACleanup();
        return false;
    }

    sockaddr_in boundAddr{};
    int addrLen = sizeof(boundAddr);
    if (::getsockname(tcpSock, reinterpret_cast<sockaddr*>(&boundAddr), &addrLen) == 0) {
        bound_port_.store(ntohs(boundAddr.sin_port), std::memory_order_relaxed);
    } else {
        bound_port_.store(requested_port_, std::memory_order_relaxed);
    }

    SOCKET udpSock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSock != INVALID_SOCKET) {
        sockaddr_in udpAddr{};
        udpAddr.sin_family = AF_INET;
        udpAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        udpAddr.sin_port = htons(bound_port_.load(std::memory_order_relaxed));
        if (::bind(udpSock, reinterpret_cast<const sockaddr*>(&udpAddr), sizeof(udpAddr)) == SOCKET_ERROR) {
            closesocket(udpSock);
            udpSock = INVALID_SOCKET;
        } else {
            ioctlsocket(udpSock, FIONBIO, &nonBlocking);
            int rcvBuf = 256 * 1024;
            setsockopt(udpSock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvBuf), sizeof(rcvBuf));
        }
    }

    if (!frame_event_) {
        frame_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    tcp_listen_sock_ = static_cast<uintptr_t>(tcpSock);
    udp_sock_ = static_cast<uintptr_t>(udpSock);
    running_.store(true, std::memory_order_release);

    worker_thread_ = std::thread(&IpcReceiver::ServerLoop, this);
    return true;
}

void IpcReceiver::Stop() {
    const bool wasRunning = running_.exchange(false, std::memory_order_acq_rel);
    if (frame_event_) {
        SetEvent(static_cast<HANDLE>(frame_event_));
    }
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    if (tcp_listen_sock_ != static_cast<uintptr_t>(INVALID_SOCKET)) {
        closesocket(static_cast<SOCKET>(tcp_listen_sock_));
        tcp_listen_sock_ = static_cast<uintptr_t>(INVALID_SOCKET);
    }
    if (udp_sock_ != static_cast<uintptr_t>(INVALID_SOCKET)) {
        closesocket(static_cast<SOCKET>(udp_sock_));
        udp_sock_ = static_cast<uintptr_t>(INVALID_SOCKET);
    }
    if (frame_event_) {
        CloseHandle(static_cast<HANDLE>(frame_event_));
        frame_event_ = nullptr;
    }
    if (wasRunning) {
        WSACleanup();
    }
}

void IpcReceiver::PublishFrame(TelemetryFrame& parsedFrame, const char* transportTag) {
    const uint64_t nowUs = NowMicroseconds();
    parsedFrame.received_us = nowUs;

    std::lock_guard<std::mutex> lock(frame_mutex_);
    std::swap(published_frame_, parsedFrame);
    has_unread_frame_ = true;
    stats_.packets_received += 1;
    stats_.last_packet_us = nowUs;
    std::snprintf(stats_.transport, sizeof(stats_.transport), "%s", transportTag);
    if (frame_event_) {
        SetEvent(static_cast<HANDLE>(frame_event_));
    }
}

bool IpcReceiver::ConsumeLatestFrame(TelemetryFrame& outFrame) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (!has_unread_frame_) {
        return false;
    }
    std::swap(outFrame, published_frame_);
    has_unread_frame_ = false;
    if (frame_event_) {
        ResetEvent(static_cast<HANDLE>(frame_event_));
    }
    return true;
}

IpcStats IpcReceiver::GetStats() const {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    return stats_;
}

void IpcReceiver::ServerLoop() {
    const SOCKET listenSock = static_cast<SOCKET>(tcp_listen_sock_);
    const SOCKET udpSock = static_cast<SOCKET>(udp_sock_);

    std::vector<TcpClient> clients;
    std::vector<WSAPOLLFD> pollFds;
    TelemetryFrame stagingFrame;
    stagingFrame.entities.reserve(64);

    std::string udpScratch(65536, '\0');
    uint64_t rateWindowStartUs = NowMicroseconds();
    uint64_t rateWindowPackets = 0;

    auto recordParseError = [this]() {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        stats_.parse_errors += 1;
    };

    auto updateClientMetrics = [this, &clients]() {
        uint32_t wsCount = 0;
        for (const auto& c : clients) {
            if (c.mode == ClientMode::WebSocket) {
                ++wsCount;
            }
        }
        std::lock_guard<std::mutex> lock(frame_mutex_);
        stats_.active_ws_clients = wsCount;
    };

    while (running_.load(std::memory_order_relaxed)) {
        pollFds.clear();
        pollFds.push_back({ listenSock, POLLRDNORM, 0 });
        if (udpSock != INVALID_SOCKET) {
            pollFds.push_back({ udpSock, POLLRDNORM, 0 });
        }
        const std::size_t clientStartIdx = pollFds.size();
        for (const auto& c : clients) {
            pollFds.push_back({ c.sock, POLLRDNORM, 0 });
        }

        const int ready = WSAPoll(pollFds.data(), static_cast<ULONG>(pollFds.size()), 20);
        if (ready == SOCKET_ERROR) {
            break;
        }

        const uint64_t nowUs = NowMicroseconds();
        if (nowUs - rateWindowStartUs >= 500000ULL) {
            const double elapsedSec = static_cast<double>(nowUs - rateWindowStartUs) / 1000000.0;
            {
                std::lock_guard<std::mutex> lock(frame_mutex_);
                stats_.packets_per_sec = static_cast<double>(rateWindowPackets) / elapsedSec;
            }
            rateWindowStartUs = nowUs;
            rateWindowPackets = 0;
        }

        if (ready == 0) {
            continue;
        }

        const std::size_t existingClientCount = clients.size();

        // 1. Nouvelles connexions TCP
        if (pollFds[0].revents & POLLRDNORM) {
            while (true) {
                SOCKET clientSock = ::accept(listenSock, nullptr, nullptr);
                if (clientSock == INVALID_SOCKET) {
                    break;
                }
                u_long nonBlock = 1;
                ioctlsocket(clientSock, FIONBIO, &nonBlock);
                BOOL noDelay = TRUE;
                setsockopt(clientSock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
                int rcvBuf = 256 * 1024;
                setsockopt(clientSock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvBuf), sizeof(rcvBuf));
                TcpClient cli;
                cli.sock = clientSock;
                cli.recv_buf.reserve(16 * 1024);
                clients.push_back(std::move(cli));
            }
        }

        // 2. Datagrames UDP entrants
        if (udpSock != INVALID_SOCKET && (pollFds[1].revents & POLLRDNORM)) {
            while (true) {
                const int n = ::recvfrom(udpSock, udpScratch.data(), static_cast<int>(udpScratch.size()), 0, nullptr, nullptr);
                if (n <= 0) {
                    break;
                }
                std::string_view sv(udpScratch.data(), static_cast<std::size_t>(n));
                if (ParseTelemetryJson(sv, stagingFrame)) {
                    PublishFrame(stagingFrame, "UDP");
                    ++rateWindowPackets;
                } else {
                    recordParseError();
                }
            }
        }

        // 3. Flux TCP (HTTP/1.1 Keep-Alive & WebSocket RFC 6455)
        bool clientsChanged = false;
        for (std::size_t i = existingClientCount; i > 0; --i) {
            const std::size_t idx = i - 1;
            const SHORT rev = pollFds[clientStartIdx + idx].revents;
            if (rev == 0) {
                continue;
            }
            TcpClient& c = clients[idx];
            bool closeClient = (rev & (POLLERR | POLLHUP | POLLNVAL)) != 0;

            if (!closeClient && (rev & POLLRDNORM)) {
                char chunk[8192];
                while (true) {
                    const int n = ::recv(c.sock, chunk, sizeof(chunk), 0);
                    if (n > 0) {
                        c.recv_buf.append(chunk, static_cast<std::size_t>(n));
                        if (c.recv_buf.size() > kMaxHttpBodyBytes + kMaxHttpHeaderBytes) {
                            closeClient = true;
                            break;
                        }
                    } else if (n == 0) {
                        closeClient = true;
                        break;
                    } else {
                        const int err = WSAGetLastError();
                        if (err != WSAEWOULDBLOCK) {
                            closeClient = true;
                        }
                        break;
                    }
                }
            }

            // Machine a etats HTTP / WebSocket (avec curseur pour eviter les memmove repetes)
            std::size_t consumePos = 0;
            while (!closeClient && consumePos < c.recv_buf.size()) {
                const std::string_view unread(
                    c.recv_buf.data() + consumePos,
                    c.recv_buf.size() - consumePos);

                if (c.mode == ClientMode::Http) {
                    const std::size_t hdrEnd = unread.find("\r\n\r\n");
                    if (hdrEnd == std::string_view::npos) {
                        if (unread.size() > kMaxHttpHeaderBytes) {
                            closeClient = true;
                        }
                        break;
                    }

                    const std::string_view hdrBlock = unread.substr(0, hdrEnd);
                    const std::size_t bodyStart = hdrEnd + 4;

                    // Parse requete + en-tetes utiles
                    std::size_t lineEnd = hdrBlock.find("\r\n");
                    const std::string_view reqLine = (lineEnd == std::string_view::npos) ? hdrBlock : hdrBlock.substr(0, lineEnd);

                    bool isPost = reqLine.starts_with("POST ");
                    bool isGet = reqLine.starts_with("GET ");
                    bool upgradeWs = false;
                    bool expectContinue = false;
                    bool wantClose = reqLine.find("HTTP/1.0") != std::string_view::npos;
                    std::string wsKey;
                    std::size_t contentLength = 0;

                    std::size_t pos = (lineEnd == std::string_view::npos) ? hdrBlock.size() : lineEnd + 2;
                    while (pos < hdrBlock.size()) {
                        const std::size_t next = hdrBlock.find("\r\n", pos);
                        const std::string_view line = (next == std::string_view::npos)
                            ? hdrBlock.substr(pos)
                            : hdrBlock.substr(pos, next - pos);
                        const std::size_t colon = line.find(':');
                        if (colon != std::string_view::npos) {
                            const std::string_view name = TrimWs(line.substr(0, colon));
                            const std::string_view val = TrimWs(line.substr(colon + 1));
                            if (IEquals(name, "Upgrade") && IEquals(val, "websocket")) {
                                upgradeWs = true;
                            } else if (IEquals(name, "Sec-WebSocket-Key")) {
                                wsKey = std::string(val);
                            } else if (IEquals(name, "Content-Length")) {
                                std::from_chars(val.data(), val.data() + val.size(), contentLength);
                            } else if (IEquals(name, "Expect") && IContains(val, "100-continue")) {
                                expectContinue = true;
                            } else if (IEquals(name, "Connection") && IContains(val, "close")) {
                                wantClose = true;
                            }
                        }
                        if (next == std::string_view::npos) break;
                        pos = next + 2;
                    }

                    if (upgradeWs && !wsKey.empty()) {
                        const std::string accept = ComputeWebSocketAccept(wsKey);
                        const std::string resp =
                            "HTTP/1.1 101 Switching Protocols\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
                        SendAll(c.sock, resp.data(), resp.size());
                        consumePos += bodyStart;
                        c.sent_continue = false;
                        c.mode = ClientMode::WebSocket;
                        clientsChanged = true;
                        continue;
                    }

                    if (contentLength > kMaxHttpBodyBytes) {
                        closeClient = true;
                        break;
                    }
                    if (unread.size() < bodyStart + contentLength) {
                        if (expectContinue && !c.sent_continue) {
                            static constexpr char kContinue[] = "HTTP/1.1 100 Continue\r\n\r\n";
                            SendAll(c.sock, kContinue, sizeof(kContinue) - 1);
                            c.sent_continue = true;
                        }
                        break; // Attendre le reste du corps HTTP
                    }

                    const std::string_view body = unread.substr(bodyStart, contentLength);
                    if (isPost) {
                        if (ParseTelemetryJson(body, stagingFrame)) {
                            PublishFrame(stagingFrame, "HTTP");
                            ++rateWindowPackets;
                        } else {
                            recordParseError();
                        }
                        const char* resp = wantClose
                            ? "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                            : "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n";
                        SendAll(c.sock, resp, std::strlen(resp));
                    } else if (isGet && reqLine.find("/health") != std::string_view::npos) {
                        static constexpr char kHealth[] =
                            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 31\r\nConnection: keep-alive\r\n\r\n"
                            "{\"status\":\"ok\",\"service\":\"nah2\"}";
                        SendAll(c.sock, kHealth, sizeof(kHealth) - 1);
                    } else {
                        static constexpr char kNotFound[] =
                            "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                        SendAll(c.sock, kNotFound, sizeof(kNotFound) - 1);
                        wantClose = true;
                    }

                    consumePos += bodyStart + contentLength;
                    c.sent_continue = false;
                    if (wantClose) {
                        closeClient = true;
                        break;
                    }
                } else {
                    // Mode WebSocket RFC 6455 (demasquage in-place zero copie pour trames completes)
                    if (unread.size() < 2) break;
                    const uint8_t b0 = static_cast<uint8_t>(unread[0]);
                    const uint8_t b1 = static_cast<uint8_t>(unread[1]);
                    const bool fin = (b0 & 0x80u) != 0;
                    const uint8_t opcode = b0 & 0x0Fu;
                    const bool masked = (b1 & 0x80u) != 0;
                    uint64_t payloadLen = b1 & 0x7Fu;
                    std::size_t headerSize = 2;

                    if (payloadLen == 126) {
                        if (unread.size() < 4) break;
                        payloadLen = (static_cast<uint64_t>(static_cast<uint8_t>(unread[2])) << 8) |
                                     static_cast<uint64_t>(static_cast<uint8_t>(unread[3]));
                        headerSize = 4;
                    } else if (payloadLen == 127) {
                        if (unread.size() < 10) break;
                        payloadLen = 0;
                        for (int k = 0; k < 8; ++k) {
                            payloadLen = (payloadLen << 8) | static_cast<uint8_t>(unread[2 + k]);
                        }
                        headerSize = 10;
                    }

                    if (payloadLen > kMaxWsFrameBytes) {
                        closeClient = true;
                        break;
                    }

                    const std::size_t maskLen = masked ? 4 : 0;
                    const std::size_t payloadSize = static_cast<std::size_t>(payloadLen);
                    const std::size_t totalFrame = headerSize + maskLen + payloadSize;
                    if (unread.size() < totalFrame) {
                        break; // Trame WebSocket partielle
                    }

                    char* frameData = c.recv_buf.data() + consumePos;
                    char* payloadPtr = frameData + headerSize + maskLen;
                    if (masked && payloadSize > 0) {
                        const uint8_t m0 = static_cast<uint8_t>(frameData[headerSize + 0]);
                        const uint8_t m1 = static_cast<uint8_t>(frameData[headerSize + 1]);
                        const uint8_t m2 = static_cast<uint8_t>(frameData[headerSize + 2]);
                        const uint8_t m3 = static_cast<uint8_t>(frameData[headerSize + 3]);
                        const uint8_t mask4[4] = { m0, m1, m2, m3 };
                        std::size_t k = 0;
                        for (; k + 4 <= payloadSize; k += 4) {
                            payloadPtr[k + 0] = static_cast<char>(static_cast<uint8_t>(payloadPtr[k + 0]) ^ m0);
                            payloadPtr[k + 1] = static_cast<char>(static_cast<uint8_t>(payloadPtr[k + 1]) ^ m1);
                            payloadPtr[k + 2] = static_cast<char>(static_cast<uint8_t>(payloadPtr[k + 2]) ^ m2);
                            payloadPtr[k + 3] = static_cast<char>(static_cast<uint8_t>(payloadPtr[k + 3]) ^ m3);
                        }
                        for (; k < payloadSize; ++k) {
                            payloadPtr[k] = static_cast<char>(static_cast<uint8_t>(payloadPtr[k]) ^ mask4[k & 3]);
                        }
                    }

                    const std::string_view payloadView(payloadPtr, payloadSize);
                    consumePos += totalFrame;

                    if (opcode == 0x8) {
                        SendWsFrame(c.sock, 0x8, "");
                        closeClient = true;
                        break;
                    } else if (opcode == 0x9) {
                        SendWsFrame(c.sock, 0xA, payloadView);
                    } else if (opcode == 0xA) {
                        // Pong
                    } else if (opcode == 0x1 || opcode == 0x2 || opcode == 0x0) {
                        if (fin && opcode != 0x0 && c.ws_frag_buf.empty()) {
                            if (ParseTelemetryJson(payloadView, stagingFrame)) {
                                PublishFrame(stagingFrame, "WS");
                                ++rateWindowPackets;
                            } else {
                                recordParseError();
                            }
                        } else {
                            if (opcode != 0x0) {
                                c.ws_frag_opcode = opcode;
                                c.ws_frag_buf.clear();
                            }
                            c.ws_frag_buf.append(payloadView);
                            if (c.ws_frag_buf.size() > kMaxWsFrameBytes) {
                                closeClient = true;
                                break;
                            }
                            if (fin) {
                                if (c.ws_frag_opcode == 0x1 || c.ws_frag_opcode == 0x2) {
                                    if (ParseTelemetryJson(c.ws_frag_buf, stagingFrame)) {
                                        PublishFrame(stagingFrame, "WS");
                                        ++rateWindowPackets;
                                    } else {
                                        recordParseError();
                                    }
                                }
                                c.ws_frag_buf.clear();
                            }
                        }
                    }
                }
            }

            if (consumePos > 0) {
                if (consumePos >= c.recv_buf.size()) {
                    c.recv_buf.clear();
                } else {
                    c.recv_buf.erase(0, consumePos);
                }
            }

            if (closeClient) {
                closesocket(c.sock);
                clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(idx));
                clientsChanged = true;
            }
        }

        if (clientsChanged) {
            updateClientMetrics();
        }
    }

    for (auto& c : clients) {
        closesocket(c.sock);
    }
}

} // namespace nah2
