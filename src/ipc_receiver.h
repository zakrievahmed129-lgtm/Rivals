#pragma once

#include "telemetry_types.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace nah2 {

// Parse une trame JSON de telemetrie Roblox vers TelemetryFrame.
// Retourne true si le JSON est valide et contient au moins un bloc "cam".
bool ParseTelemetryJson(std::string_view json, TelemetryFrame& outFrame);

// Serveur IPC local multi-protocole (HTTP/1.1 Keep-Alive, WebSocket RFC 6455, UDP)
// tournant dans un thread dedie avec echange par double buffer sans blocage du rendu.
class IpcReceiver {
public:
    explicit IpcReceiver(uint16_t port = 27015);
    ~IpcReceiver();

    IpcReceiver(const IpcReceiver&) = delete;
    IpcReceiver& operator=(const IpcReceiver&) = delete;

    // Demarre les sockets TCP/UDP sur 127.0.0.1:port et lance le thread reseau.
    // Si port == 0, l'OS choisit un port libre consultable via GetBoundPort().
    bool Start();
    void Stop();

    bool IsRunning() const { return running_.load(std::memory_order_relaxed); }
    uint16_t GetBoundPort() const { return bound_port_.load(std::memory_order_relaxed); }

    // Echange O(1) non-bloquant cote thread de rendu : recupere la derniere trame si nouvelle.
    bool ConsumeLatestFrame(TelemetryFrame& outFrame);

    // Statistiques reseau thread-safe pour le HUD diagnostic.
    IpcStats GetStats() const;

    // Evenement Win32 manual-reset signale des qu'une nouvelle trame non lue est disponible.
    void* GetFrameEvent() const { return frame_event_; }

    static uint64_t NowMicroseconds();

private:
    void ServerLoop();
    void PublishFrame(TelemetryFrame& parsedFrame, const char* transportTag);

    uint16_t requested_port_ = 27015;
    std::atomic<uint16_t> bound_port_{ 0 };
    std::atomic<bool> running_{ false };
    std::thread worker_thread_;

    // Double buffer protege par mutex ultra-court (uniquement std::swap de vecteurs pre-alloues)
    mutable std::mutex frame_mutex_;
    TelemetryFrame published_frame_{};
    bool has_unread_frame_ = false;
    IpcStats stats_{};

    // Etat interne des sockets et synchronisation (opaque ici, manipule dans ipc_receiver.cpp)
    void* frame_event_ = nullptr;
    uintptr_t tcp_listen_sock_ = static_cast<uintptr_t>(~0ULL);
    uintptr_t udp_sock_ = static_cast<uintptr_t>(~0ULL);
};

} // namespace nah2
