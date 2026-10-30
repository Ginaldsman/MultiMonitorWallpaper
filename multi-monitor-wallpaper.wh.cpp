// ==WindhawkMod==
// @id              multi-monitor-wallpaper
// @name            Papel de parede por monitor
// @description     Mantém imagens diferentes em até três monitores, inclusive ao alternar entre áreas de trabalho virtuais
// @version         0.1.0
// @author          Local
// @include         explorer.exe
// @compilerOptions -lole32 -luuid -lwtsapi32 -luser32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Papel de parede por monitor

Aplica uma imagem local a cada uma das três entradas e mantém o mesmo conjunto
em todas as áreas de trabalho virtuais. Não altera a tela de bloqueio, não usa
hooks e não cria processos auxiliares.

Para uma associação estável, ative o diagnóstico, salve as configurações e copie
o `device path` de cada monitor para a entrada correspondente. Sem identificador,
a entrada é associada pela posição retornada por `IDesktopWallpaper`, que pode
mudar quando o layout dos monitores muda.

O seletor visual de arquivos está disponível no Windhawk 2.0. No Windhawk 1.7.3,
digite o caminho local completo, por exemplo `C:\Imagens\monitor1.jpg`.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- enabled: true
  $name: Gerenciar papéis de parede
  $description: Desativar interrompe as alterações sem restaurar ou apagar imagens.
- monitor1Image: ""
  $name: Entrada 1 - imagem
  $description: Caminho local completo da imagem.
  #! $format: filePath
- monitor1DevicePath: ""
  $name: Entrada 1 - device path (opcional)
  $description: Identificador estável exibido no diagnóstico; recomendado.
- monitor2Image: ""
  $name: Entrada 2 - imagem
  $description: Caminho local completo da imagem.
  #! $format: filePath
- monitor2DevicePath: ""
  $name: Entrada 2 - device path (opcional)
  $description: Identificador estável exibido no diagnóstico; recomendado.
- monitor3Image: ""
  $name: Entrada 3 - imagem
  $description: Caminho local completo da imagem.
  #! $format: filePath
- monitor3DevicePath: ""
  $name: Entrada 3 - device path (opcional)
  $description: Identificador estável exibido no diagnóstico; recomendado.
- pollIntervalMs: 2000
  $name: Intervalo de verificação (ms)
  $description: Fallback para trocas de área de trabalho virtual; valores são limitados entre 1000 e 60000.
  #! $min: 1000
  #! $max: 60000
- diagnosticLogging: false
  $name: Registrar diagnóstico
  $description: Registra monitores, identificadores, coordenadas, imagens e associações no log do módulo.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <shobjidl.h>
#include <wtsapi32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cwctype>
#include <string>

namespace {

constexpr UINT kDebounceTimer = 2;
constexpr UINT kDebounceMs = 750;
constexpr DWORD kMinPollIntervalMs = 1000;
constexpr DWORD kMaxPollIntervalMs = 60000;
constexpr PCWSTR kWindowClassName = L"WindhawkMultiMonitorWallpaper_0_1";

struct MonitorSlot {
    std::wstring imagePath;
    std::wstring devicePath;
};

struct Settings {
    bool enabled = true;
    std::array<MonitorSlot, 3> monitors;
    DWORD pollIntervalMs = 2000;
    bool diagnosticLogging = false;
};

struct WorkerContext {
    HWND window = nullptr;
    ATOM windowClassAtom = 0;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    IDesktopWallpaper* desktopWallpaper = nullptr;
    bool sessionNotificationsRegistered = false;
    bool diagnosticsDirty = true;
    HRESULT lastComError = S_OK;
    std::wstring lastWindowIssue;
    std::array<std::wstring, 3> lastSlotIssues;
};

Settings g_settings;
SRWLOCK g_settingsLock = SRWLOCK_INIT;
HANDLE g_wakeEvent = nullptr;
HANDLE g_workerThread = nullptr;
std::atomic<bool> g_stop = false;

struct SRWLockGuard {
    PSRWLOCK lock;
    bool exclusive;

    SRWLockGuard(PSRWLOCK lock, bool exclusive)
        : lock(lock), exclusive(exclusive) {
        if (exclusive) {
            AcquireSRWLockExclusive(lock);
        } else {
            AcquireSRWLockShared(lock);
        }
    }

