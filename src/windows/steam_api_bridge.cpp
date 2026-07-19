#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using HSteamPipe = std::int32_t;
using HSteamUser = std::int32_t;
using CreateInterfaceFn = void*(__cdecl*)(const char*, int*);

constexpr std::uint32_t kStandableAppId = 2370570;
constexpr std::size_t kSteamClientCreatePipe = 0;
constexpr std::size_t kSteamClientConnectGlobalUser = 2;
constexpr std::size_t kSteamClientGetGenericInterface = 12;
constexpr std::size_t kSteamClientGetApps = 15;
constexpr std::size_t kSteamAppsIsSubscribed = 0;
constexpr std::size_t kSteamAppsIsSubscribedApp = 6;

enum SteamApiInitResult : std::int32_t {
    SteamApiInitResultOk = 0,
    SteamApiInitResultFailedGeneric = 1,
    SteamApiInitResultNoSteamClient = 2,
    SteamApiInitResultVersionMismatch = 3,
};

struct ApiState {
    HMODULE steamclient{};
    void* client{};
    HSteamPipe pipe{};
    HSteamUser user{};
    bool owns_standable{};
    SteamApiInitResult result{SteamApiInitResultNoSteamClient};
    std::string error{"Steam client has not been initialized"};
};

SRWLOCK g_state_lock = SRWLOCK_INIT;
ApiState g_state;
std::atomic<std::uintptr_t> g_context_generation{1};

template <typename Function>
Function VtableMethod(void* object, std::size_t index) {
    if (object == nullptr) {
        return nullptr;
    }
    auto*** object_pointer = reinterpret_cast<void***>(object);
    if (*object_pointer == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<Function>((*object_pointer)[index]);
}

void SetError(ApiState& state, SteamApiInitResult result, const std::string& message) {
    state.result = result;
    state.error = message;
}

std::string WindowsError(const char* operation) {
    return std::string(operation) + " failed with Windows error " +
        std::to_string(::GetLastError());
}

HMODULE TryLoadAbsolute(const wchar_t* path) {
    if (path == nullptr || *path == L'\0') {
        return nullptr;
    }
    const DWORD attributes = ::GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return nullptr;
    }
    return ::LoadLibraryW(path);
}

HMODULE LoadSteamClient() {
    if (HMODULE existing = ::GetModuleHandleW(L"steamclient64.dll")) {
        return existing;
    }

    wchar_t override_path[32768]{};
    const DWORD override_length = ::GetEnvironmentVariableW(
        L"STANDABLE_STEAMCLIENT64",
        override_path,
        static_cast<DWORD>(std::size(override_path)));
    if (override_length > 0 && override_length < std::size(override_path)) {
        if (HMODULE module = TryLoadAbsolute(override_path)) {
            return module;
        }
    }

    // Proton creates these paths from the authenticated Steam client's
    // legacycompat runtime while preparing the prefix.
    constexpr const wchar_t* candidates[] = {
        L"C:\\Program Files (x86)\\Steam\\steamclient64.dll",
        L"C:\\Program Files\\Steam\\steamclient64.dll",
    };
    for (const wchar_t* candidate : candidates) {
        if (HMODULE module = TryLoadAbsolute(candidate)) {
            return module;
        }
    }

    // Keep the normal loader search as a final fallback for Wine and custom
    // Proton layouts.
    return ::LoadLibraryW(L"steamclient64.dll");
}

bool ConnectToSteam(ApiState& state) {
    state.steamclient = LoadSteamClient();
    if (state.steamclient == nullptr) {
        SetError(state, SteamApiInitResultNoSteamClient, WindowsError("LoadLibraryW(steamclient64.dll)"));
        return false;
    }

    const auto create_interface = reinterpret_cast<CreateInterfaceFn>(
        ::GetProcAddress(state.steamclient, "CreateInterface"));
    if (create_interface == nullptr) {
        SetError(state, SteamApiInitResultVersionMismatch,
            WindowsError("GetProcAddress(CreateInterface)"));
        return false;
    }

    constexpr const char* client_versions[] = {
        "SteamClient021",
        "SteamClient020",
        "SteamClient019",
    };
    for (const char* version : client_versions) {
        int return_code = 0;
        state.client = create_interface(version, &return_code);
        if (state.client != nullptr) {
            break;
        }
    }
    if (state.client == nullptr) {
        SetError(state, SteamApiInitResultVersionMismatch,
            "Valve steamclient64.dll did not expose a supported SteamClient interface");
        return false;
    }

    using CreatePipeFn = HSteamPipe(__fastcall*)(void*);
    using ConnectUserFn = HSteamUser(__fastcall*)(void*, HSteamPipe);
    const auto create_pipe = VtableMethod<CreatePipeFn>(state.client, kSteamClientCreatePipe);
    const auto connect_user = VtableMethod<ConnectUserFn>(state.client, kSteamClientConnectGlobalUser);
    if (create_pipe == nullptr || connect_user == nullptr) {
        SetError(state, SteamApiInitResultVersionMismatch,
            "SteamClient interface is missing required pipe methods");
        return false;
    }

    state.pipe = create_pipe(state.client);
    if (state.pipe == 0) {
        SetError(state, SteamApiInitResultNoSteamClient,
            "The running Steam client did not create an API pipe");
        return false;
    }
    state.user = connect_user(state.client, state.pipe);
    if (state.user == 0) {
        SetError(state, SteamApiInitResultNoSteamClient,
            "The API pipe did not connect to the signed-in Steam user");
        return false;
    }

    using GetAppsFn = void*(__fastcall*)(void*, HSteamUser, HSteamPipe, const char*);
    const auto get_apps = VtableMethod<GetAppsFn>(state.client, kSteamClientGetApps);
    void* apps = get_apps == nullptr
        ? nullptr
        : get_apps(state.client, state.user, state.pipe, "STEAMAPPS_INTERFACE_VERSION008");
    if (apps == nullptr) {
        SetError(state, SteamApiInitResultVersionMismatch,
            "SteamApps008 is unavailable from the running Steam client");
        return false;
    }

    using IsSubscribedFn = bool(__fastcall*)(void*);
    using IsSubscribedAppFn = bool(__fastcall*)(void*, std::uint32_t);
    const auto is_subscribed = VtableMethod<IsSubscribedFn>(apps, kSteamAppsIsSubscribed);
    const auto is_subscribed_app = VtableMethod<IsSubscribedAppFn>(apps, kSteamAppsIsSubscribedApp);
    if (is_subscribed == nullptr || is_subscribed_app == nullptr) {
        SetError(state, SteamApiInitResultVersionMismatch,
            "SteamApps008 is missing ownership methods");
        return false;
    }

    state.owns_standable = is_subscribed_app(apps, kStandableAppId);
    if (!state.owns_standable) {
        // BIsSubscribed checks the current SteamAppId and is retained as a
        // compatibility fallback for clients that do not answer the explicit
        // app query during their first IPC frame.
        state.owns_standable = is_subscribed(apps);
    }
    if (!state.owns_standable) {
        SetError(state, SteamApiInitResultFailedGeneric,
            "The signed-in Steam account does not report ownership of Standable (App 2370570)");
        return false;
    }

    state.result = SteamApiInitResultOk;
    state.error.clear();
    g_context_generation.fetch_add(1, std::memory_order_release);
    return true;
}

