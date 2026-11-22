// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 SparkMoE contributors.

#include "desktop-window.h"

#if defined(_WIN32) && defined(SPARKMOE_WEBVIEW2)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <wrl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace sparkmoe {

struct desktop_window::implementation {
    static constexpr UINT show_message = WM_APP + 10;
    static constexpr UINT stop_message = WM_APP + 11;
    static constexpr UINT recover_message = WM_APP + 12;
    static constexpr UINT_PTR startup_timer = 1;
    static constexpr const wchar_t * class_name = L"SparkMoE.Desktop";
    using create_environment_fn = HRESULT (STDAPICALLTYPE *)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);
    using runtime_version_fn = HRESULT (STDAPICALLTYPE *)(PCWSTR, LPWSTR *);

    options config;
    std::thread thread;
    std::atomic<HWND> window{nullptr};
    std::atomic<bool> stop_requested{false};
    std::mutex mutex;
    std::condition_variable condition;
    bool initialized = false;
    bool ready = false;
    bool ever_ready = false;
    bool failed = false;
    bool recovered = false;
    HMODULE loader = nullptr;
    create_environment_fn create_environment = nullptr;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    std::shared_ptr<int> callbacks;
    std::wstring url;
    std::wstring origin;

    void log(const std::string & message) const {
        if (config.log) {
            config.log("desktop: " + message);
        }
    }

    void complete(bool success) {
        std::lock_guard<std::mutex> lock(mutex);
        ready = success;
        initialized = true;
        condition.notify_all();
    }

    void fail(const char * stage, HRESULT result) {
        if (failed || stop_requested) {
            return;
        }
        failed = true;
        std::ostringstream message;
        message << stage << " failed HRESULT=0x" << std::hex << static_cast<unsigned long>(result);
        log(message.str());
        complete(false);
        if (ever_ready && config.browser_fallback) {
            config.browser_fallback();
        }
        PostMessageW(window.load(), stop_message, 0, 0);
    }

    bool trusted(const std::wstring & candidate) const {
        if (candidate.compare(0, origin.size(), origin) != 0) {
            return false;
        }
        return candidate.size() == origin.size() || candidate[origin.size()] == L'/' || candidate[origin.size()] == L'?' || candidate[origin.size()] == L'#';
    }

    static bool external(const std::wstring & candidate) {
        return candidate.compare(0, 8, L"https://") == 0 || candidate.compare(0, 7, L"http://") == 0;
    }

    void open_external(const std::wstring & candidate) {
        if (external(candidate)) {
            ShellExecuteW(window.load(), L"open", candidate.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    void resize() {
        if (controller) {
            RECT bounds{};
            GetClientRect(window.load(), &bounds);
            controller->put_Bounds(bounds);
            controller->NotifyParentWindowPositionChanged();
        }
    }

    void theme() {
        DWORD light = 1;
        DWORD size = sizeof(light);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
        const BOOL dark = light == 0;
        HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (dwm) {
            using set_attribute_fn = HRESULT (WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
            auto set_attribute = reinterpret_cast<set_attribute_fn>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
            if (set_attribute) {
                set_attribute(window.load(), 20, &dark, sizeof(dark));
            }
            FreeLibrary(dwm);
        }
    }

    std::filesystem::path placement_file() const {
        return config.user_data_directory.parent_path() / "window-placement.txt";
    }

    void save_placement() {
        WINDOWPLACEMENT placement{sizeof(placement)};
        if (GetWindowPlacement(window.load(), &placement)) {
            std::ofstream output(placement_file());
            RECT r = placement.rcNormalPosition;
            MONITORINFO monitor{sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromWindow(window.load(), MONITOR_DEFAULTTONEAREST), &monitor)) {
                OffsetRect(&r, monitor.rcWork.left - monitor.rcMonitor.left, monitor.rcWork.top - monitor.rcMonitor.top);
            }
            output << r.left << ' ' << r.top << ' ' << r.right << ' ' << r.bottom << ' ' << (placement.showCmd == SW_SHOWMAXIMIZED || (placement.flags & WPF_RESTORETOMAXIMIZED)) << ' ' << GetDpiForWindow(window.load());
        }
    }

    int restore_placement() {
        RECT r{};
        int maximized = 0;
        UINT saved_dpi = 96;
        std::ifstream input(placement_file());
        if (!(input >> r.left >> r.top >> r.right >> r.bottom >> maximized >> saved_dpi) || r.right <= r.left || r.bottom <= r.top || saved_dpi < 48 || saved_dpi > 768) {
            return SW_SHOWNORMAL;
        }
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &monitor)) {
            return SW_SHOWNORMAL;
        }
        const RECT & work = monitor.rcWork;
        SetWindowPos(window.load(), nullptr, work.left, work.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        const UINT dpi = GetDpiForWindow(window.load());
        const auto scaled_width = (static_cast<long long>(r.right) - r.left) * dpi / saved_dpi;
        const auto scaled_height = (static_cast<long long>(r.bottom) - r.top) * dpi / saved_dpi;
        const int width = static_cast<int>(std::clamp<long long>(scaled_width, std::min<LONG>(MulDiv(720, dpi, 96), work.right - work.left), work.right - work.left));
        const int height = static_cast<int>(std::clamp<long long>(scaled_height, std::min<LONG>(MulDiv(480, dpi, 96), work.bottom - work.top), work.bottom - work.top));
        const int left = std::clamp<LONG>(r.left, work.left, work.right - width);
        const int top = std::clamp<LONG>(r.top, work.top, work.bottom - height);
        SetWindowPos(window.load(), nullptr, left, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        return maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    }

    void close_webview() {
        callbacks.reset();
        webview.Reset();
        if (controller) {
            controller->Close();
            controller.Reset();
        }
        environment.Reset();
    }

    HRESULT configure_webview() {
        HRESULT result = controller->get_CoreWebView2(&webview);
        if (FAILED(result)) {
            return result;
        }
        ComPtr<ICoreWebView2Settings> settings;
        result = webview->get_Settings(&settings);
        if (FAILED(result)) {
            return result;
        }
        result = settings->put_AreHostObjectsAllowed(FALSE);
        if (FAILED(result)) { return result; }
        result = settings->put_IsWebMessageEnabled(FALSE);
        if (FAILED(result)) { return result; }
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        const std::weak_ptr<int> lifetime = callbacks;
        EventRegistrationToken token{};
        result = webview->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>([this, lifetime](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs * args) -> HRESULT {
            if (lifetime.expired()) { return S_OK; }
            LPWSTR raw = nullptr;
            args->get_Uri(&raw);
            const std::wstring candidate = raw ? raw : L"";
            CoTaskMemFree(raw);
            if (!trusted(candidate)) {
                args->put_Cancel(TRUE);
                BOOL user_initiated = FALSE;
                args->get_IsUserInitiated(&user_initiated);
                if (user_initiated) { open_external(candidate); }
                log("blocked navigation outside manager origin");
            }
            return S_OK;
        }).Get(), &token);
        if (FAILED(result)) { return result; }
        result = webview->add_FrameNavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>([this, lifetime](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs * args) -> HRESULT {
            if (lifetime.expired()) { return S_OK; }
            LPWSTR raw = nullptr;
            args->get_Uri(&raw);
            if (!raw || !trusted(raw)) { args->put_Cancel(TRUE); }
            CoTaskMemFree(raw);
            return S_OK;
        }).Get(), &token);
        if (FAILED(result)) { return result; }
        result = webview->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>([this, lifetime](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs * args) -> HRESULT {
            args->put_Handled(TRUE);
            if (lifetime.expired()) { return S_OK; }
            LPWSTR raw = nullptr;
            BOOL user_initiated = FALSE;
            args->get_IsUserInitiated(&user_initiated);
            args->get_Uri(&raw);
            if (raw && user_initiated) {
                if (trusted(raw)) { webview->Navigate(raw); } else { open_external(raw); }
            }
            CoTaskMemFree(raw);
            return S_OK;
        }).Get(), &token);
        if (FAILED(result)) { return result; }
        result = webview->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>([this, lifetime](ICoreWebView2 *, ICoreWebView2PermissionRequestedEventArgs * args) -> HRESULT {
            if (lifetime.expired()) { return args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY); }
            LPWSTR raw = nullptr;
            BOOL user_initiated = FALSE;
            COREWEBVIEW2_PERMISSION_KIND kind{};
            args->get_Uri(&raw);
            args->get_IsUserInitiated(&user_initiated);
            args->get_PermissionKind(&kind);
            const bool allowed = raw && trusted(raw) && user_initiated && (kind == COREWEBVIEW2_PERMISSION_KIND_MICROPHONE || kind == COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ);
            CoTaskMemFree(raw);
            return args->put_State(allowed ? COREWEBVIEW2_PERMISSION_STATE_DEFAULT : COREWEBVIEW2_PERMISSION_STATE_DENY);
        }).Get(), &token);
        if (FAILED(result)) { return result; }
        ComPtr<ICoreWebView2_4> downloads;
        result = webview.As(&downloads);
        if (FAILED(result)) { return result; }
        result = downloads->add_DownloadStarting(Callback<ICoreWebView2DownloadStartingEventHandler>([this, lifetime](ICoreWebView2 *, ICoreWebView2DownloadStartingEventArgs * args) -> HRESULT {
            if (lifetime.expired()) { return args->put_Cancel(TRUE); }
            ComPtr<ICoreWebView2DownloadOperation> operation;
            LPWSTR raw = nullptr;
            if (FAILED(args->get_DownloadOperation(&operation)) || FAILED(operation->get_Uri(&raw))) {
                return args->put_Cancel(TRUE);
            }
            const std::wstring candidate = raw ? raw : L"";
            CoTaskMemFree(raw);
            if (!trusted(candidate) && !(candidate.compare(0, 5, L"blob:") == 0 && trusted(candidate.substr(5)))) {
                return args->put_Cancel(TRUE);
            }
            return args->put_Handled(FALSE);
        }).Get(), &token);
        if (FAILED(result)) { return result; }
        result = webview->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>([this, lifetime](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs * args) -> HRESULT {
            if (lifetime.expired()) { return S_OK; }
            BOOL success = FALSE;
            args->get_IsSuccess(&success);
            if (success) {
                KillTimer(window.load(), startup_timer);
                if (!ever_ready) {
                    ShowWindow(window.load(), restore_placement());
                    SetForegroundWindow(window.load());
                }
                ever_ready = true;
                complete(true);
                log("navigation ready");
            } else {
                COREWEBVIEW2_WEB_ERROR_STATUS status{};
                args->get_WebErrorStatus(&status);
                if (status != COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED) {
                    fail("navigation", HRESULT_FROM_WIN32(ERROR_CONNECTION_ABORTED));
                }
            }
            return S_OK;
        }).Get(), &token);
        if (FAILED(result)) { return result; }
        result = webview->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>([this, lifetime](ICoreWebView2 *, ICoreWebView2ProcessFailedEventArgs *) -> HRESULT {
            if (lifetime.expired()) { return S_OK; }
            if (recovered) {
                fail("webview process", E_FAIL);
            } else {
                recovered = true;
                complete(false);
                log("webview process failed; recreating once");
                PostMessageW(window.load(), recover_message, 0, 0);
            }
            return S_OK;
        }).Get(), &token);
        if (FAILED(result)) { return result; }
        resize();
        result = controller->put_IsVisible(IsWindowVisible(window.load()) && !IsIconic(window.load()));
        if (FAILED(result)) { return result; }
        return webview->Navigate(url.c_str());
    }

    void initialize_webview() {
        if (stop_requested) { return; }
        callbacks = std::make_shared<int>(0);
        const std::weak_ptr<int> lifetime = callbacks;
        auto environment_options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        if (!environment_options) { fail("environment options", E_OUTOFMEMORY); return; }
        environment_options->put_ReleaseChannels(COREWEBVIEW2_RELEASE_CHANNELS_STABLE);
        SetTimer(window.load(), startup_timer, 20000, nullptr);
        const HRESULT result = create_environment(nullptr, config.user_data_directory.c_str(), environment_options.Get(),
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([this, lifetime](HRESULT status, ICoreWebView2Environment * created) -> HRESULT {
                if (lifetime.expired()) { return S_OK; }
                if (FAILED(status) || !created) { fail("environment", FAILED(status) ? status : E_FAIL); return S_OK; }
                environment = created;
                const HRESULT requested = environment->CreateCoreWebView2Controller(window.load(), Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([this, lifetime](HRESULT controller_status, ICoreWebView2Controller * created_controller) -> HRESULT {
                    if (lifetime.expired()) { return S_OK; }
                    if (FAILED(controller_status) || !created_controller) { fail("controller", FAILED(controller_status) ? controller_status : E_FAIL); return S_OK; }
                    controller = created_controller;
                    const HRESULT configured = configure_webview();
                    if (FAILED(configured)) { fail("configuration", configured); }
                    return S_OK;
                }).Get());
                if (FAILED(requested)) { fail("controller request", requested); }
                return S_OK;
            }).Get());
        if (FAILED(result)) { fail("environment request", result); }
    }

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto * self = reinterpret_cast<implementation *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<implementation *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->window.store(hwnd);
        }
        if (!self) { return DefWindowProcW(hwnd, message, wparam, lparam); }
        switch (message) {
            case show_message:
                ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
                SetForegroundWindow(hwnd);
                if (self->controller) { self->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC); }
                return 0;
            case WM_SIZE:
                if (self->controller) { self->controller->put_IsVisible(wparam != SIZE_MINIMIZED && IsWindowVisible(hwnd)); }
                self->resize();
                return 0;
            case WM_SHOWWINDOW:
                if (self->controller) { self->controller->put_IsVisible(wparam != 0 && !IsIconic(hwnd)); }
                break;
            case WM_MOVE:
                self->resize();
                return 0;
            case WM_SETFOCUS:
                if (self->controller) { self->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC); }
                return 0;
            case WM_GETMINMAXINFO: {
                auto * bounds = reinterpret_cast<MINMAXINFO *>(lparam);
                const UINT dpi = GetDpiForWindow(hwnd);
                bounds->ptMinTrackSize = {MulDiv(720, dpi, 96), MulDiv(480, dpi, 96)};
                MONITORINFO monitor{sizeof(monitor)};
                if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
                    bounds->ptMinTrackSize.x = std::min(bounds->ptMinTrackSize.x, monitor.rcWork.right - monitor.rcWork.left);
                    bounds->ptMinTrackSize.y = std::min(bounds->ptMinTrackSize.y, monitor.rcWork.bottom - monitor.rcWork.top);
                }
                return 0;
            }
            case WM_DPICHANGED: {
                const auto * bounds = reinterpret_cast<RECT *>(lparam);
                SetWindowPos(hwnd, nullptr, bounds->left, bounds->top, bounds->right - bounds->left, bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
                self->resize();
                return 0;
            }
            case WM_SETTINGCHANGE:
                self->theme();
                break;
            case WM_TIMER:
                if (wparam == startup_timer) { self->fail("startup timeout", HRESULT_FROM_WIN32(ERROR_TIMEOUT)); }
                return 0;
            case recover_message:
                self->close_webview();
                self->initialize_webview();
                return 0;
            case WM_QUERYENDSESSION:
                return TRUE;
            case WM_ENDSESSION:
                if (wparam && self->config.request_shutdown) { self->config.request_shutdown(); }
                return 0;
            case WM_CLOSE:
                if (self->config.keep_in_background && self->config.keep_in_background()) {
                    self->save_placement();
                    ShowWindow(hwnd, SW_HIDE);
                    return 0;
                }
                if (self->config.has_active_requests && self->config.has_active_requests() && MessageBoxW(hwnd, self->config.use_hebrew && self->config.use_hebrew() ? L"בקשה עדיין פעילה. לצאת מ־Spark Edge ולהפסיק אותה?" : L"A request is still running. Exit Spark Edge and stop it?", L"Spark Edge", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
                    return 0;
                }
                if (self->config.request_shutdown) { self->config.request_shutdown(); }
                [[fallthrough]];
            case stop_message:
                self->stop_requested.store(true);
                if (self->ever_ready) { self->save_placement(); }
                self->close_webview();
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                self->window.store(nullptr);
                PostQuitMessage(0);
                return 0;
            default:
                break;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void run() {
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(apartment)) { fail("COM apartment", apartment); return; }
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        std::error_code error;
        std::filesystem::create_directories(config.user_data_directory, error);
        if (error) {
            fail("user data directory", HRESULT_FROM_WIN32(error.value()));
            CoUninitialize();
            return;
        }
        wchar_t executable[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
        if (length > 0 && length < ARRAYSIZE(executable)) {
            const auto path = std::filesystem::path(executable).parent_path() / L"WebView2Loader.dll";
            loader = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
        auto version = loader ? reinterpret_cast<runtime_version_fn>(GetProcAddress(loader, "GetAvailableCoreWebView2BrowserVersionString")) : nullptr;
        create_environment = loader ? reinterpret_cast<create_environment_fn>(GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions")) : nullptr;
        LPWSTR runtime = nullptr;
        const HRESULT runtime_result = version ? version(nullptr, &runtime) : HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
        const bool available = SUCCEEDED(runtime_result) && runtime && *runtime && create_environment;
        CoTaskMemFree(runtime);
        if (available && !stop_requested) {
            WNDCLASSEXW definition{sizeof(definition)};
            definition.lpfnWndProc = window_proc;
            definition.hInstance = GetModuleHandleW(nullptr);
            definition.hIcon = LoadIconW(definition.hInstance, MAKEINTRESOURCEW(101));
            definition.hIconSm = definition.hIcon;
            definition.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
            definition.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            definition.lpszClassName = class_name;
            RegisterClassExW(&definition);
            const HWND hwnd = CreateWindowExW(0, class_name, L"Spark Edge", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800, nullptr, nullptr, definition.hInstance, this);
            if (hwnd) {
                theme();
                initialize_webview();
                if (stop_requested) { PostMessageW(hwnd, stop_message, 0, 0); }
                MSG message{};
                while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            } else {
                fail("window creation", HRESULT_FROM_WIN32(GetLastError()));
            }
            UnregisterClassW(class_name, definition.hInstance);
        } else {
            fail("Runtime detection", FAILED(runtime_result) ? runtime_result : E_FAIL);
            log("WebView2 loader or Runtime unavailable; use browser fallback");
        }
        close_webview();
        complete(false);
        CoUninitialize();
        if (loader) { FreeLibrary(loader); loader = nullptr; }
    }
};

desktop_window::desktop_window() : impl_(std::make_unique<implementation>()) {}
desktop_window::~desktop_window() { stop(); }

bool desktop_window::start(options config) {
    stop();
    impl_ = std::make_unique<implementation>();
    impl_->config = std::move(config);
    const std::string prefix = "http://127.0.0.1:";
    if (impl_->config.url.compare(0, prefix.size(), prefix) != 0) {
        impl_->log("invalid manager URL");
        return false;
    }
    const auto end = impl_->config.url.find('/', prefix.size());
    const auto port = impl_->config.url.substr(prefix.size(), end == std::string::npos ? end : end - prefix.size());
    if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos || std::stoi(port) < 1 || std::stoi(port) > 65535) {
        impl_->log("invalid manager URL");
        return false;
    }
    impl_->url.assign(impl_->config.url.begin(), impl_->config.url.end());
    impl_->origin = L"http://127.0.0.1:" + std::wstring(port.begin(), port.end());
    impl_->thread = std::thread([this]() { impl_->run(); });
    std::unique_lock<std::mutex> lock(impl_->mutex);
    const bool signaled = impl_->condition.wait_for(lock, std::chrono::seconds(25), [this]() { return impl_->initialized; });
    const bool ready = signaled && impl_->ready;
    lock.unlock();
    if (!ready) { stop(); }
    return ready;
}

bool desktop_window::show() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const HWND hwnd = impl_->window.load();
    return impl_->ready && hwnd && PostMessageW(hwnd, implementation::show_message, 0, 0);
}

void desktop_window::stop() {
    impl_->stop_requested.store(true);
    const HWND hwnd = impl_->window.load();
    if (hwnd) { PostMessageW(hwnd, implementation::stop_message, 0, 0); }
    if (impl_->thread.joinable()) { impl_->thread.join(); }
}

void * desktop_window::native_handle() const { return impl_->window.load(); }

}

#else

namespace sparkmoe {
struct desktop_window::implementation {};
desktop_window::desktop_window() : impl_(std::make_unique<implementation>()) {}
desktop_window::~desktop_window() = default;
bool desktop_window::start(options) { return false; }
bool desktop_window::show() { return false; }
void desktop_window::stop() {}
void * desktop_window::native_handle() const { return nullptr; }
}

#endif