    ~SRWLockGuard() {
        if (exclusive) {
            ReleaseSRWLockExclusive(lock);
        } else {
            ReleaseSRWLockShared(lock);
        }
    }

    SRWLockGuard(const SRWLockGuard&) = delete;
    SRWLockGuard& operator=(const SRWLockGuard&) = delete;
};

std::wstring Trim(std::wstring value) {
    auto isSpace = [](wchar_t c) { return std::iswspace(c) != 0; };
    value.erase(value.begin(),
                std::find_if_not(value.begin(), value.end(), isSpace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), isSpace).base(),
                value.end());
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

std::wstring GetStringSetting(PCWSTR name) {
    PCWSTR value = Wh_GetStringSetting(name);
    std::wstring result = Trim(value ? value : L"");
    Wh_FreeStringSetting(value);
    return result;
}

void LoadSettings() {
    Settings settings;
    settings.enabled = Wh_GetIntSetting(L"enabled", 1) != 0;
    settings.pollIntervalMs = static_cast<DWORD>(
        std::clamp(Wh_GetIntSetting(L"pollIntervalMs", 2000),
                   static_cast<int>(kMinPollIntervalMs),
                   static_cast<int>(kMaxPollIntervalMs)));
    settings.diagnosticLogging =
        Wh_GetIntSetting(L"diagnosticLogging", 0) != 0;

    for (size_t i = 0; i < settings.monitors.size(); ++i) {
        wchar_t name[64];
        swprintf_s(name, L"monitor%zuImage", i + 1);
        settings.monitors[i].imagePath = GetStringSetting(name);
        swprintf_s(name, L"monitor%zuDevicePath", i + 1);
        settings.monitors[i].devicePath = GetStringSetting(name);
    }

    SRWLockGuard lock(&g_settingsLock, true);
    g_settings = std::move(settings);
}

Settings GetSettings() {
    SRWLockGuard lock(&g_settingsLock, false);
    return g_settings;
}

bool PathsEqual(PCWSTR left, PCWSTR right) {
    return CompareStringOrdinal(left, -1, right, -1, TRUE) == CSTR_EQUAL;
}

bool IsLocalAbsolutePath(const std::wstring& path) {
    if (path.size() < 3 || !std::iswalpha(path[0]) || path[1] != L':' ||
        (path[2] != L'\\' && path[2] != L'/')) {
        return false;
    }

    wchar_t root[] = {path[0], L':', L'\\', L'\0'};
    UINT driveType = GetDriveTypeW(root);
    return driveType != DRIVE_REMOTE && driveType != DRIVE_NO_ROOT_DIR;
}

void ReportSlotIssue(WorkerContext& context, size_t slot,
                     const std::wstring& key, PCWSTR message, HRESULT hr) {
    if (context.lastSlotIssues[slot] == key) {
        return;
    }
    context.lastSlotIssues[slot] = key;
    if (FAILED(hr)) {
        Wh_Log(L"%s (HRESULT=0x%08X)", message,
               static_cast<unsigned int>(hr));
    } else {
        Wh_Log(L"%s", message);
    }
}

void ApplyWallpapers(WorkerContext& context);

void ScheduleApply(WorkerContext& context, bool diagnosticsChanged = false) {
    if (diagnosticsChanged) {
        context.diagnosticsDirty = true;
    }
    if (!context.window ||
        !SetTimer(context.window, kDebounceTimer, kDebounceMs, nullptr)) {
        ApplyWallpapers(context);
    }
}

void ReportWindowIssue(WorkerContext& context, const std::wstring& key,
                       PCWSTR message, DWORD error) {
    if (context.lastWindowIssue == key) {
        return;
    }
    context.lastWindowIssue = key;
    Wh_Log(L"%s (erro=%lu)", message, error);
}

LRESULT CALLBACK WorkerWindowProc(HWND window, UINT message, WPARAM wParam,
                                  LPARAM lParam);

void EnsureWorkerWindow(WorkerContext& context) {
    if (!context.window && !context.windowClassAtom) {
        WNDCLASSEXW windowClass = {
            .cbSize = sizeof(windowClass),
            .lpfnWndProc = WorkerWindowProc,
            .hInstance = context.instance,
            .lpszClassName = kWindowClassName,
        };
        context.windowClassAtom = RegisterClassExW(&windowClass);
        if (!context.windowClassAtom) {
            DWORD error = GetLastError();
            ReportWindowIssue(context, L"class:" + std::to_wstring(error),
                              L"RegisterClassEx falhou", error);
            return;
        }
    }

    if (!context.window) {
        context.window = CreateWindowExW(
            WS_EX_TOOLWINDOW, kWindowClassName, L"", WS_POPUP, 0, 0, 0, 0,
            nullptr, nullptr, context.instance, nullptr);
        if (!context.window) {
            DWORD error = GetLastError();
            ReportWindowIssue(context, L"window:" + std::to_wstring(error),
                              L"CreateWindowEx falhou", error);
            return;
        }
        SetWindowLongPtrW(context.window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(&context));
    }

    if (!context.sessionNotificationsRegistered) {
        context.sessionNotificationsRegistered =
            WTSRegisterSessionNotification(context.window,
                                           NOTIFY_FOR_THIS_SESSION) != 0;
        if (!context.sessionNotificationsRegistered) {
            DWORD error = GetLastError();
            ReportWindowIssue(context, L"wts:" + std::to_wstring(error),
                              L"WTSRegisterSessionNotification falhou", error);
            return;
        }
    }

    context.lastWindowIssue.clear();
}

bool EnsureDesktopWallpaper(WorkerContext& context) {
    if (context.desktopWallpaper) {
        return true;
    }

    HRESULT hr = CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&context.desktopWallpaper));
    if (FAILED(hr)) {
        if (context.lastComError != hr) {
            Wh_Log(L"Falha ao criar IDesktopWallpaper (HRESULT=0x%08X)",
                   static_cast<unsigned int>(hr));
            context.lastComError = hr;
        }
        context.desktopWallpaper = nullptr;
        return false;
    }

    context.lastComError = S_OK;
    return true;
}

