/**
 * test-ui-patch.cpp
 * KPMdriver + AImGui — Zombie Shooter (com.aldagames.zombieshooter)
 * Offline game memory info panel
 */

#include "Global.h"
#include "AImGui.h"
#include "tools/KPMdriver.h"

#include <thread>
#include <iostream>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>

// ─────────────────────────────────────────────
//  Structs
// ─────────────────────────────────────────────
struct Vector3 { float x, y, z; };

struct GameInfo {
    // Driver
    bool    driverOk        = false;
    bool    procHidden      = false;
    std::string driverType  = "Unknown";

    // Process
    int     pid             = -1;
    bool    pidFound        = false;

    // Module
    uintptr_t base          = 0;
    uintptr_t base_bss      = 0;
    uintptr_t rangeStart    = 0;
    uintptr_t rangeEnd      = 0;
    bool    moduleFound     = false;

    // Memory test
    int     readTest        = 0;
    bool    readOk          = false;

    // Maps
    std::vector<std::string> maps;
    int     mapsCount       = 0;

    // ESP Placeholder (isi offset nanti setelah dump IL2CPP)
    int     enemyCount      = 0;

    // Timing
    std::string lastRefresh = "--:--:--";
};

// ─────────────────────────────────────────────
//  Globals
// ─────────────────────────────────────────────
static GameInfo     g_info;
static std::mutex   g_mutex;
static std::atomic<bool> g_running      { true  };
static std::atomic<bool> g_refreshing   { false };
static std::atomic<bool> g_driverInited { false };
static std::atomic<bool> g_procHidden   { false };

// ─────────────────────────────────────────────
//  Helper — timestamp string
// ─────────────────────────────────────────────
static std::string NowTime() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm* tm = std::localtime(&t);
    std::ostringstream ss;
    ss << std::setfill('0')
       << std::setw(2) << tm->tm_hour << ":"
       << std::setw(2) << tm->tm_min  << ":"
       << std::setw(2) << tm->tm_sec;
    return ss.str();
}

// ─────────────────────────────────────────────
//  Helper — hex string
// ─────────────────────────────────────────────
static std::string ToHex(uintptr_t v) {
    if (v == 0) return "0x0";
    std::ostringstream ss;
    ss << "0x" << std::hex << std::uppercase << v;
    return ss.str();
}

// ─────────────────────────────────────────────
//  Background thread — refresh game info
//  Hanya baca data game. Driver init & hideProc
//  dilakukan SEKALI di main(), bukan di sini.
// ─────────────────────────────────────────────
static void RefreshInfo() {
    if (!g_driverInited.load()) return;
    g_refreshing = true;
    GameInfo tmp;

    // Pertahankan status driver dari sesi sebelumnya
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        tmp.driverOk   = g_info.driverOk;
        tmp.driverType = g_info.driverType;
        tmp.procHidden = g_procHidden.load();
    }

    // --- PID ---
    auto pidOpt = Core.get_pid("com.aldagames.zombieshooter");
    if (pidOpt) {
        tmp.pid      = *pidOpt;
        tmp.pidFound = true;
        Core.init_pid(tmp.pid);

        // --- Module base ---
        auto baseOpt = Core.get_base(tmp.pid, "libil2cpp.so");
        auto bssOpt  = Core.get_base(tmp.pid, "libil2cpp.so", true);
        auto rangeOpt= Core.get_base_range(tmp.pid, "libil2cpp.so");

        if (baseOpt) {
            tmp.base        = *baseOpt;
            tmp.moduleFound = true;
        }
        if (bssOpt)   tmp.base_bss   = *bssOpt;
        if (rangeOpt) {
            tmp.rangeStart = rangeOpt->first;
            tmp.rangeEnd   = rangeOpt->second;
        }

        // --- Read test (first 4 bytes of .so) ---
        if (tmp.base) {
            tmp.readTest = Core.read<int>(tmp.base);
            tmp.readOk   = (tmp.readTest != 0);
        }

        // --- Maps (limit 200 entries for display) ---
        auto mapsOpt = Core.getMaps(tmp.pid);
        if (mapsOpt) {
            tmp.maps      = *mapsOpt;
            tmp.mapsCount = (int)tmp.maps.size();
        }

        // --- ESP placeholder: enemy count ---
        // TODO: isi offset setelah dump IL2CPP
        // uintptr_t gm   = Core.read<uintptr_t>(tmp.base + OFF_GMANAGER);
        // uintptr_t list = Core.read<uintptr_t>(gm + OFF_ENEMYLIST);
        // tmp.enemyCount = Core.read<int>(list + 0x18);
        tmp.enemyCount = 0; // placeholder
    }

    tmp.lastRefresh = NowTime();

    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_info = tmp;
    }
    g_refreshing = false;
}

