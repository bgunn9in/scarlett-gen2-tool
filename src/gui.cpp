#define SCARLETT_GUI
#include "updater.cpp"

#include <dbt.h>
#include <shellapi.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <thread>

namespace {
constexpr UINT result_message = WM_APP + 1, stage_message = WM_APP + 2;
constexpr UINT_PTR refresh_timer = 1;
enum Button { refresh = 101, enable, disable, builds, install };
enum class Operation { read, power, firmware };

struct Snapshot {
    std::optional<unsigned> firmware;
    std::optional<bool> power;
    std::wstring error;
};

struct Job {
    Operation operation = Operation::read;
    unsigned current = 0, target = 0;
    bool power = false;
    bool automatic = false;
};

struct Result {
    Snapshot snapshot;
    std::wstring status;
};

std::wstring display_error(const Error& error) {
    const std::string_view step(error.step);
    if (step == "unknown-PhantomPower-setting") return L"неизвестное значение PhantomPower в App_Env";
    if (step == "App_Env-contains-additional-data") return L"в App_Env есть дополнительные данные";
    if (step == "unexpected-current-App_Upgrade") return L"содержимое App_Upgrade отличается от исходного образа";
    if (step == "power-setting-changed" || step == "firmware-changed") return L"состояние карты изменилось";
    if (step == "image-size" || step == "image-SHA256") return L"файл прошивки отличается от оригинала";
    return std::wstring(step.begin(), step.end()) + L" (" + std::to_wstring(error.code) + L")";
}

std::wstring firmware_text(unsigned version) {
    wchar_t label[48]{};
    swprintf_s(label, L"%u (0x%04X)", version, version);
    return label;
}

Snapshot read_device() {
    Snapshot snapshot;
    try {
        Pal device(Access::shared_read);
        device.stable();
        snapshot.firmware = device.version();
        snapshot.power = read_power(device);
    } catch (const Error& error) {
        snapshot.error = display_error(error);
    }
    return snapshot;
}

Bytes local_image(unsigned version) {
    require(supported_build(version), "unsupported-image");
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    require(length && length < executable.size(), "module-path");
    executable.resize(length);
    const auto name = L"8202-" + std::to_wstring(version) + L".bin";
    const auto path = std::filesystem::path(executable).parent_path() / name;
    return load_image(path.c_str(), version);
}

std::wstring backup_path() {
    wchar_t local[MAX_PATH]{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    require(length && length < MAX_PATH, "LOCALAPPDATA");
    const auto root = std::filesystem::path(local) / L"FocusriteScarlettGen2" / L"Backups";
    std::error_code error;
    (void)std::filesystem::create_directories(root, error);
    if (error) fail("create-backup-root", static_cast<DWORD>(error.value()));
    SYSTEMTIME date{};
    GetLocalTime(&date);
    wchar_t name[80]{};
    swprintf_s(name, L"%04u%02u%02u-%02u%02u%02u-%lu-%llu", unsigned(date.wYear), unsigned(date.wMonth),
               unsigned(date.wDay), unsigned(date.wHour), unsigned(date.wMinute), unsigned(date.wSecond),
               GetCurrentProcessId(), GetTickCount64());
    return (root / name).wstring();
}

const wchar_t* phase_text(std::string_view phase) {
    if (phase == "backup-saved") return L"Резервная копия сохранена";
    if (phase == "erase-requested") return L"Стирание выбранного сегмента";
    if (phase == "programming") return L"Запись";
    if (phase == "verifying-readback") return L"Сверка записанных данных";
    if (phase == "restart-requested") return L"Перезапуск Scarlett";
    if (phase == "awaiting-reconnect") return L"Ожидание повторного подключения Scarlett";
    return L"Проверка карты перед записью";
}

Result run_job(HWND window, Job job) {
    if (job.operation == Operation::read) {
        auto snapshot = read_device();
        auto status = snapshot.firmware ? std::wstring(L"Чтение завершено.") : L"Ошибка обнаружения: " + snapshot.error;
        return {std::move(snapshot), std::move(status)};
    }
    const bool firmware = job.operation == Operation::firmware;
    const bool target_power = !job.power;
    const wchar_t* phase = phase_text("preflight");
    std::wstring directory;
    Backup backup;
    const auto stage = [&](const char* name) {
        backup.stage(name);
        phase = phase_text(name);
        PostMessageW(window, stage_message, 0, reinterpret_cast<LPARAM>(phase));
    };
    auto with_path = [&](std::wstring status) {
        if (!directory.empty()) status += L"\r\nПапка операции: " + directory;
        return status;
    };
    try {
        const auto image = firmware ? local_image(job.target) : Bytes{};
        {
            Pal device(Access::flash);
            require(device.version() == job.current && supported_build(job.current), "firmware-changed");
            View payload = bytes_of(target_power ? power_on : power_off);
            Bytes original;
            std::wstring filename = L"App_Env.bin";
            if (firmware) {
                payload = image;
                original = prepare_firmware(device, job.current);
                filename = L"App_Upgrade-" + std::to_wstring(job.current) + L".bin";
            } else {
                original = prepare_power(device, job.power);
            }
            directory = backup_path();
            backup.save(directory, filename.c_str(), original);
            stage("backup-saved");
            program_segment(device, firmware ? upgrade_index : env_index, payload, stage, [] { Sleep(100); });
        }
        stage("awaiting-reconnect");
        Snapshot current;
        for (unsigned attempt = 0; attempt < 30; ++attempt) {
            Sleep(1000);
            current = read_device();
            const bool verified = firmware ? current.firmware == job.target :
                                  current.firmware == job.current && current.power == target_power;
            if (verified) {
                backup.stage("verified-after-reconnect");
                return {std::move(current), with_path(firmware ? L"Прошивка подтверждена после перезапуска."
                                                                : L"Режим подтверждён после перезапуска.")};
            }
        }
        return {std::move(current), with_path(L"Запись сверена. Для подтверждения состояния переподключите Scarlett.")};
    } catch (const Error& error) {
        if (std::string_view(error.step) == "image-open") {
            return {read_device(), L"Не удалось открыть 8202-" + std::to_wstring(job.target) +
                L".bin рядом с EXE. См. подготовку прошивки в README."};
        }
        return {read_device(), with_path(std::wstring(phase) + L" — ошибка: " + display_error(error))};
    } catch (...) {
        return {{}, with_path(std::wstring(phase) + L" — непредвиденная ошибка.")};
    }
}

struct Window {
    HWND device = nullptr, firmware = nullptr, power = nullptr, status = nullptr;
    HWND refresh_button = nullptr, on = nullptr, off = nullptr, combo = nullptr, write = nullptr;
    Snapshot snapshot;
    std::thread worker;
    std::wstring check;
    bool refresh_pending = false;
    bool automatic_read = false;
    bool report_error = false;
};

unsigned chosen_build(const Window& ui) {
    const auto row = SendMessageW(ui.combo, CB_GETCURSEL, 0, 0);
    if (row == CB_ERR) return 0;
    return static_cast<unsigned>(SendMessageW(ui.combo, CB_GETITEMDATA, static_cast<WPARAM>(row), 0));
}

void update_controls(const Window& ui) {
    const bool idle = !ui.worker.joinable();
    const bool supported = ui.snapshot.firmware && supported_build(*ui.snapshot.firmware);
    const auto target = chosen_build(ui);
    EnableWindow(ui.refresh_button, idle);
    EnableWindow(ui.on, idle && supported && ui.snapshot.power == false);
    EnableWindow(ui.off, idle && supported && ui.snapshot.power == true);
    EnableWindow(ui.combo, idle && supported);
    EnableWindow(ui.write, idle && supported && supported_build(target) && ui.snapshot.firmware != target);
}

void show_snapshot(const Window& ui) {
    SetWindowTextW(ui.device, ui.snapshot.firmware ? L"Scarlett 2i2 Gen 2  |  USB 1235:8202" : L"Scarlett 2i2 Gen 2 — карта не обнаружена");
    const auto firmware = ui.snapshot.firmware ? firmware_text(*ui.snapshot.firmware) : L"—";
    SetWindowTextW(ui.firmware, (L"Прошивка: " + firmware).c_str());
    const auto power = ui.snapshot.power ? (*ui.snapshot.power ? L"включён" : L"выключен") :
                        ui.snapshot.firmware ? L"ошибка чтения — " + ui.snapshot.error : L"—";
    SetWindowTextW(ui.power, (L"Low Power Mode: " + power).c_str());
}

void start_job(HWND hwnd, Window& ui, Job job) {
    if (ui.worker.joinable()) return;
    auto result = std::make_unique<Result>();
    ui.automatic_read = job.automatic;
    if (!job.automatic)
        SetWindowTextW(ui.status, job.operation == Operation::read ? L"Чтение Scarlett…" : phase_text("preflight"));
    ui.worker = std::thread([hwnd, job, result = std::move(result)]() mutable {
        try {
            *result = run_job(hwnd, job);
        } catch (const Error& error) {
            result->status = display_error(error);
        } catch (...) {
            result->status = L"Непредвиденная ошибка при чтении карты.";
        }
        auto* value = result.release();
        if (!PostMessageW(hwnd, result_message, 0, reinterpret_cast<LPARAM>(value))) delete value;
    });
    update_controls(ui);
}

std::wstring control_text(HWND control) {
    std::wstring text(GetWindowTextLengthW(control) + 1, L'\0');
    text.resize(GetWindowTextW(control, text.data(), static_cast<int>(text.size())));
    return text;
}

void check_window(Window& ui) {
    const auto device = control_text(ui.device);
    const auto firmware = control_text(ui.firmware);
    const auto power = control_text(ui.power);
    const auto status = control_text(ui.status);
    const bool rendering_ok = ui.snapshot.firmware && ui.snapshot.power &&
        device.find(L"1235:8202") != std::wstring::npos &&
        firmware == L"Прошивка: " + firmware_text(*ui.snapshot.firmware) &&
        power == (*ui.snapshot.power ? L"Low Power Mode: включён" : L"Low Power Mode: выключен");
    ui.report_error = !rendering_ok;
    std::wstring files;
    try {
        (void)local_image(old_build);
        (void)local_image(new_build);
        files = L"SHA-256 обоих файлов рядом с EXE подтверждён.";
    } catch (const Error& error) {
        ui.report_error = true;
        files = L"Ошибка проверки файлов прошивки: " + display_error(error);
    }
    std::wstring preflight;
    if (ui.snapshot.firmware && supported_build(*ui.snapshot.firmware) && ui.snapshot.power) {
        try {
            Pal connection(Access::shared_read);
            const auto version = *ui.snapshot.firmware;
            (void)prepare_firmware(connection, version);
            (void)prepare_power(connection, *ui.snapshot.power);
            preflight = L"Каждый сегмент считан дважды. App_Upgrade соответствует образу. "
                        L"App_Env содержит только PhantomPower.";
        } catch (const Error& error) {
            ui.report_error = true;
            preflight = L"Ошибка предпроверки сегментов: " + display_error(error);
        }
    }
    const auto report = L"\ufeff" + device + L"\r\n" + firmware + L"\r\n" + power + L"\r\n" + status +
        L"\r\nОшибка чтения: " + ui.snapshot.error + L"\r\nРежим проверки: чтение.\r\n" + files + L"\r\n" + preflight + L"\r\n";
    Handle file(CreateFileW(ui.check.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) fail("create-GUI-read-check", GetLastError());
    write_file(file.value, {reinterpret_cast<const BYTE*>(report.data()), report.size() * sizeof(wchar_t)});
}

HWND control(HWND parent, const wchar_t* kind, const wchar_t* title, DWORD style,
             int x, int y, int width, int height, int id = 0) {
    auto handle = CreateWindowExW(0, kind, title, WS_CHILD | WS_VISIBLE | style, x, y, width, height,
                                  parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  GetModuleHandleW(nullptr), nullptr);
    require(handle != nullptr, "create-GUI-control");
    SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
    return handle;
}

void create_controls(HWND hwnd, Window& ui) {
    ui.device = control(hwnd, L"STATIC", L"Scarlett 2i2 Gen 2", 0, 16, 14, 530, 22);
    ui.firmware = control(hwnd, L"STATIC", L"Прошивка: —", 0, 16, 42, 530, 20);
    ui.power = control(hwnd, L"STATIC", L"Low Power Mode: —", 0, 16, 69, 530, 34);
    ui.on = control(hwnd, L"BUTTON", L"Включить", WS_TABSTOP, 16, 108, 104, 29, enable);
    ui.off = control(hwnd, L"BUTTON", L"Выключить", WS_TABSTOP, 128, 108, 104, 29, disable);
    ui.refresh_button = control(hwnd, L"BUTTON", L"Обновить статус", WS_TABSTOP, 397, 108, 149, 29, refresh);
    control(hwnd, L"STATIC", L"Целевая прошивка", 0, 16, 165, 130, 22);
    ui.combo = control(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 151, 160, 106, 100, builds);
    for (const auto build : {old_build, new_build}) {
        const auto row = SendMessageW(ui.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(std::to_wstring(build).c_str()));
        require(row >= 0 && SendMessageW(ui.combo, CB_SETITEMDATA, static_cast<WPARAM>(row), build) != CB_ERR,
                "init-GUI-firmware-list");
    }
    ui.write = control(hwnd, L"BUTTON", L"Прошить…", WS_TABSTOP, 271, 158, 117, 30, install);
    ui.status = control(hwnd, L"EDIT", L"", WS_TABSTOP | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                        16, 209, 530, 84);
    start_job(hwnd, ui, {});
}

LRESULT dispatch(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, Window& ui) {
    switch (message) {
    case WM_CREATE:
        create_controls(hwnd, ui);
        return 0;
    case WM_DEVICECHANGE:
        if (ui.check.empty() && (wparam == DBT_DEVNODES_CHANGED || wparam == DBT_DEVICEARRIVAL ||
                                wparam == DBT_DEVICEREMOVECOMPLETE)) {
            ui.refresh_pending = true;
            if (!ui.worker.joinable()) SetTimer(hwnd, refresh_timer, 750, nullptr);
        }
        return TRUE;
    case WM_TIMER:
        if (wparam == refresh_timer) {
            KillTimer(hwnd, refresh_timer);
            if (!ui.worker.joinable() && ui.refresh_pending) {
                ui.refresh_pending = false;
                Job job;
                job.automatic = true;
                start_job(hwnd, ui, job);
            }
            return 0;
        }
        break;
    case WM_COMMAND: {
        if (!ui.check.empty() || ui.worker.joinable()) return 0;
        const auto id = LOWORD(wparam);
        if (id == builds) {
            update_controls(ui);
        } else if (id == refresh) {
            start_job(hwnd, ui, {});
        } else if (ui.snapshot.firmware && supported_build(*ui.snapshot.firmware)) {
            const auto current = *ui.snapshot.firmware;
            if ((id == enable || id == disable) && ui.snapshot.power && *ui.snapshot.power != (id == enable)) {
                start_job(hwnd, ui, {Operation::power, current, 0, *ui.snapshot.power});
            } else if (id == install) {
                const auto target = chosen_build(ui);
                if (!supported_build(target) || target == current) return 0;
                const std::wstring prompt = (target < current ? L"Понизить прошивку " : L"Обновить прошивку ") +
                    std::to_wstring(current) + L" → " + std::to_wstring(target) +
                    L"?\n\nScarlett перезапустится. Оставьте USB подключённым до завершения.";
                if (MessageBoxW(hwnd, prompt.c_str(), L"Запись прошивки", MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) == IDYES)
                    start_job(hwnd, ui, {Operation::firmware, current, target});
            }
        }
        return 0;
    }
    case stage_message:
        SetWindowTextW(ui.status, reinterpret_cast<const wchar_t*>(lparam));
        return 0;
    case result_message: {
        const std::unique_ptr<Result> result(reinterpret_cast<Result*>(lparam));
        ui.worker.join();
        ui.snapshot = std::move(result->snapshot);
        show_snapshot(ui);
        if (!ui.automatic_read) SetWindowTextW(ui.status, result->status.c_str());
        update_controls(ui);
        if (!ui.check.empty()) {
            try { check_window(ui); } catch (...) { ui.report_error = true; }
            DestroyWindow(hwnd);
        } else if (ui.refresh_pending) {
            SetTimer(hwnd, refresh_timer, 750, nullptr);
        }
        return 0;
    }
    case WM_CLOSE:
        if (!ui.worker.joinable()) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(ui.report_error ? 3 : 0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto creation = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
    }
    const auto ui = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!ui) return DefWindowProcW(hwnd, message, wparam, lparam);
    try {
        return dispatch(hwnd, message, wparam, lparam, *ui);
    } catch (...) {
        if (message == WM_CREATE) return -1;
        if (ui->status) SetWindowTextW(ui->status, L"Ошибка Windows при обработке команды.");
        return 0;
    }
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    Window ui;
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 2;
    if (argc == 3 && std::wstring_view(argv[1]) == L"--check-gui") ui.check = argv[2];
    LocalFree(argv);
    if (argc != 1 && ui.check.empty()) return 2;
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = L"ScarlettGen2Utility";
    if (!RegisterClassW(&wc)) return 2;
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT area{0, 0, 562, 312};
    AdjustWindowRect(&area, style, FALSE);
    const auto hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"Scarlett 2i2 Gen 2",
                                      style, CW_USEDEFAULT, CW_USEDEFAULT, area.right - area.left,
                                      area.bottom - area.top, nullptr, nullptr, instance, &ui);
    if (!hwnd) {
        if (ui.check.empty()) MessageBoxW(nullptr, L"Не удалось создать окно.", L"Scarlett 2i2 Gen 2", MB_ICONERROR);
        return 2;
    }
    if (ui.check.empty()) ShowWindow(hwnd, show);
    MSG message{};
    int read = 0;
    while ((read = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (IsDialogMessageW(hwnd, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (ui.worker.joinable()) ui.worker.join();
    return read < 0 ? 2 : static_cast<int>(message.wParam);
}
