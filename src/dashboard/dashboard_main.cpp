#include <openvr.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <csignal>
#include <cerrno>
#include <sys/types.h>
#include <unistd.h>

namespace vr {

// This is Valve's small internal bootstrap interface from src/ivrclientcore.h.
// Loading it directly lets the companion use SteamVR's native vrclient.so
// without bundling a second copy of libopenvr_api.
class IVRClientCore {
public:
    virtual EVRInitError Init(EVRApplicationType application_type, const char* startup_info) = 0;
    virtual void Cleanup() = 0;
    virtual EVRInitError IsInterfaceVersionValid(const char* interface_version) = 0;
    virtual void* GetGenericInterface(const char* name_and_version, EVRInitError* error) = 0;
    virtual bool BIsHmdPresent() = 0;
    virtual const char* GetEnglishStringForHmdError(EVRInitError error) = 0;
    virtual const char* GetIDForVRInitError(EVRInitError error) = 0;
};

inline constexpr const char* kClientCoreVersion = "IVRClientCore_003";

}  // namespace vr

namespace x11 {

struct Display;
struct Visual;
struct Screen;

using XID = unsigned long;
using Window = XID;
using Drawable = XID;
using Pixmap = XID;
using Atom = XID;
using Colormap = XID;
using Time = unsigned long;
using Bool = int;
using Status = int;

struct XWindowAttributes {
    int x;
    int y;
    int width;
    int height;
    int border_width;
    int depth;
    Visual* visual;
    Window root;
    int c_class;
    int bit_gravity;
    int win_gravity;
    int backing_store;
    unsigned long backing_planes;
    unsigned long backing_pixel;
    Bool save_under;
    Colormap colormap;
    Bool map_installed;
    int map_state;
    long all_event_masks;
    long your_event_mask;
    long do_not_propagate_mask;
    Bool override_redirect;
    Screen* screen;
};

struct XClassHint {
    char* res_name;
    char* res_class;
};

// The fields used by the bridge are the ABI-stable prefix of XImage.
struct XImage {
    int width;
    int height;
    int xoffset;
    int format;
    char* data;
    int byte_order;
    int bitmap_unit;
    int bitmap_bit_order;
    int bitmap_pad;
    int depth;
    int bytes_per_line;
    int bits_per_pixel;
    unsigned long red_mask;
    unsigned long green_mask;
    unsigned long blue_mask;
};

struct XErrorEvent {
    int type;
    Display* display;
    unsigned long resource_id;
    unsigned long serial;
    unsigned char error_code;
    unsigned char request_code;
    unsigned char minor_code;
};

struct XMotionEvent {
    int type;
    unsigned long serial;
    Bool send_event;
    Display* display;
    Window window;
    Window root;
    Window subwindow;
    Time time;
    int x;
    int y;
    int x_root;
    int y_root;
    unsigned int state;
    char is_hint;
    Bool same_screen;
};

struct XButtonEvent {
    int type;
    unsigned long serial;
    Bool send_event;
    Display* display;
    Window window;
    Window root;
    Window subwindow;
    Time time;
    int x;
    int y;
    int x_root;
    int y_root;
    unsigned int state;
    unsigned int button;
    Bool same_screen;
};

union XEvent {
    int type;
    XMotionEvent motion;
    XButtonEvent button;
    long padding[24];
};

using ErrorHandler = int (*)(Display*, XErrorEvent*);

inline constexpr Bool False = 0;
inline constexpr Bool True = 1;
inline constexpr int Success = 0;
inline constexpr int IsViewable = 2;
inline constexpr int ZPixmap = 2;
inline constexpr unsigned long AllPlanes = ~0UL;
inline constexpr Atom AnyPropertyType = 0;
inline constexpr int MotionNotify = 6;
inline constexpr int ButtonPress = 4;
inline constexpr int ButtonRelease = 5;
inline constexpr long ButtonPressMask = 1L << 2;
inline constexpr long ButtonReleaseMask = 1L << 3;
inline constexpr long PointerMotionMask = 1L << 6;
inline constexpr unsigned int Button1Mask = 1U << 8;
inline constexpr unsigned int Button2Mask = 1U << 9;
inline constexpr unsigned int Button3Mask = 1U << 10;
inline constexpr int CompositeRedirectAutomatic = 0;

}  // namespace x11

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_running{true};
std::atomic<unsigned int> g_x_error_count{0};

void handle_signal(int) {
    g_running.store(false);
}

int ignore_x_error(x11::Display*, x11::XErrorEvent*) {
    g_x_error_count.fetch_add(1U);
    return 0;
}

class SharedLibrary {
public:
    SharedLibrary() = default;
    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;