size_t FindSlotForMonitor(const Settings& settings, UINT monitorIndex,
                          PCWSTR devicePath,
                          const std::array<bool, 3>& usedSlots) {
    for (size_t i = 0; i < settings.monitors.size(); ++i) {
        if (!usedSlots[i] && !settings.monitors[i].devicePath.empty() &&
            PathsEqual(settings.monitors[i].devicePath.c_str(), devicePath)) {
            return i;
        }
    }

    if (monitorIndex < settings.monitors.size() &&
        !usedSlots[monitorIndex] &&
        settings.monitors[monitorIndex].devicePath.empty()) {
        return monitorIndex;
    }
    return settings.monitors.size();
}

void ApplyWallpapers(WorkerContext& context) {
    try {
        Settings settings = GetSettings();
        if (!settings.enabled || !EnsureDesktopWallpaper(context)) {
            return;
        }

        UINT monitorCount = 0;
        HRESULT hr =
            context.desktopWallpaper->GetMonitorDevicePathCount(&monitorCount);
        if (FAILED(hr)) {
            if (context.lastComError != hr) {
                Wh_Log(L"GetMonitorDevicePathCount falhou (HRESULT=0x%08X)",
                       static_cast<unsigned int>(hr));
                context.lastComError = hr;
            }
            context.desktopWallpaper->Release();
            context.desktopWallpaper = nullptr;
            return;
        }
        context.lastComError = S_OK;

        bool logDiagnostics = settings.diagnosticLogging &&
                              context.diagnosticsDirty;
        std::array<bool, 3> usedSlots = {};

        for (UINT index = 0; index < monitorCount; ++index) {
            LPWSTR id = nullptr;
            hr = context.desktopWallpaper->GetMonitorDevicePathAt(index, &id);
            if (FAILED(hr)) {
                Wh_Log(L"GetMonitorDevicePathAt(%u) falhou (HRESULT=0x%08X)",
                       index, static_cast<unsigned int>(hr));
                continue;
            }
            std::wstring devicePath = id ? id : L"";
            CoTaskMemFree(id);

            RECT rect = {};
            HRESULT rectHr = context.desktopWallpaper->GetMonitorRECT(
                devicePath.c_str(), &rect);
            bool attached = rectHr == S_OK;

            LPWSTR currentValue = nullptr;
            HRESULT currentHr = context.desktopWallpaper->GetWallpaper(
                devicePath.c_str(), &currentValue);
            std::wstring currentPath =
                SUCCEEDED(currentHr) && currentValue ? currentValue : L"";
            CoTaskMemFree(currentValue);

            size_t slot = FindSlotForMonitor(settings, index,
                                             devicePath.c_str(), usedSlots);
            if (logDiagnostics) {
                Wh_Log(
                    L"Monitor index=%u, ativo=%d, rect=(%ld,%ld)-(%ld,%ld), "
                    L"devicePath=\"%s\", wallpaper=\"%s\", entrada=%zu",
                    index, attached, rect.left, rect.top, rect.right,
                    rect.bottom, devicePath.c_str(), currentPath.c_str(),
                    slot < settings.monitors.size() ? slot + 1 : 0);
            }

            if (!attached || slot == settings.monitors.size()) {
                continue;
            }
            usedSlots[slot] = true;

            const std::wstring& imagePath =
                settings.monitors[slot].imagePath;
            if (imagePath.empty()) {
                context.lastSlotIssues[slot].clear();
                continue;
            }

            if (!IsLocalAbsolutePath(imagePath)) {
                std::wstring message =
                    L"Entrada " + std::to_wstring(slot + 1) +
                    L": informe um caminho local absoluto (por exemplo, "
                    L"C:\\Imagens\\fundo.jpg)";
                ReportSlotIssue(context, slot, L"path:" + imagePath,
                                message.c_str(), S_OK);
                continue;
            }

            DWORD attributes = GetFileAttributesW(imagePath.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                DWORD error = GetLastError();
                std::wstring message = L"Entrada " +
                                       std::to_wstring(slot + 1) +
                                       L": imagem inexistente ou inválida: " +
                                       imagePath;
                ReportSlotIssue(context, slot,
                                L"file:" + imagePath + L":" +
                                    std::to_wstring(error),
                                message.c_str(), S_OK);
                continue;
            }

            if (FAILED(currentHr)) {
                std::wstring message =
                    L"GetWallpaper falhou na entrada " +
                    std::to_wstring(slot + 1) + L": " + devicePath;
                ReportSlotIssue(context, slot,
                                L"current:" + devicePath + L":" +
                                    std::to_wstring(
                                        static_cast<unsigned>(currentHr)),
                                message.c_str(), currentHr);
                continue;
            }
            context.lastSlotIssues[slot].clear();

            if (PathsEqual(currentPath.c_str(), imagePath.c_str())) {
                continue;
            }

            hr = context.desktopWallpaper->SetWallpaper(devicePath.c_str(),
                                                        imagePath.c_str());
            if (FAILED(hr)) {
                std::wstring message =
                    L"SetWallpaper falhou na entrada " +
                    std::to_wstring(slot + 1) + L": " + imagePath;
                ReportSlotIssue(context, slot,
                                L"set:" + imagePath + L":" +
                                    std::to_wstring(static_cast<unsigned>(hr)),
                                message.c_str(), hr);
                continue;
            }

            context.lastSlotIssues[slot].clear();
            Wh_Log(L"Entrada %zu aplicada ao monitor %u: %s", slot + 1,
                   index, imagePath.c_str());
        }

        if (logDiagnostics) {
            context.diagnosticsDirty = false;
        }
    } catch (...) {
        Wh_Log(L"Exceção capturada durante a aplicação dos papéis de parede");
    }
}

