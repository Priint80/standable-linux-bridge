#include <dlfcn.h>

#include <cstdlib>
#include <iostream>

namespace {

using DriverFactory = void* (*)(const char*, int*);
constexpr int kInterfaceNotFound = 105;

[[noreturn]] void Fail(const char* message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " /path/to/driver_standable.so\n";
        return EXIT_FAILURE;
    }

    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        std::cerr << "FAIL: dlopen: " << dlerror() << '\n';
        return EXIT_FAILURE;
    }

    dlerror();
    auto* factory = reinterpret_cast<DriverFactory>(dlsym(library, "HmdDriverFactory"));
    if (const char* error = dlerror(); error != nullptr) {
        std::cerr << "FAIL: dlsym: " << error << '\n';
        dlclose(library);
        return EXIT_FAILURE;
    }

    int return_code = 0;
    if (factory("IServerTrackedDeviceProvider_004", &return_code) == nullptr) {
        dlclose(library);
        Fail("factory rejected IServerTrackedDeviceProvider_004");
    }

    return_code = 0;
    if (factory("NotARealOpenVRInterface_001", &return_code) != nullptr) {
        dlclose(library);
        Fail("factory accepted an unknown interface");
    }
    if (return_code != kInterfaceNotFound) {
        dlclose(library);
        Fail("factory returned the wrong error for an unknown interface");
    }

    if (dlclose(library) != 0) {
        Fail("dlclose failed");
    }

    std::cout << "PASS: Linux ELF, exported factory, and OpenVR interface negotiation\n";
    return EXIT_SUCCESS;
}