    SharedLibrary(SharedLibrary&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    SharedLibrary& operator=(SharedLibrary&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    ~SharedLibrary() {
        reset();
    }

    bool open(const std::filesystem::path& path, int flags = RTLD_NOW | RTLD_LOCAL) {
        reset();
        handle_ = dlopen(path.c_str(), flags);
        return handle_ != nullptr;
    }

    bool open(const char* soname, int flags = RTLD_NOW | RTLD_LOCAL) {
        reset();
        handle_ = dlopen(soname, flags);
        return handle_ != nullptr;
    }

    [[nodiscard]] const char* error() const {
        const char* message = dlerror();
        return message != nullptr ? message : "unknown dynamic loader error";
    }

    template <typename Function>
    [[nodiscard]] Function symbol(const char* name) const {
        static_assert(sizeof(Function) == sizeof(void*));
        void* raw = dlsym(handle_, name);
        Function function{};
        std::memcpy(&function, &raw, sizeof(function));
        return function;
    }

    [[nodiscard]] explicit operator bool() const {
        return handle_ != nullptr;
    }

private:
    void reset() {
        if (handle_ != nullptr) {
            dlclose(handle_);
            handle_ = nullptr;
        }
    }

    void* handle_{nullptr};
};

struct X11Api {
    using OpenDisplayFn = x11::Display* (*)(const char*);
    using CloseDisplayFn = int (*)(x11::Display*);
    using DefaultScreenFn = int (*)(x11::Display*);
    using RootWindowFn = x11::Window (*)(x11::Display*, int);
    using QueryTreeFn = x11::Status (*)(x11::Display*, x11::Window, x11::Window*, x11::Window*, x11::Window**, unsigned int*);
    using GetWindowAttributesFn = x11::Status (*)(x11::Display*, x11::Window, x11::XWindowAttributes*);
    using FetchNameFn = x11::Status (*)(x11::Display*, x11::Window, char**);
    using GetClassHintFn = x11::Status (*)(x11::Display*, x11::Window, x11::XClassHint*);
    using InternAtomFn = x11::Atom (*)(x11::Display*, const char*, x11::Bool);
    using GetWindowPropertyFn = int (*)(x11::Display*, x11::Window, x11::Atom, long, long, x11::Bool,
                                       x11::Atom, x11::Atom*, int*, unsigned long*, unsigned long*, unsigned char**);
    using FreeFn = int (*)(void*);
    using GetImageFn = x11::XImage* (*)(x11::Display*, x11::Drawable, int, int, unsigned int, unsigned int,
                                       unsigned long, int);
    using DestroyImageFn = int (*)(x11::XImage*);
    using GetPixelFn = unsigned long (*)(x11::XImage*, int, int);
    using FreePixmapFn = int (*)(x11::Display*, x11::Pixmap);
    using SendEventFn = x11::Status (*)(x11::Display*, x11::Window, x11::Bool, long, x11::XEvent*);
    using FlushFn = int (*)(x11::Display*);
    using SyncFn = int (*)(x11::Display*, x11::Bool);
    using TranslateCoordinatesFn = x11::Bool (*)(x11::Display*, x11::Window, x11::Window, int, int, int*, int*, x11::Window*);
    using SetErrorHandlerFn = x11::ErrorHandler (*)(x11::ErrorHandler);
    using CompositeQueryExtensionFn = x11::Bool (*)(x11::Display*, int*, int*);
    using CompositeRedirectWindowFn = void (*)(x11::Display*, x11::Window, int);
    using CompositeUnredirectWindowFn = void (*)(x11::Display*, x11::Window, int);
    using CompositeNameWindowPixmapFn = x11::Pixmap (*)(x11::Display*, x11::Window);

    bool load() {
        if (!xlib.open("libX11.so.6")) {
            std::cerr << "dashboard: cannot load libX11.so.6: " << xlib.error() << '\n';
            return false;
        }

        open_display = xlib.symbol<OpenDisplayFn>("XOpenDisplay");
        close_display = xlib.symbol<CloseDisplayFn>("XCloseDisplay");
        default_screen = xlib.symbol<DefaultScreenFn>("XDefaultScreen");
        root_window = xlib.symbol<RootWindowFn>("XRootWindow");
        query_tree = xlib.symbol<QueryTreeFn>("XQueryTree");
        get_window_attributes = xlib.symbol<GetWindowAttributesFn>("XGetWindowAttributes");
        fetch_name = xlib.symbol<FetchNameFn>("XFetchName");
        get_class_hint = xlib.symbol<GetClassHintFn>("XGetClassHint");
        intern_atom = xlib.symbol<InternAtomFn>("XInternAtom");
        get_window_property = xlib.symbol<GetWindowPropertyFn>("XGetWindowProperty");
        free_memory = xlib.symbol<FreeFn>("XFree");
        get_image = xlib.symbol<GetImageFn>("XGetImage");
        destroy_image = xlib.symbol<DestroyImageFn>("XDestroyImage");
        get_pixel = xlib.symbol<GetPixelFn>("XGetPixel");
        free_pixmap = xlib.symbol<FreePixmapFn>("XFreePixmap");
        send_event = xlib.symbol<SendEventFn>("XSendEvent");
        flush = xlib.symbol<FlushFn>("XFlush");
        sync = xlib.symbol<SyncFn>("XSync");
        translate_coordinates = xlib.symbol<TranslateCoordinatesFn>("XTranslateCoordinates");
        set_error_handler = xlib.symbol<SetErrorHandlerFn>("XSetErrorHandler");

        const bool required = open_display != nullptr && close_display != nullptr && default_screen != nullptr &&
                              root_window != nullptr && query_tree != nullptr && get_window_attributes != nullptr &&
                              fetch_name != nullptr && get_class_hint != nullptr && intern_atom != nullptr &&
                              get_window_property != nullptr && free_memory != nullptr && get_image != nullptr &&
                              destroy_image != nullptr && get_pixel != nullptr && free_pixmap != nullptr &&
                              send_event != nullptr && flush != nullptr && sync != nullptr &&
                              translate_coordinates != nullptr && set_error_handler != nullptr;
        if (!required) {
            std::cerr << "dashboard: libX11 is missing one or more required symbols\n";
            return false;
        }

        if (composite.open("libXcomposite.so.1")) {
            composite_query_extension = composite.symbol<CompositeQueryExtensionFn>("XCompositeQueryExtension");
            composite_redirect_window = composite.symbol<CompositeRedirectWindowFn>("XCompositeRedirectWindow");
            composite_unredirect_window = composite.symbol<CompositeUnredirectWindowFn>("XCompositeUnredirectWindow");
            composite_name_window_pixmap = composite.symbol<CompositeNameWindowPixmapFn>("XCompositeNameWindowPixmap");
        }
        return true;
    }

    [[nodiscard]] bool has_composite() const {
        return composite_query_extension != nullptr && composite_redirect_window != nullptr &&
               composite_unredirect_window != nullptr && composite_name_window_pixmap != nullptr;
    }

    SharedLibrary xlib;
    SharedLibrary composite;
    OpenDisplayFn open_display{};
    CloseDisplayFn close_display{};
    DefaultScreenFn default_screen{};
    RootWindowFn root_window{};
    QueryTreeFn query_tree{};
    GetWindowAttributesFn get_window_attributes{};
    FetchNameFn fetch_name{};
    GetClassHintFn get_class_hint{};
    InternAtomFn intern_atom{};
    GetWindowPropertyFn get_window_property{};
    FreeFn free_memory{};
    GetImageFn get_image{};
    DestroyImageFn destroy_image{};
    GetPixelFn get_pixel{};
    FreePixmapFn free_pixmap{};
    SendEventFn send_event{};
    FlushFn flush{};
    SyncFn sync{};
    TranslateCoordinatesFn translate_coordinates{};
    SetErrorHandlerFn set_error_handler{};
    CompositeQueryExtensionFn composite_query_extension{};
    CompositeRedirectWindowFn composite_redirect_window{};
    CompositeUnredirectWindowFn composite_unredirect_window{};
    CompositeNameWindowPixmapFn composite_name_window_pixmap{};
};

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        if (character >= static_cast<unsigned char>('A') && character <= static_cast<unsigned char>('Z')) {
            return static_cast<char>(character - static_cast<unsigned char>('A') + static_cast<unsigned char>('a'));
        }
        return static_cast<char>(character);
    });
    return value;
}

