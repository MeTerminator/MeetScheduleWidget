#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include "course_data.hpp"

namespace fs = std::filesystem;
using namespace Gdiplus;

// 主屏幕完整桌面划分为 8×8 网格；占第 7 行、第 7–8 列，向右延伸至屏幕边缘。
constexpr double WidthRatio = 0.25, HeightRatio = 0.125;
constexpr double CenterX = 0.875, CenterY = 0.8125;
constexpr UINT DataMessage = WM_APP + 1, TrayMessage = WM_APP + 2;
constexpr UINT RefreshCommand = 101, StatusCommand = 102, ExitCommand = 103;
HWND windowHandle = nullptr;
HWND desktopHost = nullptr;
UINT taskbarCreated = 0;
int widgetWidth = 0, widgetHeight = 0;
float dpiScale = 1;
bool attachedToDesktop = false, busy = false;
int64_t refreshAt = 0;
std::thread worker;
std::vector<courses::Course> currentCourses;
courses::Display currentDisplay{"正在读取课程", "", ""};
std::wstring statusText = L"正在读取课程";
fs::path cachePath;
std::string feedUrl;
ULONG_PTR gdiplusToken = 0;

std::wstring wide(const std::string& value) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), (int)value.size(),
                                     nullptr, 0);
    if (!length && !value.empty())
        return L"无法显示的文字";
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), (int)value.size(),
                        result.data(), length);
    return result;
}

std::runtime_error windowsError(const char* operation) {
    return std::runtime_error(std::string(operation) + "（Windows 错误 " +
                              std::to_string(GetLastError()) + "）");
}

bool loadFeedUrl() {
    // 使用 exe 所在目录，双击、快捷方式或开机启动时路径都一致。
    std::vector<wchar_t> executable(32768);
    DWORD length = GetModuleFileNameW(nullptr, executable.data(), (DWORD)executable.size());
    if (!length || length >= executable.size()) {
        MessageBoxW(nullptr, L"无法确定程序目录。", L"课程小组件", MB_OK | MB_ICONERROR);
        return false;
    }
    auto filePath =
        fs::path(std::wstring(executable.data(), length)).parent_path() / L"meetics.txt";
    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        std::wstring message =
            L"未找到或无法读取 meetics.txt。\n\n请在程序同目录创建 meetics.txt，"
            L"用 UTF-8 编码保存，并在文件中填写一行完整的 HTTPS 日历订阅链接。\n\n文件位置：\n" +
            filePath.wstring() + L"\n\n创建后重新运行程序。";
        MessageBoxW(nullptr, message.c_str(), L"请创建 meetics.txt", MB_OK | MB_ICONWARNING);
        return false;
    }
    feedUrl.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (feedUrl.compare(0, 3, "\xEF\xBB\xBF") == 0)
        feedUrl.erase(0, 3);
    feedUrl = courses::trim(feedUrl);
    auto url = wide(feedUrl);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = (DWORD)-1;
    if (feedUrl.empty() || feedUrl.find_first_of(" \t\r\n") != std::string::npos ||
        feedUrl.find('\0') != std::string::npos || !WinHttpCrackUrl(url.c_str(), 0, 0, &parts) ||
        parts.nScheme != INTERNET_SCHEME_HTTPS || !parts.dwHostNameLength) {
        std::wstring message =
            L"meetics.txt 内容无效。\n\n请用 UTF-8 编码填写一行完整的 HTTPS 日历订阅链接，"
            L"不要添加引号或其他文字。\n\n文件位置：\n" +
            filePath.wstring();
        MessageBoxW(nullptr, message.c_str(), L"请检查 meetics.txt", MB_OK | MB_ICONWARNING);
        return false;
    }
    return true;
}

struct InternetHandle {
    HINTERNET handle = nullptr;
    explicit InternetHandle(HINTERNET value) : handle(value) {
        if (!handle)
            throw windowsError("网络初始化失败");
    }
    ~InternetHandle() {
        WinHttpCloseHandle(handle);
    }
    operator HINTERNET() const {
        return handle;
    }
};

