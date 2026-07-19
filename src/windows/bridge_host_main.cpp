#include "fake_openvr.hpp"
#include "windows_transport.hpp"

#include <openvr_driver.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include <windows.h>

namespace {

using DriverFactory = void*(__cdecl*)(const char*, int*);

struct Options {
    std::uint64_t session{};
    std::uint16_t native_port{standable::bridge::kDefaultNativePort};
    std::uint16_t helper_port{standable::bridge::kDefaultHelperPort};
};

bool ParseUnsigned(const char* text, std::uint64_t& value) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    const auto parsed = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    value = parsed;
    return true;
}

bool ParseOptions(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--session") == 0 && index + 1 < argc) {
            if (!ParseUnsigned(argv[++index], options.session)) {
                return false;
            }
        } else if (std::strcmp(argv[index], "--native-port") == 0 && index + 1 < argc) {
            std::uint64_t port = 0;
            if (!ParseUnsigned(argv[++index], port) || port == 0 || port > 65535) {
                return false;
            }
            options.native_port = static_cast<std::uint16_t>(port);
        } else if (std::strcmp(argv[index], "--helper-port") == 0 && index + 1 < argc) {
            std::uint64_t port = 0;
            if (!ParseUnsigned(argv[++index], port) || port == 0 || port > 65535) {
                return false;
            }
            options.helper_port = static_cast<std::uint16_t>(port);
        } else {
            return false;
        }
    }
    return options.session != 0;
}

std::filesystem::path DriverRoot() {
    std::wstring executable(32768, L'\0');
    const DWORD length = ::GetModuleFileNameW(
        nullptr,
        executable.data(),
        static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) {
        return {};
    }
    executable.resize(length);
    const std::filesystem::path path(executable);
    // <driver-root>/bin/win64/standable_bridge_host.exe
    return path.parent_path().parent_path().parent_path();
}

std::string WindowsError(const char* operation) {
    return std::string(operation) + " failed with Windows error " + std::to_string(::GetLastError());
}

} // namespace