[[nodiscard]] bool is_bridge_helper_window(std::string_view title, std::string_view class_name) {
    const std::string identity = lowercase(std::string(title) + "\n" + std::string(class_name));
    return identity.find("standable_bridge_host") != std::string::npos ||
           identity.find("standable-linux-bridge") != std::string::npos ||
           identity.find("standable_dashboard_overlay") != std::string::npos;
}

[[nodiscard]] std::uint8_t channel_from_pixel(unsigned long pixel, unsigned long mask) {
    if (mask == 0UL) {
        return 0U;
    }
    const unsigned int shift = std::countr_zero(mask);
    const unsigned long maximum = mask >> shift;
    const unsigned long value = (pixel & mask) >> shift;
    if (maximum == 0UL) {
        return 0U;
    }
    return static_cast<std::uint8_t>((value * 255UL + maximum / 2UL) / maximum);
}

struct CapturedFrame {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t source_width{};
    std::uint32_t source_height{};
    std::vector<std::uint8_t> rgba;
};

class X11WindowCapture {
public:
    X11WindowCapture() = default;
    X11WindowCapture(const X11WindowCapture&) = delete;
    X11WindowCapture& operator=(const X11WindowCapture&) = delete;

    ~X11WindowCapture() {
        release_target();
        if (display_ != nullptr) {
            api_.close_display(display_);
        }
    }

    bool initialize() {
        if (!api_.load()) {
            return false;
        }
        api_.set_error_handler(ignore_x_error);
        display_ = api_.open_display(nullptr);
        if (display_ == nullptr) {
            std::cerr << "dashboard: cannot open X11 display; DISPLAY="
                      << (std::getenv("DISPLAY") != nullptr ? std::getenv("DISPLAY") : "<unset>") << '\n';
            return false;
        }
        screen_ = api_.default_screen(display_);
        root_ = api_.root_window(display_, screen_);
        net_wm_name_ = api_.intern_atom(display_, "_NET_WM_NAME", x11::False);
        utf8_string_ = api_.intern_atom(display_, "UTF8_STRING", x11::False);

        if (api_.has_composite()) {
            int event_base = 0;
            int error_base = 0;
            composite_available_ = api_.composite_query_extension(display_, &event_base, &error_base) != x11::False;
        }
        std::cout << "dashboard: X11 capture ready (XComposite="
                  << (composite_available_ ? "yes" : "no; using XGetImage fallback") << ")\n";
        return true;
    }

    [[nodiscard]] bool has_target() const {
        return target_ != 0UL;
    }

    [[nodiscard]] std::uint32_t source_width() const {
        return source_width_;
    }

    [[nodiscard]] std::uint32_t source_height() const {
        return source_height_;
    }

    bool refresh_target() {
        if (target_ != 0UL) {
            x11::XWindowAttributes attributes{};
            if (api_.get_window_attributes(display_, target_, &attributes) != 0 &&
                attributes.map_state == x11::IsViewable && attributes.width > 0 && attributes.height > 0) {
                return true;
            }
            std::cout << "dashboard: Standable window disappeared; searching again\n";
            release_target();
        }

        const std::optional<WindowCandidate> candidate = find_standable_window();
        if (!candidate.has_value()) {
            return false;
        }

        target_ = candidate->window;
        source_width_ = static_cast<std::uint32_t>(candidate->width);
        source_height_ = static_cast<std::uint32_t>(candidate->height);
        title_ = candidate->title;
        class_name_ = candidate->class_name;
        setup_composite_pixmap();
        std::cout << "dashboard: capturing X11 window 0x" << std::hex << target_ << std::dec
                  << " size=" << source_width_ << 'x' << source_height_
                  << " title=\"" << title_ << "\" class=\"" << class_name_ << "\"\n";
        return true;
    }

    std::optional<CapturedFrame> capture(std::uint32_t maximum_width, std::uint32_t maximum_height) {
        if (!refresh_target()) {
            return std::nullopt;
        }

        x11::XWindowAttributes attributes{};
        if (api_.get_window_attributes(display_, target_, &attributes) == 0 || attributes.width <= 0 || attributes.height <= 0) {
            release_target();
            return std::nullopt;
        }

        const std::uint32_t new_width = static_cast<std::uint32_t>(attributes.width);
        const std::uint32_t new_height = static_cast<std::uint32_t>(attributes.height);
        if (new_width != source_width_ || new_height != source_height_) {
            source_width_ = new_width;
            source_height_ = new_height;
            setup_composite_pixmap();
            std::cout << "dashboard: source resized to " << source_width_ << 'x' << source_height_ << '\n';
        }

        const x11::Drawable drawable = pixmap_ != 0UL ? pixmap_ : target_;
        g_x_error_count.store(0U);
        x11::XImage* image = api_.get_image(display_, drawable, 0, 0, source_width_, source_height_,
                                            x11::AllPlanes, x11::ZPixmap);
        api_.sync(display_, x11::False);
        if (image == nullptr || g_x_error_count.load() != 0U) {
            if (image != nullptr) {
                api_.destroy_image(image);
            }
            if (pixmap_ != 0UL) {
                std::cout << "dashboard: composite pixmap capture failed; retrying direct window capture\n";
                release_pixmap();
                image = api_.get_image(display_, target_, 0, 0, source_width_, source_height_,
                                       x11::AllPlanes, x11::ZPixmap);
            }
        }
        if (image == nullptr) {
            return std::nullopt;
        }

        const double scale = std::min({1.0,
                                       static_cast<double>(maximum_width) / static_cast<double>(source_width_),
                                       static_cast<double>(maximum_height) / static_cast<double>(source_height_)});
        CapturedFrame frame;
        frame.source_width = source_width_;
        frame.source_height = source_height_;
        frame.width = std::max(1U, static_cast<std::uint32_t>(std::floor(static_cast<double>(source_width_) * scale)));
        frame.height = std::max(1U, static_cast<std::uint32_t>(std::floor(static_cast<double>(source_height_) * scale)));
        const std::size_t pixel_count = static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height);
        frame.rgba.resize(pixel_count * 4U);