std::string fetchIcs() {
    auto url = wide(feedUrl);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
        throw windowsError("订阅地址无效");
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength)
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    InternetHandle session(WinHttpOpen(L"MeetScheduleWidget/1.0",
                                       WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS, 0));
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);
    InternetHandle connection(WinHttpConnect(session, host.c_str(), parts.nPort, 0));
    InternetHandle request(WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              WINHTTP_FLAG_SECURE));
    // 系统 HTTPS 证书校验保持开启。
    if (!WinHttpSendRequest(request, L"Accept: text/calendar\r\n", (DWORD)-1,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr))
        throw windowsError("无法获取课程日历");
    DWORD code = 0, length = sizeof(code);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &code, &length, WINHTTP_NO_HEADER_INDEX))
        throw windowsError("无法读取服务器状态");
    if (code != 200)
        throw std::runtime_error("日历服务器返回 HTTP " + std::to_string(code));
    std::string body;
    char buffer[16384];
    DWORD read = 0;
    do {
        if (!WinHttpReadData(request, buffer, sizeof(buffer), &read))
            throw windowsError("下载日历失败");
        body.append(buffer, read);
        if (body.size() > 8 * 1024 * 1024)
            throw std::runtime_error("日历大小超过 8 MB");
    } while (read);
    return body;
}

struct LoadResult {
    std::vector<courses::Course> list;
    int64_t nextRefresh = 0;
    bool available = false;
    std::wstring status;
};

void refresh(bool force = false) {
    if (busy)
        return;
    if (worker.joinable())
        worker.join();
    busy = true;
    worker = std::thread([force] {
        auto result = std::make_unique<LoadResult>();
        int64_t fetched = 0;
        try {
            std::ifstream file(cachePath, std::ios::binary);
            if (file) {
                auto cache = courses::Json::parse(file);
                result->list = courses::readCache(cache, feedUrl);
                fetched = cache.at("fetched_at").get<int64_t>();
                result->available = true;
            }
        } catch (...) { /* 损坏或旧版本缓存会自动重新下载。 */
        }
        auto at = courses::now();
        if (!force && result->available && fetched <= at && at - fetched < courses::CacheSeconds) {
            result->nextRefresh = fetched + courses::CacheSeconds;
            result->status = L"使用本地缓存；有效期 5 分钟。";
        } else {
            try {
                auto list = courses::parseIcs(fetchIcs());
                fetched = courses::now();
                result->list = std::move(list);
                result->available = true;
                result->nextRefresh = fetched + courses::CacheSeconds;
                result->status = L"课程同步成功；5 分钟后自动更新。";
                try {
                    auto temporary = cachePath;
                    temporary += L".tmp";
                    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
                    auto json = courses::makeCache(result->list, fetched, feedUrl).dump(2);
                    file.write(json.data(), (std::streamsize)json.size());
                    file.close();
                    if (!file || !MoveFileExW(temporary.c_str(), cachePath.c_str(),
                                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                        throw std::runtime_error("缓存无法写入");
                } catch (...) {
                    result->status += L" 本地缓存写入失败，当前使用内存数据。";
                }
            } catch (const std::exception& e) {
                result->nextRefresh = courses::now() + 60;
                result->status = wide(e.what()) + L"\n将在 1 分钟后重试。";
                if (result->available)
                    result->status += L"\n当前显示上次缓存的数据（已过期）。";
            }
        }
        auto pointer = result.release();
        if (!PostMessageW(windowHandle, DataMessage, 0, (LPARAM)pointer))
            delete pointer;
    });
}

void tray(bool add) {
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = windowHandle;
    icon.uID = 1;
    icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    icon.uCallbackMessage = TrayMessage;
    icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(icon.szTip, L"课程小组件 · 右键刷新或退出");
    Shell_NotifyIconW(add ? NIM_ADD : NIM_DELETE, &icon);
}

BOOL CALLBACK findDesktop(HWND candidate, LPARAM output) {
    if (FindWindowExW(candidate, nullptr, L"SHELLDLL_DefView", nullptr)) {
        HWND host = FindWindowExW(nullptr, candidate, L"WorkerW", nullptr);
        if (host) {
            *reinterpret_cast<HWND*>(output) = host;
            return FALSE;
        }
    }
    return TRUE;
}

void attachDesktop() {
    // Explorer 没有公开的桌面小组件 API。优先使用壁纸 WorkerW，失败则保留置底窗口。
    HWND host = nullptr;
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) {
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_ABORTIFHUNG, 500, &ignored);
        EnumWindows(findDesktop, (LPARAM)&host);
    }
    attachedToDesktop = false;
    if (host) {
        SetLastError(0);
        HWND previous = SetParent(windowHandle, host);
        if (previous || GetLastError() == 0) {
            auto style = GetWindowLongPtrW(windowHandle, GWL_STYLE);
            SetWindowLongPtrW(windowHandle, GWL_STYLE, (style & ~WS_POPUP) | WS_CHILD);
            attachedToDesktop = true;
        }
    }
    desktopHost = attachedToDesktop ? host : nullptr;
}