// ─────────────────────────────────────────────
//  ImGui helpers
// ─────────────────────────────────────────────
static void StatusBadge(bool ok, const char* labelOk, const char* labelFail) {
    if (ok) {
        ImGui::TextColored({0.2f,1.0f,0.2f,1.0f}, "[OK] %s", labelOk);
    } else {
        ImGui::TextColored({1.0f,0.3f,0.3f,1.0f}, "[!!] %s", labelFail);
    }
}

static void SectionHeader(const char* label) {
    ImGui::Spacing();
    ImGui::TextColored({1.0f,0.8f,0.0f,1.0f}, "── %s ──", label);
    ImGui::Separator();
}

// ─────────────────────────────────────────────
//  Main
// ─────────────────────────────────────────────
int main()
{
    android::AImGui imgui(android::AImGui::Options{
        .renderType            = android::AImGui::RenderType::RenderNative,
        .autoUpdateOrientation = true
    });

    if (!imgui) {
        LogInfo("[-] ImGui initialization failed");
        return 0;
    }

    // ── Driver init — SEKALI saja di sini ───────────────
    Core.start();
    {
        std::lock_guard<std::mutex> lk(g_mutex);
#if defined(DITPRO_KPM)
        g_info.driverType = "DITPRO_KPM (syscall)";
#elif defined(DITS_KO)
        g_info.driverType = "DITS_KO (ioctl)";
#else
        g_info.driverType = "Auto-detect";
#endif
        g_info.driverOk = true;

        // hideProc dipanggil SEKALI — kalau dipanggil ulang tiap refresh
        // bisa corrupt state KPM dan menyebabkan reboot saat exit
        bool hidden = (Core.hideProc() == 0);
        g_procHidden.store(hidden);
        g_info.procHidden = hidden;
    }
    g_driverInited.store(true);

    bool state           = true;
    bool showMaps        = false;
    bool showAbout       = false;
    bool autoRefresh     = false;
    int  refreshInterval = 3;  // seconds
    auto lastAutoRefresh = std::chrono::steady_clock::now();

    // Input event thread
    std::thread inputThread([&]{
        while (g_running) {
            imgui.ProcessInputEvent();
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    });

    // Initial refresh
    std::thread(RefreshInfo).detach();

    // ── Render loop ──────────────────────────
    while (state)
    {
        imgui.BeginFrame();

        // Auto-refresh
        if (autoRefresh && !g_refreshing) {
            auto now = std::chrono::steady_clock::now();
            float elapsed = std::chrono::duration<float>(now - lastAutoRefresh).count();
            if (elapsed >= refreshInterval) {
                lastAutoRefresh = now;
                std::thread(RefreshInfo).detach();
            }
        }

        // ═══════════════════════════════════════
        //  Window: Main Panel
        // ═══════════════════════════════════════
        ImGui::SetNextWindowSize({460, 560}, ImGuiCond_Once);
        ImGui::SetNextWindowPos({10, 10},    ImGuiCond_Once);
        ImGui::Begin("[ Zombie Shooter — KPM Info Panel ]", &state,
            ImGuiWindowFlags_NoScrollbar);

        GameInfo info;
        { std::lock_guard<std::mutex> lk(g_mutex); info = g_info; }

        // ── Section: Driver ─────────────────────
        SectionHeader("DRIVER");
        ImGui::Text("Type   : %s", info.driverType.c_str());
        StatusBadge(info.driverOk,   "Driver OK",          "Driver NOT found");
        StatusBadge(info.procHidden, "Process Hidden",     "Hide process FAILED");

        // ── Section: Process ────────────────────
        SectionHeader("PROCESS");
        ImGui::Text("Package: com.aldagames.zombieshooter");
        if (info.pidFound)
            ImGui::TextColored({0.2f,1.0f,0.2f,1.0f}, "PID    : %d", info.pid);
        else
            ImGui::TextColored({1.0f,0.3f,0.3f,1.0f}, "PID    : NOT FOUND (game running?)");

        // ── Section: Module ─────────────────────
        SectionHeader("MODULE — libil2cpp.so");
        if (info.moduleFound) {
            ImGui::Text("Base      : %s", ToHex(info.base).c_str());
            ImGui::Text("Base BSS  : %s", ToHex(info.base_bss).c_str());
            ImGui::Text("Range     : %s — %s",
                ToHex(info.rangeStart).c_str(),
                ToHex(info.rangeEnd).c_str());
            ImGui::Text("Size      : %.2f MB",
                (float)(info.rangeEnd - info.rangeStart) / (1024.f * 1024.f));
        } else {
            ImGui::TextColored({1.0f,0.3f,0.3f,1.0f}, "libil2cpp.so not found");
        }

        // ── Section: Memory Read Test ───────────
        SectionHeader("MEMORY READ TEST");
        StatusBadge(info.readOk, "Read OK", "Read FAILED / zero");
        ImGui::Text("Base[0] read (int): %d  (0x%X)", info.readTest, info.readTest);

        // ── Section: ESP Placeholder ────────────
        SectionHeader("ESP — ENEMY DATA (placeholder)");
        ImGui::TextColored({0.6f,0.6f,0.6f,1.0f},
            "Offset IL2CPP belum diset — isi setelah dump");
        ImGui::Text("Enemy count : %d", info.enemyCount);
        ImGui::Text("TODO: OFF_GMANAGER, OFF_ENEMYLIST, OFF_TRANSFORM");

        // ── Section: Memory Maps ────────────────
        SectionHeader("MEMORY MAPS");
        ImGui::Text("Total entries: %d", info.mapsCount);
        if (info.mapsCount > 0) {
            if (ImGui::Button(showMaps ? "Hide Maps" : "Show Maps"))
                showMaps = !showMaps;
        }

        // ── Section: Controls ───────────────────
        SectionHeader("CONTROLS");
        ImGui::Text("Last refresh: %s", info.lastRefresh.c_str());

        if (g_refreshing) {
            ImGui::TextColored({1.0f,1.0f,0.0f,1.0f}, "  Refreshing...");
        } else {
            if (ImGui::Button("Refresh Now"))
                std::thread(RefreshInfo).detach();
        }

        ImGui::SameLine();
        ImGui::Checkbox("Auto", &autoRefresh);
        if (autoRefresh) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::SliderInt("sec", &refreshInterval, 1, 10);
        }

        ImGui::SameLine();
        if (ImGui::Button("About")) showAbout = !showAbout;

        ImGui::Spacing();
        ImGui::Text("FPS: %.1f  |  %.3f ms/frame",
            ImGui::GetIO().Framerate,
            1000.0f / ImGui::GetIO().Framerate);

        ImGui::End();

        // ═══════════════════════════════════════
        //  Window: Maps Viewer
        // ═══════════════════════════════════════
        if (showMaps) {
            ImGui::SetNextWindowSize({520, 400}, ImGuiCond_Once);
            ImGui::SetNextWindowPos({20, 30}, ImGuiCond_Once);
            ImGui::Begin("Memory Maps", &showMaps);
            ImGui::Text("Entries: %d", (int)info.maps.size());
            ImGui::Separator();
            ImGui::BeginChild("MapsScroll", {0, 0}, false, ImGuiWindowFlags_HorizontalScrollbar);
            for (const auto& line : info.maps) {
                // Color-code by permission
                if (line.find("r-x") != std::string::npos)
                    ImGui::TextColored({0.4f,1.0f,0.4f,1.0f}, "%s", line.c_str()); // exec
                else if (line.find("rw-") != std::string::npos)
                    ImGui::TextColored({0.4f,0.8f,1.0f,1.0f}, "%s", line.c_str()); // rw
                else
                    ImGui::TextDisabled("%s", line.c_str());
            }
            ImGui::EndChild();
            ImGui::End();
        }

        // ═══════════════════════════════════════
        //  Window: About
        // ═══════════════════════════════════════
        if (showAbout) {
            ImGui::SetNextWindowSize({300, 160}, ImGuiCond_Always);
            ImGui::Begin("About", &showAbout, ImGuiWindowFlags_NoResize);
            ImGui::Text("test-ui-patch");
            ImGui::Text("Target  : com.aldagames.zombieshooter");
            ImGui::Text("Engine  : Unity IL2CPP (arm64)");
            ImGui::Text("Driver  : KPMdriver (DITPRO / DITS)");
            ImGui::Separator();
            ImGui::TextColored({0.6f,0.6f,0.6f,1.0f},
                "Offline game — personal mod only");
            ImGui::End();
        }

        imgui.EndFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // ── Cleanup ──────────────────────────────────────────
    g_running = false;

    // Tunggu RefreshInfo selesai dulu.
    // Kalau langsung UnMem saat thread masih akses Core → kernel panic / reboot.
    while (g_refreshing.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (inputThread.joinable()) inputThread.join();

    // Order WAJIB: unProc() DULU, baru UnMem().
    // Kalau UnMem() dipanggil dulu → driver uninstall → unProc() gagal
    // → process tetap hidden → kernel tidak bisa track process → reboot.
    if (g_procHidden.load()) {
        Core.unProc();
        g_procHidden.store(false);
    }
    Core.UnMem();
    // Destructor ~driver() akan memanggil UnMem()+unProc() lagi,
    // tapi karena driver sudah uninstall, call tersebut return -1 (tidak bahaya).

    return 0;
}