        for (std::uint32_t y = 0; y < frame.height; ++y) {
            const int source_y = static_cast<int>((static_cast<std::uint64_t>(y) * source_height_) / frame.height);
            for (std::uint32_t x = 0; x < frame.width; ++x) {
                const int source_x = static_cast<int>((static_cast<std::uint64_t>(x) * source_width_) / frame.width);
                const unsigned long pixel = read_pixel(image, source_x, source_y);
                const std::size_t offset = (static_cast<std::size_t>(y) * frame.width + x) * 4U;
                frame.rgba[offset] = channel_from_pixel(pixel, image->red_mask);
                frame.rgba[offset + 1U] = channel_from_pixel(pixel, image->green_mask);
                frame.rgba[offset + 2U] = channel_from_pixel(pixel, image->blue_mask);
                frame.rgba[offset + 3U] = 255U;
            }
        }
        api_.destroy_image(image);
        return frame;
    }

    void send_mouse_move(float overlay_x, float overlay_y, unsigned int button_state) {
        if (target_ == 0UL || source_width_ == 0U || source_height_ == 0U) {
            return;
        }
        const int x = clamp_coordinate(overlay_x, source_width_);
        const int y = static_cast<int>(source_height_) - 1 - clamp_coordinate(overlay_y, source_height_);
        last_x_ = x;
        last_y_ = y;
        x11::XEvent event{};
        event.motion.type = x11::MotionNotify;
        event.motion.send_event = x11::True;
        event.motion.display = display_;
        event.motion.window = target_;
        event.motion.root = root_;
        event.motion.time = 0UL;
        event.motion.x = x;
        event.motion.y = y;
        populate_root_coordinates(event.motion.x_root, event.motion.y_root, x, y);
        event.motion.state = button_state;
        event.motion.same_screen = x11::True;
        api_.send_event(display_, target_, x11::True, x11::PointerMotionMask, &event);
        api_.flush(display_);
    }

    void send_mouse_button(unsigned int button, bool pressed, unsigned int button_state) {
        if (target_ == 0UL) {
            return;
        }
        x11::XEvent event{};
        event.button.type = pressed ? x11::ButtonPress : x11::ButtonRelease;
        event.button.send_event = x11::True;
        event.button.display = display_;
        event.button.window = target_;
        event.button.root = root_;
        event.button.time = 0UL;
        event.button.x = last_x_;
        event.button.y = last_y_;
        populate_root_coordinates(event.button.x_root, event.button.y_root, last_x_, last_y_);
        event.button.state = button_state;
        event.button.button = button;
        event.button.same_screen = x11::True;
        const long mask = pressed ? x11::ButtonPressMask : x11::ButtonReleaseMask;
        api_.send_event(display_, target_, x11::True, mask, &event);
        api_.flush(display_);
    }

    void send_scroll(float x_delta, float y_delta, unsigned int button_state) {
        const int vertical_steps = std::clamp(static_cast<int>(std::round(std::abs(y_delta))), 1, 8);
        if (std::abs(y_delta) > 0.01F) {
            const unsigned int button = y_delta > 0.0F ? 4U : 5U;
            for (int index = 0; index < vertical_steps; ++index) {
                send_mouse_button(button, true, button_state);
                send_mouse_button(button, false, button_state);
            }
        }
        const int horizontal_steps = std::clamp(static_cast<int>(std::round(std::abs(x_delta))), 1, 8);
        if (std::abs(x_delta) > 0.01F) {
            const unsigned int button = x_delta > 0.0F ? 7U : 6U;
            for (int index = 0; index < horizontal_steps; ++index) {
                send_mouse_button(button, true, button_state);
                send_mouse_button(button, false, button_state);
            }
        }
    }