void positionWidget() {
    POINT origin{};
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &monitor);
    auto area = monitor.rcMonitor;
    int width = area.right - area.left, height = area.bottom - area.top;
    widgetWidth = std::max(1, (int)(width * WidthRatio));
    widgetHeight = std::max(1, (int)(height * HeightRatio));
    dpiScale = GetDpiForWindow(windowHandle) / 96.0f;
    POINT point{area.left + (int)(width * CenterX) - widgetWidth / 2,
                area.top + (int)(height * CenterY) - widgetHeight / 2};
    if (attachedToDesktop)
        ScreenToClient(desktopHost, &point);
    SetWindowPos(windowHandle, HWND_BOTTOM, point.x, point.y, widgetWidth, widgetHeight,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
}

void paint() {
    if (widgetWidth <= 0 || widgetHeight <= 0)
        return;
    HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = widgetWidth;
    info.bmiHeader.biHeight = -widgetHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP surface = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!surface) {
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ previous = SelectObject(memory, surface);
    {
        Bitmap bitmap(widgetWidth, widgetHeight, widgetWidth * 4, PixelFormat32bppPARGB,
                      (BYTE*)pixels);
        Graphics graphics(&bitmap);
        graphics.Clear(Color(0, 0, 0, 0));
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        // 每行独立选取可容纳完整文字的最大字号，保留少量边距。
        float padding = std::max(1.0f, std::min(widgetWidth, widgetHeight) * 0.025f);
        float gap = std::max(1.0f, widgetHeight * 0.015f);
        float shadowOffset = std::max(1.0f, dpiScale);
        float availableWidth = std::max(1.0f, widgetWidth - 2 * padding - shadowOffset);
        float rowHeight = std::max(1.0f, (widgetHeight - 2 * padding - 2 * gap) / 3 - shadowOffset);
        std::wstring rows[] = {wide(currentDisplay.first), wide(currentDisplay.title),
                               wide(currentDisplay.subtitle)};
        std::wstring sizingRows[] = {rows[0], rows[1], rows[2]};
        // 微软雅黑数字等宽；以同位数的 8 测量，避免倒计时每秒变化时字号抖动。
        for (auto& character : sizingRows[0])
            if (character >= L'0' && character <= L'9')
                character = L'8';
        StringFormat format;
        format.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
        format.SetTrimming(StringTrimmingNone);
        auto measure = [&](const std::wstring& content, Font& font) {
            RectF bounds;
            graphics.MeasureString(content.c_str(), -1, &font, PointF(0, 0), &format, &bounds);
            return bounds;
        };
        // 字体测量只在内容、窗口尺寸或 DPI 变化时执行。
        static std::wstring previousSizing[3];
        static int previousWidth = 0, previousHeight = 0;
        static float previousDpi = 0, sizes[3] = {};
        for (int row = 0; row < 3; ++row) {
            if (previousWidth != widgetWidth || previousHeight != widgetHeight ||
                previousDpi != dpiScale || previousSizing[row] != sizingRows[row]) {
                float low = 0.5f, high = rowHeight;
                for (int step = 0; step < 14; ++step) {
                    float candidate = (low + high) / 2;
                    Font regular(L"Microsoft YaHei", candidate, FontStyleRegular, UnitPixel);
                    Font bold(L"Microsoft YaHei", candidate, FontStyleBold, UnitPixel);
                    float width = 0, height = std::max(regular.GetHeight(&graphics),
                                                       bold.GetHeight(&graphics));
                    auto space = sizingRows[row].find(L' ');
                    if (row == 0 && space != std::wstring::npos) {
                        auto label = measure(sizingRows[row].substr(0, space + 1), regular);
                        auto timer = measure(sizingRows[row].substr(space + 1), bold);
                        width = label.Width + timer.Width;
                        height = std::max(height, std::max(label.Height, timer.Height));
                    } else if (!sizingRows[row].empty()) {
                        auto bounds = measure(sizingRows[row], row == 1 ? bold : regular);
                        width = bounds.Width;
                        height = std::max(height, bounds.Height);
                    }
                    if (width <= availableWidth && height <= rowHeight)
                        low = candidate;
                    else
                        high = candidate;
                }
                sizes[row] = low;
                previousSizing[row] = sizingRows[row];
            }
        }
        previousWidth = widgetWidth;
        previousHeight = widgetHeight;
        previousDpi = dpiScale;
        SolidBrush text(Color(255, 255, 255, 255)), shadow(Color(210, 0, 0, 0));
        for (int row = 0; row < 3; ++row) {
            if (rows[row].empty())
                continue;
            Font regular(L"Microsoft YaHei", sizes[row], FontStyleRegular, UnitPixel);
            Font bold(L"Microsoft YaHei", sizes[row], FontStyleBold, UnitPixel);
            float textHeight = std::max(regular.GetHeight(&graphics), bold.GetHeight(&graphics));
            float top =
                padding + row * (rowHeight + shadowOffset + gap) + (rowHeight - textHeight) / 2;
            auto draw = [&](const std::wstring& content, Font& font, float x) {
                RectF rect(x, top, availableWidth - (x - padding), rowHeight);
                RectF shade = rect;
                shade.X += shadowOffset;
                shade.Y += shadowOffset;
                graphics.DrawString(content.c_str(), -1, &font, shade, &format, &shadow);
                graphics.DrawString(content.c_str(), -1, &font, rect, &format, &text);
            };
            auto space = rows[row].find(L' ');
            if (row == 0 && space != std::wstring::npos) {
                auto label = rows[row].substr(0, space + 1), timer = rows[row].substr(space + 1);
                draw(label, regular, padding);
                draw(timer, bold, padding + measure(label, regular).Width);
            } else
                draw(rows[row], row == 1 ? bold : regular, padding);
        }
    }
    POINT source{};
    SIZE size{widgetWidth, widgetHeight};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(windowHandle, screen, nullptr, &size, memory, &source, 0, &blend,
                        ULW_ALPHA);
    SelectObject(memory, previous);
    DeleteObject(surface);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

void menu() {
    POINT cursor;
    GetCursorPos(&cursor);
    HMENU popup = CreatePopupMenu();
    AppendMenuW(popup, MF_STRING | (busy ? MF_GRAYED : 0), RefreshCommand, L"立即刷新");
    AppendMenuW(popup, MF_STRING, StatusCommand, L"同步状态");
    AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(popup, MF_STRING, ExitCommand, L"退出");
    // 前台隐藏的消息窗口让托盘菜单可正常关闭，同时不提升桌面组件层级。
    HWND menuOwner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP, 0, 0, 0, 0,
                                     nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetForegroundWindow(menuOwner);
    UINT selected = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0,
                                   menuOwner, nullptr);
    DestroyMenu(popup);
    DestroyWindow(menuOwner);
    if (selected == RefreshCommand)
        refresh(true);
    if (selected == StatusCommand) {
        std::wstring text = statusText + L"\n\n缓存文件：\n" + cachePath.wstring() +
                            (attachedToDesktop ? L"\n\n窗口：桌面层" : L"\n\n窗口：普通置底模式");
        MessageBoxW(nullptr, text.c_str(), L"课程小组件", MB_OK | MB_ICONINFORMATION);
    }
    if (selected == ExitCommand)
        DestroyWindow(windowHandle);
}