LRESULT CALLBACK WorkerWindowProc(HWND window, UINT message, WPARAM wParam,
                                  LPARAM lParam) {
    WorkerContext* context = reinterpret_cast<WorkerContext*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!context) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
        case WM_TIMER:
            if (wParam == kDebounceTimer) {
                KillTimer(window, kDebounceTimer);
                ApplyWallpapers(*context);
                return 0;
            }
            break;

        case WM_DISPLAYCHANGE:
            ScheduleApply(*context, true);
            return 0;

        case WM_POWERBROADCAST:
            if (wParam == PBT_APMRESUMEAUTOMATIC ||
                wParam == PBT_APMRESUMESUSPEND) {
                ScheduleApply(*context, true);
            }
            return TRUE;

        case WM_WTSSESSION_CHANGE:
            if (wParam == WTS_SESSION_LOGON ||
                wParam == WTS_SESSION_UNLOCK ||
                wParam == WTS_CONSOLE_CONNECT ||
                wParam == WTS_REMOTE_CONNECT) {
                ScheduleApply(*context, true);
                return 0;
            }
            break;

        case WM_NCDESTROY:
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool IsCurrentShellProcess(bool* isShell) {
    *isShell = false;
    HWND shellWindow = GetShellWindow();
    if (!shellWindow) {
        return false;
    }

    DWORD shellProcessId = 0;
    GetWindowThreadProcessId(shellWindow, &shellProcessId);
    if (!shellProcessId) {
        return false;
    }

    *isShell = shellProcessId == GetCurrentProcessId();
    return true;
}