private:
    struct WindowCandidate {
        x11::Window window{};
        int width{};
        int height{};
        int score{};
        std::string title;
        std::string class_name;
    };

    struct PendingWindow {
        x11::Window window{};
        unsigned int depth{};
    };

    [[nodiscard]] std::string window_title(x11::Window window) const {
        if (net_wm_name_ != 0UL) {
            x11::Atom actual_type = 0UL;
            int actual_format = 0;
            unsigned long item_count = 0UL;
            unsigned long bytes_after = 0UL;
            unsigned char* property = nullptr;
            const int status = api_.get_window_property(display_, window, net_wm_name_, 0L, 1024L, x11::False,
                                                        utf8_string_ != 0UL ? utf8_string_ : x11::AnyPropertyType,
                                                        &actual_type, &actual_format, &item_count, &bytes_after, &property);
            if (status == x11::Success && property != nullptr && actual_format == 8) {
                std::string value(reinterpret_cast<char*>(property), static_cast<std::size_t>(item_count));
                api_.free_memory(property);
                return value;
            }
            if (property != nullptr) {
                api_.free_memory(property);
            }
        }
        char* fetched = nullptr;
        if (api_.fetch_name(display_, window, &fetched) != 0 && fetched != nullptr) {
            std::string value(fetched);
            api_.free_memory(fetched);
            return value;
        }
        return {};
    }

    [[nodiscard]] std::string window_class(x11::Window window) const {
        x11::XClassHint hint{};
        if (api_.get_class_hint(display_, window, &hint) == 0) {
            return {};
        }
        std::string value;
        if (hint.res_name != nullptr) {
            value = hint.res_name;
            api_.free_memory(hint.res_name);
        }
        if (hint.res_class != nullptr) {
            if (!value.empty()) {
                value.push_back('/');
            }
            value += hint.res_class;
            api_.free_memory(hint.res_class);
        }
        return value;
    }

    [[nodiscard]] std::optional<WindowCandidate> find_standable_window() const {
        std::vector<PendingWindow> pending{{root_, 0U}};
        std::optional<WindowCandidate> best;
        std::size_t cursor = 0U;
        std::size_t visited = 0U;
        while (cursor < pending.size() && visited < 8192U) {
            const PendingWindow parent = pending[cursor++];
            x11::Window returned_root = 0UL;
            x11::Window returned_parent = 0UL;
            x11::Window* children = nullptr;
            unsigned int child_count = 0U;
            if (api_.query_tree(display_, parent.window, &returned_root, &returned_parent, &children, &child_count) == 0) {
                continue;
            }
            for (unsigned int index = 0U; index < child_count && visited < 8192U; ++index, ++visited) {
                const x11::Window child = children[index];
                x11::XWindowAttributes attributes{};
                if (api_.get_window_attributes(display_, child, &attributes) == 0) {
                    continue;
                }
                if (parent.depth < 6U) {
                    pending.push_back(PendingWindow{child, parent.depth + 1U});
                }
                if (attributes.map_state != x11::IsViewable || attributes.width < 240 || attributes.height < 160) {
                    continue;
                }

                const std::string title = window_title(child);
                const std::string class_name = window_class(child);
                if (is_bridge_helper_window(title, class_name)) {
                    continue;
                }
                const std::string title_lower = lowercase(title);
                const std::string class_lower = lowercase(class_name);
                int score = 0;
                if (title_lower.find("standable full body estimation") != std::string::npos) {
                    score += 400;
                } else if (title_lower.find("standable") != std::string::npos) {
                    score += 250;
                }
                if (class_lower.find("standable.exe") != std::string::npos) {
                    score += 500;
                } else if (class_lower.find("standable") != std::string::npos) {
                    score += 300;
                }
                if (score == 0) {
                    continue;
                }
                const long long area = static_cast<long long>(attributes.width) * static_cast<long long>(attributes.height);
                score += static_cast<int>(std::min(area / 100000LL, 100LL));
                WindowCandidate candidate{child, attributes.width, attributes.height, score, title, class_name};
                if (!best.has_value() || candidate.score > best->score) {
                    best = std::move(candidate);
                }
            }
            if (children != nullptr) {
                api_.free_memory(children);
            }
        }
        return best;
    }

    void setup_composite_pixmap() {
        release_pixmap();
        if (!composite_available_ || target_ == 0UL) {
            return;
        }
        g_x_error_count.store(0U);
        api_.composite_redirect_window(display_, target_, x11::CompositeRedirectAutomatic);
        api_.sync(display_, x11::False);
        if (g_x_error_count.load() != 0U) {
            g_x_error_count.store(0U);
        }
        pixmap_ = api_.composite_name_window_pixmap(display_, target_);
        api_.sync(display_, x11::False);
        if (g_x_error_count.load() != 0U) {
            pixmap_ = 0UL;
        }
        redirected_ = pixmap_ != 0UL;
    }

    void release_pixmap() {
        if (pixmap_ != 0UL) {
            api_.free_pixmap(display_, pixmap_);
            pixmap_ = 0UL;
        }
        if (redirected_ && target_ != 0UL && composite_available_) {
            g_x_error_count.store(0U);
            api_.composite_unredirect_window(display_, target_, x11::CompositeRedirectAutomatic);
            api_.sync(display_, x11::False);
            redirected_ = false;
        }
    }

    void release_target() {
        if (display_ != nullptr) {
            release_pixmap();
        }
        target_ = 0UL;
        source_width_ = 0U;
        source_height_ = 0U;
        title_.clear();
        class_name_.clear();
    }

    [[nodiscard]] unsigned long read_pixel(const x11::XImage* image, int x, int y) const {
        if (image->bits_per_pixel == 32 && image->data != nullptr) {
            std::uint32_t value = 0U;
            const char* address = image->data + static_cast<std::ptrdiff_t>(y) * image->bytes_per_line +
                                  static_cast<std::ptrdiff_t>(x) * 4;
            std::memcpy(&value, address, sizeof(value));
            if (image->byte_order != 0) {
                value = std::byteswap(value);
            }
            return static_cast<unsigned long>(value);
        }
        return api_.get_pixel(const_cast<x11::XImage*>(image), x, y);
    }

    [[nodiscard]] static int clamp_coordinate(float coordinate, std::uint32_t extent) {
        const float maximum = static_cast<float>(extent > 0U ? extent - 1U : 0U);
        return static_cast<int>(std::round(std::clamp(coordinate, 0.0F, maximum)));
    }

    void populate_root_coordinates(int& root_x, int& root_y, int local_x, int local_y) const {
        int origin_x = 0;
        int origin_y = 0;
        x11::Window child = 0UL;
        api_.translate_coordinates(display_, target_, root_, 0, 0, &origin_x, &origin_y, &child);
        root_x = origin_x + local_x;
        root_y = origin_y + local_y;
    }

    X11Api api_;
    x11::Display* display_{nullptr};
    int screen_{};
    x11::Window root_{};
    x11::Atom net_wm_name_{};
    x11::Atom utf8_string_{};
    bool composite_available_{false};
    bool redirected_{false};
    x11::Window target_{};
    x11::Pixmap pixmap_{};
    std::uint32_t source_width_{};
    std::uint32_t source_height_{};
    int last_x_{};
    int last_y_{};
    std::string title_;
    std::string class_name_;
};

class OpenVrClient {
public:
    OpenVrClient() = default;
    OpenVrClient(const OpenVrClient&) = delete;
    OpenVrClient& operator=(const OpenVrClient&) = delete;