LRESULT CALLBACK windowProcedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (taskbarCreated && message == taskbarCreated) {
        tray(true);
        attachDesktop();
        positionWidget();
        paint();
        return 0;
    }
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        paint();
        return 0;
    }
    case WM_CONTEXTMENU:
        menu();
        return 0;
    case TrayMessage:
        if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU)
            menu();
        return 0;
    case WM_WINDOWPOSCHANGING: {
        auto change = reinterpret_cast<WINDOWPOS*>(lparam);
        if (!(change->flags & SWP_NOZORDER))
            change->hwndInsertAfter = HWND_BOTTOM;
        return 0;
    }
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
    case WM_SETTINGCHANGE:
        positionWidget();
        paint();
        return 0;
    case DataMessage: {
        std::unique_ptr<LoadResult> result(reinterpret_cast<LoadResult*>(lparam));
        busy = false;
        refreshAt = result->nextRefresh;
        statusText = result->status;
        if (result->available) {
            currentCourses = std::move(result->list);
            currentDisplay = courses::display(currentCourses, courses::now());
        } else if (currentCourses.empty()) {
            currentDisplay = {"暂时无法读取课程", "请检查网络连接", "稍后自动重试"};
        }
        paint();
        return 0;
    }
    case WM_TIMER: {
        auto at = courses::now();
        if (!currentCourses.empty())
            currentDisplay = courses::display(currentCourses, at);
        // 在跨过上下课边界时即时换课；只有小窗口重绘，每秒一次。
        paint();
        if (!busy && at >= refreshAt)
            refresh(true);
        return 0;
    }
    case WM_DESTROY: {
        KillTimer(hwnd, 1);
        tray(false);
        if (worker.joinable())
            worker.join();
        MSG pending;
        while (PeekMessageW(&pending, hwnd, DataMessage, DataMessage, PM_REMOVE))
            delete reinterpret_cast<LoadResult*>(pending.lParam);
        PostQuitMessage(0);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\MeetScheduleWidget.Native.v1");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex)
            CloseHandle(mutex);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!loadFeedUrl()) {
        CloseHandle(mutex);
        return 1;
    }
    wchar_t local[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, 0,
                                local))) {
        MessageBoxW(nullptr, L"无法访问本地数据目录。", L"课程小组件", MB_OK | MB_ICONERROR);
        CloseHandle(mutex);
        return 1;
    }
    std::error_code error;
    auto directory = fs::path(local) / L"MeetScheduleWidget";
    fs::create_directories(directory, error);
    cachePath = directory / L"course_cache.json";
    GdiplusStartupInput startup;
    if (GdiplusStartup(&gdiplusToken, &startup, nullptr) != Ok) {
        CloseHandle(mutex);
        return 1;
    }
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW klass{};
    klass.lpfnWndProc = windowProcedure;
    klass.hInstance = instance;
    klass.lpszClassName = L"MeetScheduleDesktopWidget";
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&klass);
    windowHandle =
        CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, klass.lpszClassName,
                        L"课程小组件", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    if (!windowHandle) {
        GdiplusShutdown(gdiplusToken);
        CloseHandle(mutex);
        return 1;
    }
    attachDesktop();
    positionWidget();
    paint();
    tray(true);
    SetTimer(windowHandle, 1, 1000, nullptr);
    refresh();
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    GdiplusShutdown(gdiplusToken);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