int main(int argc, char** argv) {
    Options options{};
    if (!ParseOptions(argc, argv, options)) {
        std::fprintf(stderr,
            "usage: standable_bridge_host.exe --session N --native-port N --helper-port N\n");
        return 2;
    }

    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const auto root = DriverRoot();
    if (root.empty()) {
        std::fprintf(stderr, "Could not determine the Standable driver root\n");
        return 3;
    }

    standable::windows::WindowsTransport transport;
    if (!transport.Open(options.helper_port, options.native_port, options.session)) {
        std::fprintf(stderr, "Could not bind the helper bridge socket\n");
        return 4;
    }

    standable::windows::OpenVrHost host(transport, root);
    host.SendHello();
    host.SendProviderStatus(
        standable::bridge::ProviderState::Starting,
        vr::VRInitError_None,
        "Loading original Standable 3.0.3 provider");

    // Prime raw poses and properties before the original provider asks for
    // them during Init. Physical callbacks remain gated until its hooks exist.
    const ULONGLONG prime_deadline = ::GetTickCount64() + 250;
    while (::GetTickCount64() < prime_deadline && !host.ShouldExit()) {
        standable::windows::ReceivedPacket packet{};
        while (transport.Receive(packet)) {
            host.HandlePacket(packet);
        }
        ::Sleep(2);
    }
    if (host.ShouldExit()) {
        return 0;
    }

    // Load the bridge-owned Steam API compatibility layer from beside this
    // executable. It forwards into Proton's authenticated steamclient64.dll
    // and refuses initialization when App 2370570 is not owned.
    const auto steam_api_path = root / "bin" / "win64" / "steam_api64.dll";
    HMODULE steam_api = ::LoadLibraryW(steam_api_path.c_str());
    if (steam_api == nullptr) {
        const auto detail = WindowsError("LoadLibraryW(bridge steam_api64.dll)");
        host.Log(detail.c_str());
        host.SendProviderStatus(
            standable::bridge::ProviderState::DriverLoadFailed,
            vr::VRInitError_Driver_Failed,
            detail.c_str());
        ::Sleep(1000);
        return 5;
    }

    const auto driver_path = root / "bin" / "win64" / "driver_standable.dll";
    HMODULE driver_module = ::LoadLibraryW(driver_path.c_str());
    if (driver_module == nullptr) {
        const auto detail = WindowsError("LoadLibraryW(driver_standable.dll)");
        host.Log(detail.c_str());
        host.SendProviderStatus(
            standable::bridge::ProviderState::DriverLoadFailed,
            vr::VRInitError_Driver_Failed,
            detail.c_str());
        ::FreeLibrary(steam_api);
        ::Sleep(1000);
        return 6;
    }

    auto* factory = reinterpret_cast<DriverFactory>(
        ::GetProcAddress(driver_module, "HmdDriverFactory"));
    if (factory == nullptr) {
        const auto detail = WindowsError("GetProcAddress(HmdDriverFactory)");
        host.Log(detail.c_str());
        host.SendProviderStatus(
            standable::bridge::ProviderState::DriverLoadFailed,
            vr::VRInitError_Init_InterfaceNotFound,
            detail.c_str());
        ::FreeLibrary(driver_module);
        ::FreeLibrary(steam_api);
        ::Sleep(1000);
        return 7;
    }

    int factory_error = vr::VRInitError_None;
    auto* provider = static_cast<vr::IServerTrackedDeviceProvider*>(
        factory(vr::IServerTrackedDeviceProvider_Version, &factory_error));
    if (provider == nullptr) {
        const std::string detail = "Original factory rejected IServerTrackedDeviceProvider_004";
        host.Log(detail.c_str());
        host.SendProviderStatus(
            standable::bridge::ProviderState::DriverLoadFailed,
            factory_error,
            detail.c_str());
        ::FreeLibrary(driver_module);
        ::FreeLibrary(steam_api);
        ::Sleep(1000);
        return 8;
    }

    const auto init_error = provider->Init(static_cast<vr::IVRDriverContext*>(&host));
    if (init_error != vr::VRInitError_None) {
        const std::string detail = "Original Standable provider Init returned " +
            std::to_string(static_cast<int>(init_error));
        host.Log(detail.c_str());
        host.SendProviderStatus(
            standable::bridge::ProviderState::ProviderInitFailed,
            init_error,
            detail.c_str());
        provider->Cleanup();
        ::FreeLibrary(driver_module);
        ::FreeLibrary(steam_api);
        ::Sleep(1000);
        return 9;
    }

    host.SendProviderStatus(
        standable::bridge::ProviderState::Ready,
        vr::VRInitError_None,
        "Original Standable provider initialized");
    host.Log("Original Standable provider initialized successfully");
    host.EnablePhysicalInjection();

    ULONGLONG last_native_packet = ::GetTickCount64();
    ULONGLONG last_heartbeat = 0;
    while (!host.ShouldExit()) {
        standable::windows::ReceivedPacket packet{};
        bool received_any = false;
        for (std::size_t count = 0; count < 512 && transport.Receive(packet); ++count) {
            received_any = true;
            host.HandlePacket(packet);
        }
        if (received_any) {
            last_native_packet = ::GetTickCount64();
        }

        provider->RunFrame();

        const ULONGLONG now = ::GetTickCount64();
        if (now - last_heartbeat >= 250) {
            host.SendHeartbeat();
            last_heartbeat = now;
        }
        if (now - last_native_packet >= 10000) {
            host.Log("Native SteamVR driver timed out; shutting down helper");
            break;
        }
        ::Sleep(5);
    }

    host.SendProviderStatus(
        standable::bridge::ProviderState::Stopping,
        vr::VRInitError_None,
        "Bridge helper stopping");
    provider->Cleanup();
    ::FreeLibrary(driver_module);
    ::FreeLibrary(steam_api);
    return 0;
}