    ~OpenVrClient() {
        shutdown();
    }

    bool initialize(const std::filesystem::path& steamvr_root) {
        shutdown();
        const std::vector<std::filesystem::path> candidates{
            steamvr_root / "bin/linux64/vrclient.so",
            steamvr_root / "bin/linux32/vrclient.so",
        };
        std::filesystem::path selected;
        for (const auto& candidate : candidates) {
            if (std::filesystem::is_regular_file(candidate)) {
                selected = candidate;
                break;
            }
        }
        if (selected.empty()) {
            std::cerr << "dashboard: SteamVR vrclient.so was not found under " << steamvr_root << '\n';
            return false;
        }
        if (!library_.open(selected)) {
            std::cerr << "dashboard: failed to load " << selected << ": " << library_.error() << '\n';
            return false;
        }

        using FactoryFn = void* (*)(const char*, int*);
        const FactoryFn factory = library_.symbol<FactoryFn>("VRClientCoreFactory");
        if (factory == nullptr) {
            std::cerr << "dashboard: vrclient.so does not export VRClientCoreFactory\n";
            return false;
        }
        int factory_error = 0;
        core_ = static_cast<vr::IVRClientCore*>(factory(vr::kClientCoreVersion, &factory_error));
        if (core_ == nullptr) {
            std::cerr << "dashboard: IVRClientCore factory failed with code " << factory_error << '\n';
            return false;
        }

        const vr::EVRInitError init_error = core_->Init(vr::VRApplication_Overlay, nullptr);
        if (init_error != vr::VRInitError_None) {
            std::cerr << "dashboard: OpenVR overlay initialization failed: " << error_string(init_error)
                      << " (" << static_cast<int>(init_error) << ")\n";
            core_ = nullptr;
            return false;
        }
        initialized_ = true;

        vr::EVRInitError interface_error = vr::VRInitError_None;
        overlay_ = static_cast<vr::IVROverlay*>(core_->GetGenericInterface(vr::IVROverlay_Version, &interface_error));
        if (overlay_ == nullptr || interface_error != vr::VRInitError_None) {
            std::cerr << "dashboard: IVROverlay " << vr::IVROverlay_Version
                      << " is unavailable: " << error_string(interface_error) << '\n';
            shutdown();
            return false;
        }
        std::cout << "dashboard: connected through " << selected << " using " << vr::IVROverlay_Version << '\n';
        return true;
    }

    void shutdown() {
        overlay_ = nullptr;
        if (initialized_ && core_ != nullptr) {
            core_->Cleanup();
        }
        initialized_ = false;
        core_ = nullptr;
        library_ = SharedLibrary{};
    }

    [[nodiscard]] vr::IVROverlay* overlay() const {
        return overlay_;
    }

    [[nodiscard]] std::string error_string(vr::EVRInitError error) const {
        if (core_ != nullptr) {
            const char* text = core_->GetEnglishStringForHmdError(error);
            if (text != nullptr) {
                return text;
            }
        }
        return "OpenVR initialization error";
    }

private:
    SharedLibrary library_;
    vr::IVRClientCore* core_{nullptr};
    vr::IVROverlay* overlay_{nullptr};
    bool initialized_{false};
};

struct Arguments {
    std::filesystem::path steamvr_root;
    std::filesystem::path driver_root;
    pid_t parent_pid{};
    int frames_per_second{20};
    bool self_test{false};
};

void print_usage() {
    std::cout << "Usage: standable_dashboard_overlay --steamvr-root PATH --driver-root PATH "
                 "[--parent-pid PID] [--fps 1..60]\n";
}

std::optional<Arguments> parse_arguments(int argc, char** argv) {
    Arguments arguments;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option(argv[index]);
        auto next = [&]() -> const char* {
            if (index + 1 >= argc) {
                return nullptr;
            }
            return argv[++index];
        };
        if (option == "--steamvr-root") {
            const char* value = next();
            if (value == nullptr) {
                return std::nullopt;
            }
            arguments.steamvr_root = value;
        } else if (option == "--driver-root") {
            const char* value = next();
            if (value == nullptr) {
                return std::nullopt;
            }
            arguments.driver_root = value;
        } else if (option == "--parent-pid") {
            const char* value = next();
            if (value == nullptr) {
                return std::nullopt;
            }
            try {
                const long parsed = std::stol(value);
                if (parsed <= 0L || parsed > std::numeric_limits<pid_t>::max()) {
                    return std::nullopt;
                }
                arguments.parent_pid = static_cast<pid_t>(parsed);
            } catch (...) {
                return std::nullopt;
            }
        } else if (option == "--fps") {
            const char* value = next();
            if (value == nullptr) {
                return std::nullopt;
            }
            try {
                arguments.frames_per_second = std::stoi(value);
            } catch (...) {
                return std::nullopt;
            }
            if (arguments.frames_per_second < 1 || arguments.frames_per_second > 60) {
                return std::nullopt;
            }
        } else if (option == "--self-test") {
            arguments.self_test = true;
        } else if (option == "-h" || option == "--help") {
            print_usage();
            std::exit(0);
        } else {
            return std::nullopt;
        }
    }
    if (!arguments.self_test && (arguments.steamvr_root.empty() || arguments.driver_root.empty())) {
        return std::nullopt;
    }
    return arguments;
}

[[nodiscard]] bool parent_is_alive(pid_t parent_pid) {
    if (parent_pid <= 0) {
        return true;
    }
    if (kill(parent_pid, 0) == 0) {
        return true;
    }
    return errno == EPERM;
}