DWORD WINAPI WorkerThread(LPVOID) {
    HRESULT comHr = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(comHr)) {
        Wh_Log(L"CoInitializeEx falhou (HRESULT=0x%08X)",
               static_cast<unsigned int>(comHr));
        return 1;
    }

    WorkerContext context;
    try {
        // Explorers secundários continuam reavaliando o shell, pois a janela
        // antiga pode coexistir com o novo processo durante uma reinicialização.
        while (!g_stop.load()) {
            bool isShell = false;
            if (IsCurrentShellProcess(&isShell) && isShell) {
                break;
            }
            WaitForSingleObject(g_wakeEvent, 1000);
        }

        if (!g_stop.load()) {
            EnsureWorkerWindow(context);
            ApplyWallpapers(context);
            ULONGLONG lastPoll = GetTickCount64();

            while (!g_stop.load()) {
                Settings settings = GetSettings();
                ULONGLONG now = GetTickCount64();
                ULONGLONG elapsed = now - lastPoll;
                DWORD timeout =
                    elapsed >= settings.pollIntervalMs
                        ? 0
                        : settings.pollIntervalMs -
                              static_cast<DWORD>(elapsed);
                DWORD wait = MsgWaitForMultipleObjects(
                    1, &g_wakeEvent, FALSE, timeout, QS_ALLINPUT);
                if (g_stop.load()) {
                    break;
                }

                if (wait == WAIT_OBJECT_0) {
                    std::fill(context.lastSlotIssues.begin(),
                              context.lastSlotIssues.end(), L"");
                    ScheduleApply(context, true);
                } else if (wait == WAIT_OBJECT_0 + 1) {
                    MSG message;
                    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                } else if (wait == WAIT_FAILED) {
                    Wh_Log(L"MsgWaitForMultipleObjects falhou (erro=%lu)",
                           GetLastError());
                    break;
                }

                now = GetTickCount64();
                if (now - lastPoll >= settings.pollIntervalMs) {
                    EnsureWorkerWindow(context);
                    ApplyWallpapers(context);
                    lastPoll = GetTickCount64();
                }
            }
        }
    } catch (...) {
        Wh_Log(L"Exceção capturada na thread de papéis de parede");
    }

    if (context.sessionNotificationsRegistered) {
        WTSUnRegisterSessionNotification(context.window);
    }
    if (context.window) {
        DestroyWindow(context.window);
    }
    if (context.windowClassAtom) {
        UnregisterClassW(kWindowClassName, context.instance);
    }
    if (context.desktopWallpaper) {
        context.desktopWallpaper->Release();
    }
    CoUninitialize();
    return 0;
}

}  // namespace

BOOL Wh_ModInit() {
    try {
        LoadSettings();
        g_stop = false;
        g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_wakeEvent) {
            return FALSE;
        }

        g_workerThread = CreateThread(nullptr, 0, WorkerThread, nullptr, 0,
                                      nullptr);
        if (!g_workerThread) {
            CloseHandle(g_wakeEvent);
            g_wakeEvent = nullptr;
            return FALSE;
        }
        return TRUE;
    } catch (...) {
        Wh_Log(L"Exceção capturada na inicialização");
        return FALSE;
    }
}

void Wh_ModSettingsChanged() {
    try {
        LoadSettings();
        if (g_wakeEvent) {
            SetEvent(g_wakeEvent);
        }
    } catch (...) {
        Wh_Log(L"Exceção capturada ao carregar as configurações");
    }
}

void Wh_ModUninit() {
    g_stop = true;
    if (g_wakeEvent) {
        SetEvent(g_wakeEvent);
    }
    if (g_workerThread) {
        WaitForSingleObject(g_workerThread, INFINITE);
        CloseHandle(g_workerThread);
        g_workerThread = nullptr;
    }
    if (g_wakeEvent) {
        CloseHandle(g_wakeEvent);
        g_wakeEvent = nullptr;
    }
}