bool EnsureConnected() {
    ::AcquireSRWLockExclusive(&g_state_lock);
    if (g_state.result == SteamApiInitResultOk) {
        ::ReleaseSRWLockExclusive(&g_state_lock);
        return true;
    }

    ApiState candidate;
    const bool connected = ConnectToSteam(candidate);
    g_state = std::move(candidate);
    ::ReleaseSRWLockExclusive(&g_state_lock);
    return connected;
}

void CopyError(char* output) {
    if (output == nullptr) {
        return;
    }
    constexpr std::size_t kSteamErrorMessageCapacity = 1024;
    ::AcquireSRWLockShared(&g_state_lock);
    std::snprintf(output, kSteamErrorMessageCapacity, "%s", g_state.error.c_str());
    ::ReleaseSRWLockShared(&g_state_lock);
}

} // namespace

extern "C" __declspec(dllexport) std::int32_t __cdecl SteamInternal_SteamAPI_Init(
    const char* interface_versions,
    char* error_message) {
    if (interface_versions == nullptr) {
        ::AcquireSRWLockExclusive(&g_state_lock);
        SetError(g_state, SteamApiInitResultVersionMismatch,
            "Steam API interface version list was null");
        ::ReleaseSRWLockExclusive(&g_state_lock);
        CopyError(error_message);
        return SteamApiInitResultVersionMismatch;
    }

    if (!EnsureConnected()) {
        CopyError(error_message);
        ::AcquireSRWLockShared(&g_state_lock);
        const auto result = g_state.result;
        ::ReleaseSRWLockShared(&g_state_lock);
        return result;
    }

    if (error_message != nullptr) {
        error_message[0] = '\0';
    }
    return SteamApiInitResultOk;
}

extern "C" __declspec(dllexport) HSteamUser __cdecl SteamAPI_GetHSteamUser() {
    if (!EnsureConnected()) {
        return 0;
    }
    ::AcquireSRWLockShared(&g_state_lock);
    const HSteamUser user = g_state.user;
    ::ReleaseSRWLockShared(&g_state_lock);
    return user;
}

extern "C" __declspec(dllexport) void* __cdecl SteamInternal_FindOrCreateUserInterface(
    HSteamUser user,
    const char* version) {
    if (version == nullptr || !EnsureConnected()) {
        return nullptr;
    }

    ::AcquireSRWLockShared(&g_state_lock);
    using GetGenericFn = void*(__fastcall*)(void*, HSteamUser, HSteamPipe, const char*);
    const auto get_generic = VtableMethod<GetGenericFn>(
        g_state.client,
        kSteamClientGetGenericInterface);
    void* result = nullptr;
    if (get_generic != nullptr) {
        const HSteamUser requested_user = user == 0 ? g_state.user : user;
        result = get_generic(g_state.client, requested_user, g_state.pipe, version);
    }
    ::ReleaseSRWLockShared(&g_state_lock);
    return result;
}

extern "C" __declspec(dllexport) void* __cdecl SteamInternal_ContextInit(void* context_data) {
    if (context_data == nullptr) {
        return nullptr;
    }

    auto** slots = static_cast<void**>(context_data);
    using InitializeFn = void(__cdecl*)(void*);
    const auto initialize = reinterpret_cast<InitializeFn>(slots[0]);
    const std::uintptr_t generation = g_context_generation.load(std::memory_order_acquire);
    const std::uintptr_t cached_generation = reinterpret_cast<std::uintptr_t>(slots[1]);
    if (initialize != nullptr && (slots[2] == nullptr || cached_generation != generation)) {
        slots[2] = nullptr;
        initialize(&slots[2]);
        slots[1] = reinterpret_cast<void*>(generation);
    }
    return &slots[2];
}