std::vector<std::uint8_t> create_placeholder(std::uint32_t width, std::uint32_t height, bool thumbnail) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4U);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const float horizontal = static_cast<float>(x) / static_cast<float>(std::max(1U, width - 1U));
            const float vertical = static_cast<float>(y) / static_cast<float>(std::max(1U, height - 1U));
            std::uint8_t red = static_cast<std::uint8_t>(24.0F + 22.0F * horizontal);
            std::uint8_t green = static_cast<std::uint8_t>(17.0F + 12.0F * vertical);
            std::uint8_t blue = static_cast<std::uint8_t>(52.0F + 50.0F * (1.0F - vertical));
            const float center_x = static_cast<float>(x) - static_cast<float>(width) * 0.5F;
            const float center_y = static_cast<float>(y) - static_cast<float>(height) * 0.5F;
            const float radius = std::sqrt(center_x * center_x + center_y * center_y);
            const float ring = static_cast<float>(std::min(width, height)) * (thumbnail ? 0.25F : 0.12F);
            if (std::abs(radius - ring) < static_cast<float>(std::max(2U, width / 80U))) {
                red = 208U;
                green = 191U;
                blue = 255U;
            }
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4U;
            pixels[offset] = red;
            pixels[offset + 1U] = green;
            pixels[offset + 2U] = blue;
            pixels[offset + 3U] = 255U;
        }
    }
    return pixels;
}

std::optional<std::filesystem::path> find_thumbnail(const std::filesystem::path& driver_root) {
    const std::vector<std::filesystem::path> candidates{
        driver_root / "resources/UI Themes/The Stand/standable_logo.png",
        driver_root / "resources/UI Themes/The Stand/Standable_Logo.png",
        driver_root / "resources/icons/standable.png",
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool overlay_ok(vr::IVROverlay* overlay, vr::EVROverlayError error, std::string_view operation) {
    if (error == vr::VROverlayError_None) {
        return true;
    }
    const char* name = overlay->GetOverlayErrorNameFromEnum(error);
    std::cerr << "dashboard: " << operation << " failed: " << (name != nullptr ? name : "unknown overlay error")
              << " (" << static_cast<int>(error) << ")\n";
    return false;
}

class DashboardOverlay {
public:
    DashboardOverlay(vr::IVROverlay* overlay, std::filesystem::path driver_root)
        : overlay_(overlay), driver_root_(std::move(driver_root)) {}

    ~DashboardOverlay() {
        if (created_ && main_handle_ != vr::k_ulOverlayHandleInvalid) {
            overlay_->DestroyOverlay(main_handle_);
        }
    }

    bool create() {
        const vr::EVROverlayError create_error = overlay_->CreateDashboardOverlay(
            "standable.linux.dashboard", "Standable", &main_handle_, &thumbnail_handle_);
        if (!overlay_ok(overlay_, create_error, "CreateDashboardOverlay")) {
            if (create_error == vr::VROverlayError_KeyInUse) {
                std::cerr << "dashboard: another Standable dashboard companion already owns the tab\n";
            }
            return false;
        }
        created_ = true;

        bool configured = true;
        configured &= overlay_ok(overlay_, overlay_->SetOverlayWidthInMeters(main_handle_, 2.5F), "SetOverlayWidthInMeters");
        configured &= overlay_ok(overlay_, overlay_->SetOverlayInputMethod(main_handle_, vr::VROverlayInputMethod_Mouse),
                                 "SetOverlayInputMethod");
        configured &= overlay_ok(overlay_, overlay_->SetOverlayFlag(
                                     main_handle_, vr::VROverlayFlags_SendVRSmoothScrollEvents, true),
                                 "SetOverlayFlag(smooth scroll)");
        configured &= overlay_ok(overlay_, overlay_->SetOverlayFlag(
                                     main_handle_, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true),
                                 "SetOverlayFlag(interactive)");

        const std::vector<std::uint8_t> waiting = create_placeholder(640U, 360U, false);
        configured &= overlay_ok(overlay_, overlay_->SetOverlayRaw(main_handle_, const_cast<std::uint8_t*>(waiting.data()),
                                                                   640U, 360U, 4U),
                                 "SetOverlayRaw(waiting frame)");

        const std::optional<std::filesystem::path> thumbnail = find_thumbnail(driver_root_);
        if (thumbnail.has_value()) {
            const vr::EVROverlayError file_error = overlay_->SetOverlayFromFile(thumbnail_handle_, thumbnail->c_str());
            if (!overlay_ok(overlay_, file_error, "SetOverlayFromFile(thumbnail)")) {
                set_fallback_thumbnail();
            } else {
                std::cout << "dashboard: thumbnail loaded from " << *thumbnail << '\n';
            }
        } else {
            set_fallback_thumbnail();
        }
        std::cout << "dashboard: native SteamVR dashboard tab created\n";
        return configured;
    }

    [[nodiscard]] bool is_active() const {
        return overlay_->IsActiveDashboardOverlay(main_handle_);
    }

    bool submit(CapturedFrame& frame) {
        if (frame.rgba.empty()) {
            return false;
        }
        const vr::EVROverlayError error = overlay_->SetOverlayRaw(main_handle_, frame.rgba.data(),
                                                                  frame.width, frame.height, 4U);
        if (!overlay_ok(overlay_, error, "SetOverlayRaw(window frame)")) {
            return false;
        }
        if (frame.source_width != mouse_width_ || frame.source_height != mouse_height_) {
            vr::HmdVector2_t mouse_scale{{static_cast<float>(frame.source_width),
                                          static_cast<float>(frame.source_height)}};
            const bool mouse_scale_set = overlay_ok(
                overlay_, overlay_->SetOverlayMouseScale(main_handle_, &mouse_scale), "SetOverlayMouseScale");
            if (!mouse_scale_set) {
                return false;
            }
            mouse_width_ = frame.source_width;
            mouse_height_ = frame.source_height;
        }
        return true;
    }

    void process_events(X11WindowCapture* capture) {
        vr::VREvent_t event{};
        while (overlay_->PollNextOverlayEvent(main_handle_, &event, sizeof(event))) {
            switch (event.eventType) {
                case vr::VREvent_MouseMove:
                    if (capture != nullptr) {
                        capture->send_mouse_move(event.data.mouse.x, event.data.mouse.y, x_button_state_);
                    }
                    break;
                case vr::VREvent_MouseButtonDown: {
                    const unsigned int button = x_button_number(event.data.mouse.button);
                    if (button != 0U && capture != nullptr) {
                        capture->send_mouse_button(button, true, x_button_state_);
                        x_button_state_ |= x_state_mask(button);
                    }
                    break;
                }
                case vr::VREvent_MouseButtonUp: {
                    const unsigned int button = x_button_number(event.data.mouse.button);
                    if (button != 0U && capture != nullptr) {
                        capture->send_mouse_button(button, false, x_button_state_);
                        x_button_state_ &= ~x_state_mask(button);
                    }
                    break;
                }
                case vr::VREvent_ScrollSmooth:
                case vr::VREvent_ScrollDiscrete:
                    if (capture != nullptr) {
                        capture->send_scroll(event.data.scroll.xdelta, event.data.scroll.ydelta, x_button_state_);
                    }
                    break;
                case vr::VREvent_Quit:
                case vr::VREvent_ProcessQuit:
                    g_running.store(false);
                    break;
                default:
                    break;
            }
        }
    }

private:
    void set_fallback_thumbnail() {
        std::vector<std::uint8_t> thumbnail = create_placeholder(256U, 256U, true);
        const bool thumbnail_set = overlay_ok(
            overlay_, overlay_->SetOverlayRaw(thumbnail_handle_, thumbnail.data(), 256U, 256U, 4U),
            "SetOverlayRaw(thumbnail)");
        if (!thumbnail_set) {
            std::cerr << "dashboard: fallback thumbnail could not be submitted\n";
        }
    }

    [[nodiscard]] static unsigned int x_button_number(std::uint32_t openvr_button) {
        switch (openvr_button) {
            case vr::VRMouseButton_Left:
                return 1U;
            case vr::VRMouseButton_Middle:
                return 2U;
            case vr::VRMouseButton_Right:
                return 3U;
            default:
                return 0U;
        }
    }

    [[nodiscard]] static unsigned int x_state_mask(unsigned int button) {
        switch (button) {
            case 1U:
                return x11::Button1Mask;
            case 2U:
                return x11::Button2Mask;
            case 3U:
                return x11::Button3Mask;
            default:
                return 0U;
        }
    }

    vr::IVROverlay* overlay_{};
    std::filesystem::path driver_root_;
    vr::VROverlayHandle_t main_handle_{vr::k_ulOverlayHandleInvalid};
    vr::VROverlayHandle_t thumbnail_handle_{vr::k_ulOverlayHandleInvalid};
    bool created_{false};
    std::uint32_t mouse_width_{};
    std::uint32_t mouse_height_{};
    unsigned int x_button_state_{};
};

int run_self_test() {
    bool passed = true;
    passed &= lowercase("StAnDaBlE.EXE") == "standable.exe";
    passed &= is_bridge_helper_window(
        R"(Z:\Standable Full Body Estimation\bin\win64\standable_bridge_host.exe)",
        "steam_app_2370570/steam_app_2370570");
    passed &= !is_bridge_helper_window("Standable v3.0.3", "steam_app_2370570/steam_app_2370570");
    passed &= channel_from_pixel(0x00FF0000UL, 0x00FF0000UL) == 255U;
    passed &= channel_from_pixel(0x0000FF00UL, 0x0000FF00UL) == 255U;
    passed &= channel_from_pixel(0x000000FFUL, 0x000000FFUL) == 255U;
    const std::vector<std::uint8_t> placeholder = create_placeholder(64U, 32U, false);
    passed &= placeholder.size() == 64U * 32U * 4U;
    if (!passed) {
        std::cerr << "dashboard self-test failed\n";
        return 1;
    }
    std::cout << "dashboard self-test passed\n";
    return 0;
}

int run_dashboard(const Arguments& arguments) {
    OpenVrClient openvr;
    while (g_running.load() && parent_is_alive(arguments.parent_pid)) {
        if (openvr.initialize(arguments.steamvr_root)) {
            break;
        }
        std::cerr << "dashboard: SteamVR is not ready; retrying in 2 seconds\n";
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (!g_running.load() || !parent_is_alive(arguments.parent_pid) || openvr.overlay() == nullptr) {
        return 4;
    }

    DashboardOverlay dashboard(openvr.overlay(), arguments.driver_root);
    if (!dashboard.create()) {
        return 5;
    }

    X11WindowCapture capture;
    bool capture_ready = capture.initialize();
    if (!capture_ready) {
        std::cerr << "dashboard: tab is active, but desktop capture is unavailable; retrying in the background\n";
    }

    constexpr std::uint32_t maximum_width = 1280U;
    constexpr std::uint32_t maximum_height = 720U;
    const auto frame_interval = std::chrono::microseconds(1000000 / arguments.frames_per_second);
    auto next_frame = Clock::now();
    auto next_search = Clock::now();
    auto next_capture_retry = Clock::now() + std::chrono::seconds(2);

    while (g_running.load() && parent_is_alive(arguments.parent_pid)) {
        dashboard.process_events(capture_ready ? &capture : nullptr);
        const auto now = Clock::now();
        if (!capture_ready && now >= next_capture_retry) {
            capture_ready = capture.initialize();
            next_capture_retry = now + std::chrono::seconds(2);
            if (capture_ready) {
                std::cout << "dashboard: desktop capture became available\n";
            }
        }
        if (capture_ready && now >= next_search) {
            capture.refresh_target();
            next_search = now + std::chrono::seconds(1);
        }
        if (capture_ready && dashboard.is_active() && now >= next_frame) {
            std::optional<CapturedFrame> frame = capture.capture(maximum_width, maximum_height);
            if (frame.has_value()) {
                dashboard.submit(*frame);
            }
            next_frame = now + frame_interval;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::cout << "dashboard: companion shutting down\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGHUP, SIG_IGN);
    const std::optional<Arguments> arguments = parse_arguments(argc, argv);
    if (!arguments.has_value()) {
        print_usage();
        return 2;
    }
    if (arguments->self_test) {
        return run_self_test();
    }
    std::cout << "Standable native dashboard companion " << STANDABLE_BRIDGE_VERSION << '\n';
    return run_dashboard(*arguments);
}
