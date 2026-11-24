#define SCARLETT_GUI
#include "updater.cpp"

#include <commctrl.h>
#include <dbt.h>
#include <shellapi.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <thread>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
constexpr UINT result_message = WM_APP + 1, stage_message = WM_APP + 2;
constexpr UINT_PTR refresh_timer = 1;
constexpr int client_width = 616, client_height = 480, header_height = 110;
enum ControlId { refresh = 101, enable, disable, builds, install, language_choice };
enum class Language { english, russian };
enum class Operation { read, power, firmware };

const wchar_t* tr(Language language, const wchar_t* english, const wchar_t* russian) {
    return language == Language::russian ? russian : english;
}

constexpr wchar_t preferences[] = L"Software\\ScarlettGen2Tool";

Language load_language() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    const auto result = RegGetValueW(HKEY_CURRENT_USER, preferences, L"Language", RRF_RT_REG_DWORD,
                                     nullptr, &value, &size);
    return result == ERROR_SUCCESS && value == 1 ? Language::russian : Language::english;
}

struct Snapshot {
    std::optional<unsigned> firmware;
    std::optional<bool> power;
    std::optional<Error> error;
};

struct Job {
    Operation operation = Operation::read;
    unsigned current = 0, target = 0;
    bool power = false;
    bool automatic = false;
};

enum class StatusKind { reading, read_done, detection_error, power_error, phase, firmware_verified,
                        power_verified, reconnect, missing_image, failed, unexpected, read_failed };

struct Status {
    StatusKind kind = StatusKind::reading;
    const char* phase = "preflight";
    std::optional<Error> error;
    unsigned target = 0;
    std::wstring directory;
};

struct Result {
    Snapshot snapshot;
    Status status;
};

std::wstring display_error(Language language, const Error& error) {
    const std::string_view step(error.step);
    if (step == "unknown-PhantomPower-setting")
        return tr(language, L"unknown PhantomPower value in App_Env", L"неизвестное значение PhantomPower в App_Env");
    if (step == "App_Env-contains-additional-data")
        return tr(language, L"App_Env contains additional data", L"в App_Env есть дополнительные данные");
    if (step == "unexpected-current-App_Upgrade")
        return tr(language, L"App_Upgrade differs from the original image", L"содержимое App_Upgrade отличается от исходного образа");
    if (step == "power-setting-changed" || step == "firmware-changed")
        return tr(language, L"the interface state changed", L"состояние карты изменилось");
    if (step == "image-size" || step == "image-SHA256")
        return tr(language, L"the firmware file differs from the original", L"файл прошивки отличается от оригинала");
    return std::wstring(step.begin(), step.end()) + L" (" + std::to_wstring(error.code) + L")";
}

std::wstring revision_text(unsigned version) {
    wchar_t label[48]{};
    swprintf_s(label, L"0x%04X", version);
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
        snapshot.error = error;
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

const wchar_t* phase_text(Language language, std::string_view phase) {
    if (phase == "backup-saved") return tr(language, L"Backup saved", L"Резервная копия сохранена");
    if (phase == "erase-requested") return tr(language, L"Erasing the selected segment", L"Стирание выбранного сегмента");
    if (phase == "programming") return tr(language, L"Writing", L"Запись");
    if (phase == "verifying-readback") return tr(language, L"Verifying the written data", L"Сверка записанных данных");
    if (phase == "restart-requested") return tr(language, L"Restarting Scarlett", L"Перезапуск Scarlett");
    if (phase == "awaiting-reconnect") return tr(language, L"Waiting for Scarlett to reconnect", L"Ожидание повторного подключения Scarlett");
    return tr(language, L"Checking the interface before writing", L"Проверка карты перед записью");
}

std::wstring status_text(Language language, const Status& status) {
    using enum StatusKind;
    const auto detail = [&] {
        return status.error ? display_error(language, *status.error) : std::wstring{};
    };
    std::wstring text;
    switch (status.kind) {
    case reading: text = tr(language, L"Reading Scarlett…", L"Чтение Scarlett…"); break;
    case read_done: text = tr(language, L"Device status updated.", L"Данные устройства обновлены."); break;
    case detection_error: text = tr(language, L"Could not detect Scarlett: ", L"Ошибка обнаружения: ") + detail(); break;
    case power_error: text = tr(language, L"Could not read Low Power Mode: ", L"Ошибка чтения Low Power Mode: ") + detail(); break;
    case phase: text = phase_text(language, status.phase); break;
    case firmware_verified:
        text = tr(language, L"Firmware verified after restart.", L"Прошивка подтверждена после перезапуска."); break;
    case power_verified:
        text = tr(language, L"Low Power Mode verified after restart.", L"Режим подтверждён после перезапуска."); break;
    case reconnect:
        text = tr(language, L"Write verified. Reconnect Scarlett to confirm its status.",
                            L"Запись сверена. Для подтверждения состояния переподключите Scarlett."); break;
    case missing_image:
        text = tr(language, L"Could not open 8202-", L"Не удалось открыть 8202-") + std::to_wstring(status.target) +
            tr(language, L".bin beside the EXE. See the firmware section in the README.",
                         L".bin рядом с EXE. См. раздел о прошивке в README."); break;
    case failed:
        text = std::wstring(phase_text(language, status.phase)) + tr(language, L" failed: ", L" — ошибка: ") + detail(); break;
    case unexpected:
        text = std::wstring(phase_text(language, status.phase)) +
               tr(language, L" failed unexpectedly.", L" — непредвиденная ошибка."); break;
    case read_failed:
        text = status.error ? detail() : tr(language, L"Unexpected error while reading Scarlett.",
                                                     L"Непредвиденная ошибка при чтении карты."); break;
    }
    if (!status.directory.empty())
        text += tr(language, L"\r\nOperation folder: ", L"\r\nПапка операции: ") + status.directory;
    return text;
}

Result run_job(HWND window, Job job) {
    if (job.operation == Operation::read) {
        auto snapshot = read_device();
        Status status;
        status.kind = !snapshot.firmware ? StatusKind::detection_error :
                      !snapshot.power ? StatusKind::power_error : StatusKind::read_done;
        status.error = snapshot.error;
        return {std::move(snapshot), std::move(status)};
    }
    const bool firmware = job.operation == Operation::firmware;
    const bool target_power = !job.power;
    Status status;
    Backup backup;
    const auto stage = [&](const char* name) {
        backup.stage(name);
        status.phase = name;
        PostMessageW(window, stage_message, 0, reinterpret_cast<LPARAM>(name));
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
            status.directory = backup_path();
            backup.save(status.directory, filename.c_str(), original);
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
                status.kind = firmware ? StatusKind::firmware_verified : StatusKind::power_verified;
                return {std::move(current), std::move(status)};
            }
        }
        status.kind = StatusKind::reconnect;
        return {std::move(current), std::move(status)};
    } catch (const Error& error) {
        status.kind = std::string_view(error.step) == "image-open" ? StatusKind::missing_image : StatusKind::failed;
        status.error = error;
        status.target = job.target;
        return {read_device(), std::move(status)};
    } catch (...) {
        status.kind = StatusKind::unexpected;
        return {{}, std::move(status)};
    }
}

struct Window {
    HWND title = nullptr, device = nullptr, firmware = nullptr, revision = nullptr, power = nullptr, status = nullptr;
    HWND refresh_button = nullptr, on = nullptr, off = nullptr, combo = nullptr, write = nullptr;
    HWND language_label = nullptr, language_picker = nullptr, firmware_label = nullptr, write_label = nullptr, status_label = nullptr;
    HFONT regular = nullptr, section = nullptr, value = nullptr, heading = nullptr;
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    bool high_contrast = false;
    Language language = Language::english;
    Snapshot snapshot;
    Status operation_status;
    std::thread worker;
    std::wstring check;
    bool refresh_pending = false;
    bool automatic_read = false;
    bool report_error = false;

    ~Window() {
        for (const auto font : {regular, section, value, heading}) if (font) DeleteObject(font);
    }
};

int px(const Window& ui, int logical) {
    return MulDiv(logical, ui.dpi, USER_DEFAULT_SCREEN_DPI);
}

COLORREF color(const Window& ui, COLORREF rgb, int system_color) {
    return ui.high_contrast ? GetSysColor(system_color) : rgb;
}

HBRUSH brush(HDC dc, COLORREF fill) {
    SetDCBrushColor(dc, fill);
    return static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
}

void paint_surface(const Window& ui, HDC dc, RECT area) {
    const auto background = color(ui, RGB(242, 245, 249), COLOR_WINDOW);
    const auto paper = color(ui, RGB(255, 255, 255), COLOR_WINDOW);
    const auto border = color(ui, RGB(215, 224, 235), COLOR_WINDOWFRAME);
    FillRect(dc, &area, brush(dc, background));
    const RECT header{0, 0, area.right, px(ui, header_height)};
    FillRect(dc, &header, brush(dc, color(ui, RGB(23, 32, 49), COLOR_WINDOW)));
    const RECT accent{0, 0, area.right, px(ui, 3)};
    FillRect(dc, &accent, brush(dc, color(ui, RGB(205, 49, 82), COLOR_HIGHLIGHT)));
    for (const RECT card : {RECT{24, 132, 298, 253}, RECT{318, 132, 592, 253},
                            RECT{24, 269, 592, 341}, RECT{24, 357, 592, 456}}) {
        const RECT scaled{px(ui, card.left), px(ui, card.top), px(ui, card.right), px(ui, card.bottom)};
        FillRect(dc, &scaled, brush(dc, paper));
        FrameRect(dc, &scaled, brush(dc, border));
    }
    const RECT dot{px(ui, 29), px(ui, 81), px(ui, 37), px(ui, 89)};
    const auto old_brush = SelectObject(dc, brush(dc, ui.snapshot.firmware ?
        color(ui, RGB(75, 216, 162), COLOR_WINDOWTEXT) : color(ui, RGB(155, 168, 190), COLOR_GRAYTEXT)));
    const auto old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, dot.left, dot.top, dot.right, dot.bottom);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
}

void paint_button(const Window& ui, const DRAWITEMSTRUCT& item) {
    const bool inactive = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool primary = item.CtlID == install;
    const auto dc = item.hDC;
    auto rect = item.rcItem;
    const auto fill = primary ? inactive ? RGB(233, 237, 243) : pressed ? RGB(135, 20, 47) : RGB(171, 29, 62) :
                               pressed ? RGB(53, 67, 90) : RGB(34, 46, 66);
    const auto ink = inactive ? primary ? RGB(91, 104, 124) : RGB(164, 176, 197) : RGB(255, 255, 255);
    FillRect(dc, &rect, brush(dc, color(ui, fill, inactive ? COLOR_BTNFACE : COLOR_HIGHLIGHT)));
    const auto edge = primary ? fill : RGB(89, 105, 131);
    FrameRect(dc, &rect, brush(dc, color(ui, edge, COLOR_WINDOWFRAME)));
    if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
        InflateRect(&rect, -px(ui, 3), -px(ui, 3));
        DrawFocusRect(dc, &rect);
    }
    wchar_t label[64]{};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    const auto old_font = SelectObject(dc, ui.section);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color(ui, ink, inactive ? COLOR_GRAYTEXT : COLOR_HIGHLIGHTTEXT));
    DrawTextW(dc, label, -1, &rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    SelectObject(dc, old_font);
}

unsigned chosen_build(const Window& ui) {
    const auto row = SendMessageW(ui.combo, CB_GETCURSEL, 0, 0);
    if (row == CB_ERR) return 0;
    return static_cast<unsigned>(SendMessageW(ui.combo, CB_GETITEMDATA, static_cast<WPARAM>(row), 0));
}

void update_controls(const Window& ui) {
    const bool idle = !ui.worker.joinable();
    const bool supported = ui.snapshot.firmware && supported_build(*ui.snapshot.firmware);
    const auto target = chosen_build(ui);
    EnableWindow(ui.language_picker, idle);
    EnableWindow(ui.refresh_button, idle);
    EnableWindow(ui.on, idle && supported && ui.snapshot.power == false);
    EnableWindow(ui.off, idle && supported && ui.snapshot.power == true);
    EnableWindow(ui.combo, idle && supported);
    EnableWindow(ui.write, idle && supported && supported_build(target) && ui.snapshot.firmware != target);
}

void show_snapshot(const Window& ui) {
    const auto language = ui.language;
    SetWindowTextW(ui.device, ui.snapshot.firmware ? tr(language, L"Connected · USB 1235:8202", L"Подключена · USB 1235:8202") :
        ui.operation_status.kind == StatusKind::reading ? tr(language, L"Looking for Scarlett…", L"Поиск Scarlett…") :
        tr(language, L"Scarlett not detected", L"Scarlett не обнаружена"));
    SetWindowTextW(ui.firmware, ui.snapshot.firmware ? std::to_wstring(*ui.snapshot.firmware).c_str() : L"—");
    SetWindowTextW(ui.revision, ui.snapshot.firmware ? revision_text(*ui.snapshot.firmware).c_str() : L"");
    const auto* power = ui.snapshot.power ? (*ui.snapshot.power ? tr(language, L"Enabled", L"Включён") :
                                                                  tr(language, L"Disabled", L"Выключен")) :
                        ui.snapshot.firmware ? tr(language, L"Read error", L"Ошибка чтения") : L"—";
    SetWindowTextW(ui.power, power);
    const RECT indicator{px(ui, 27), px(ui, 79), px(ui, 39), px(ui, 91)};
    InvalidateRect(GetParent(ui.device), &indicator, FALSE);
}

void show_status(const Window& ui) {
    SetWindowTextW(ui.status, status_text(ui.language, ui.operation_status).c_str());
}

void apply_language(const Window& ui) {
    const auto language = ui.language;
    SetWindowTextW(ui.language_label, tr(language, L"Language", L"Язык"));
    SetWindowTextW(ui.refresh_button, tr(language, L"Refresh status", L"Обновить статус"));
    SetWindowTextW(ui.firmware_label, tr(language, L"Current firmware", L"Текущая прошивка"));
    SetWindowTextW(ui.on, tr(language, L"Enable", L"Включить"));
    SetWindowTextW(ui.off, tr(language, L"Disable", L"Выключить"));
    SetWindowTextW(ui.write_label, tr(language, L"Flash firmware", L"Запись прошивки"));
    SetWindowTextW(ui.write, tr(language, L"Flash…", L"Прошить…"));
    SetWindowTextW(ui.status_label, tr(language, L"Operation status", L"Статус операции"));

    const auto selected = SendMessageW(ui.combo, CB_GETCURSEL, 0, 0);
    SendMessageW(ui.combo, CB_RESETCONTENT, 0, 0);
    for (const auto build : {0u, old_build, new_build}) {
        const auto text = build ? std::to_wstring(build) : tr(language, L"Select a version", L"Выбрать версию");
        const auto row = SendMessageW(ui.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        require(row >= 0 && SendMessageW(ui.combo, CB_SETITEMDATA, static_cast<WPARAM>(row), build) != CB_ERR,
                "init-GUI-firmware-list");
    }
    SendMessageW(ui.combo, CB_SETCURSEL, selected == CB_ERR ? 0 : static_cast<WPARAM>(selected), 0);
    show_snapshot(ui);
    show_status(ui);
}

void start_job(HWND hwnd, Window& ui, Job job) {
    if (ui.worker.joinable()) return;
    auto result = std::make_unique<Result>();
    ui.automatic_read = job.automatic;
    if (!job.automatic) {
        ui.operation_status = {};
        if (job.operation != Operation::read) ui.operation_status.kind = StatusKind::phase;
        show_status(ui);
    }
    ui.worker = std::thread([hwnd, job, result = std::move(result)]() mutable {
        try {
            *result = run_job(hwnd, job);
        } catch (const Error& error) {
            result->status.kind = StatusKind::read_failed;
            result->status.error = error;
        } catch (...) {
            result->status.kind = StatusKind::read_failed;
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

void check_window(HWND hwnd, Window& ui) {
    const auto initial_language = ui.language;
    const auto device = control_text(ui.device);
    const auto firmware = control_text(ui.firmware);
    const auto revision = control_text(ui.revision);
    const auto power = control_text(ui.power);
    const auto status = control_text(ui.status);
    const bool rendering_ok = ui.snapshot.firmware && ui.snapshot.power &&
        device.find(L"1235:8202") != std::wstring::npos &&
        firmware == std::to_wstring(*ui.snapshot.firmware) && revision == revision_text(*ui.snapshot.firmware) &&
        power == (*ui.snapshot.power ? tr(initial_language, L"Enabled", L"Включён") :
                                     tr(initial_language, L"Disabled", L"Выключен"));
    ui.report_error = !rendering_ok;
    SendMessageW(ui.combo, CB_SETCURSEL, 1, 0);
    std::wstring language_checks = L"Startup language: ";
    language_checks += tr(initial_language, L"en\r\n", L"ru\r\n");
    for (const auto language : {Language::english, Language::russian}) {
        SendMessageW(ui.language_picker, CB_SETCURSEL, static_cast<WPARAM>(language), 0);
        SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(language_choice, CBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(ui.language_picker));
        const auto refresh_text = control_text(ui.refresh_button);
        const auto flash_text = control_text(ui.write);
        const auto current_status = control_text(ui.status);
        if (ui.language != language || chosen_build(ui) != old_build ||
            refresh_text != tr(language, L"Refresh status", L"Обновить статус") ||
            flash_text != tr(language, L"Flash…", L"Прошить…") ||
            current_status != tr(language, L"Device status updated.", L"Данные устройства обновлены.")) {
            ui.report_error = true;
        }
        language_checks += (std::wstring(tr(language, L"en: ", L"ru: ")) + refresh_text +
                            L" | " + flash_text + L" | " + current_status + L"\r\n");
    }
    std::wstring files;
    try {
        (void)local_image(old_build);
        (void)local_image(new_build);
        files = L"Both firmware files beside the EXE passed SHA-256 verification.";
    } catch (const Error& error) {
        ui.report_error = true;
        files = L"Firmware file check failed: " + display_error(initial_language, error);
    }
    std::wstring preflight;
    if (ui.snapshot.firmware && supported_build(*ui.snapshot.firmware) && ui.snapshot.power) {
        try {
            Pal connection(Access::shared_read);
            const auto version = *ui.snapshot.firmware;
            (void)prepare_firmware(connection, version);
            (void)prepare_power(connection, *ui.snapshot.power);
            preflight = L"Both segments read twice. App_Upgrade matches the original image. App_Env contains only PhantomPower.";
        } catch (const Error& error) {
            ui.report_error = true;
            preflight = L"Segment check failed: " + display_error(initial_language, error);
        }
    }
    const auto report = L"\ufeff" + control_text(ui.title) + L"\r\n" + device + L"\r\nFirmware: " +
        firmware + L" (" + revision + L")\r\nLow Power Mode: " + power + L"\r\n" + status +
        L"\r\nRead error: " + (ui.snapshot.error ? display_error(initial_language, *ui.snapshot.error) : L"") +
        L"\r\nRead-only check.\r\n" + language_checks + files + L"\r\n" + preflight + L"\r\n";
    Handle file(CreateFileW(ui.check.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) fail("create-GUI-read-check", GetLastError());
    write_file(file.value, {reinterpret_cast<const BYTE*>(report.data()), report.size() * sizeof(wchar_t)});
}

HWND control(HWND parent, const Window& ui, const wchar_t* kind, const wchar_t* title, DWORD style,
             int x, int y, int width, int height, HFONT font, int id = 0) {
    auto handle = CreateWindowExW(0, kind, title, WS_CHILD | WS_VISIBLE | style,
                                  px(ui, x), px(ui, y), px(ui, width), px(ui, height), parent,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    require(handle != nullptr, "create-GUI-control");
    SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    return handle;
}

void create_controls(HWND hwnd, Window& ui) {
    const auto make_font = [&](int size, int weight) {
        auto font = CreateFontW(-px(ui, size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        require(font != nullptr, "create-GUI-font");
        return font;
    };
    ui.regular = make_font(15, FW_NORMAL);
    ui.section = make_font(16, FW_SEMIBOLD);
    ui.value = make_font(26, FW_SEMIBOLD);
    ui.heading = make_font(34, FW_SEMIBOLD);
    const auto label = [&](const wchar_t* text, int x, int y, int width, int height, HFONT font) {
        return control(hwnd, ui, L"STATIC", text, SS_NOPREFIX, x, y, width, height, font);
    };
    const auto button = [&](const wchar_t* text, int x, int y, int width, int height, int id, DWORD style = 0) {
        return control(hwnd, ui, L"BUTTON", text, WS_TABSTOP | style, x, y, width, height, ui.section, id);
    };

    ui.title = label(L"Scarlett 2i2 Gen 2", 26, 25, 405, 46, ui.heading);
    ui.device = label(L"", 48, 74, 350, 22, ui.regular);
    ui.refresh_button = button(L"", 428, 25, 162, 37, refresh, BS_OWNERDRAW);
    ui.language_label = label(L"", 408, 77, 76, 22, ui.regular);
    ui.language_picker = control(hwnd, ui, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP,
                                 485, 73, 105, 80, ui.regular, language_choice);
    SendMessageW(ui.language_picker, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), px(ui, 23));
    for (const auto* name : {L"English", L"Русский"}) {
        require(SendMessageW(ui.language_picker, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name)) >= 0,
                "init-GUI-language-list");
    }
    SendMessageW(ui.language_picker, CB_SETCURSEL, static_cast<WPARAM>(ui.language), 0);

    ui.firmware_label = label(L"", 42, 146, 235, 23, ui.section);
    ui.firmware = label(L"—", 42, 173, 235, 37, ui.value);
    ui.revision = label(L"", 42, 215, 235, 22, ui.regular);
    label(L"Low Power Mode", 336, 146, 235, 23, ui.section);
    ui.power = label(L"—", 336, 173, 235, 37, ui.value);
    ui.on = button(L"", 336, 214, 109, 29, enable);
    ui.off = button(L"", 453, 214, 119, 29, disable);

    ui.write_label = label(L"", 42, 294, 212, 24, ui.section);
    ui.combo = control(hwnd, ui, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 262, 289, 152, 100, ui.regular, builds);
    SendMessageW(ui.combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), px(ui, 24));
    ui.write = button(L"", 430, 286, 142, 38, install, BS_OWNERDRAW);

    ui.status_label = label(L"", 42, 371, 530, 23, ui.section);
    ui.status = control(hwnd, ui, L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                        42, 397, 530, 50, ui.regular);
    apply_language(ui);
    start_job(hwnd, ui, {});
}

bool confirm_flash(HWND hwnd, Language language, unsigned current, unsigned target) {
    const std::wstring prompt = (target < current ? tr(language, L"Downgrade firmware from ", L"Понизить прошивку ") :
        tr(language, L"Update firmware from ", L"Обновить прошивку ")) + std::to_wstring(current) +
        tr(language, L" to ", L" → ") + std::to_wstring(target) +
        tr(language, L"?\n\nScarlett will restart. Leave USB connected until the operation finishes.",
                     L"?\n\nScarlett перезапустится. Оставьте USB подключённым до завершения.");
    const TASKDIALOG_BUTTON buttons[]{
        {IDYES, tr(language, L"Flash", L"Прошить")}, {IDCANCEL, tr(language, L"Cancel", L"Отмена")}
    };
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = hwnd;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.pszWindowTitle = tr(language, L"Flash firmware", L"Запись прошивки");
    config.pszMainIcon = TD_WARNING_ICON;
    config.pszContent = prompt.c_str();
    config.cButtons = static_cast<UINT>(std::size(buttons));
    config.pButtons = buttons;
    config.nDefaultButton = IDCANCEL;
    int button = IDCANCEL;
    return SUCCEEDED(TaskDialogIndirect(&config, &button, nullptr, nullptr)) && button == IDYES;
}

LRESULT dispatch(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, Window& ui) {
    switch (message) {
    case WM_CREATE:
        create_controls(hwnd, ui);
        return 0;
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT:
    case WM_PRINTCLIENT: {
        PAINTSTRUCT paint{};
        const auto dc = message == WM_PAINT ? BeginPaint(hwnd, &paint) : reinterpret_cast<HDC>(wparam);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        paint_surface(ui, dc, rect);
        if (message == WM_PAINT) EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        const auto child = reinterpret_cast<HWND>(lparam);
        const auto dc = reinterpret_cast<HDC>(wparam);
        const bool header = child == ui.title || child == ui.device || child == ui.language_label;
        const auto fill = color(ui, header ? RGB(23, 32, 49) : RGB(255, 255, 255), COLOR_WINDOW);
        const auto text = child == ui.title ? RGB(255, 255, 255) : header ? RGB(203, 217, 235) :
                          child == ui.revision || child == ui.status ? RGB(75, 89, 112) : RGB(28, 39, 57);
        SetTextColor(dc, color(ui, text, COLOR_WINDOWTEXT));
        SetBkColor(dc, fill);
        SetBkMode(dc, OPAQUE);
        return reinterpret_cast<LRESULT>(brush(dc, fill));
    }
    case WM_DRAWITEM:
        paint_button(ui, *reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
        return TRUE;
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
        if (ui.worker.joinable()) return 0;
        const auto id = LOWORD(wparam);
        if (id == language_choice && HIWORD(wparam) == CBN_SELCHANGE) {
            const auto selected = SendMessageW(ui.language_picker, CB_GETCURSEL, 0, 0);
            if (selected == 0 || selected == 1) {
                ui.language = static_cast<Language>(selected);
                apply_language(ui);
                const DWORD value = static_cast<DWORD>(ui.language);
                if (ui.check.empty() && RegSetKeyValueW(HKEY_CURRENT_USER, preferences, L"Language",
                    REG_DWORD, &value, sizeof(value)) != ERROR_SUCCESS) {
                    MessageBoxW(hwnd, tr(ui.language, L"Could not save the language preference.",
                                                     L"Не удалось сохранить выбранный язык."),
                                L"Scarlett Gen 2 Tool", MB_ICONWARNING);
                }
            }
            return 0;
        }
        if (!ui.check.empty()) return 0;
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
                if (confirm_flash(hwnd, ui.language, current, target))
                    start_job(hwnd, ui, {Operation::firmware, current, target});
            }
        }
        return 0;
    }
    case stage_message:
        ui.operation_status.phase = reinterpret_cast<const char*>(lparam);
        show_status(ui);
        return 0;
    case result_message: {
        const std::unique_ptr<Result> result(reinterpret_cast<Result*>(lparam));
        ui.worker.join();
        ui.snapshot = std::move(result->snapshot);
        if (!ui.automatic_read) {
            ui.operation_status = std::move(result->status);
            show_status(ui);
        }
        show_snapshot(ui);
        update_controls(ui);
        if (!ui.check.empty()) {
            try { check_window(hwnd, ui); } catch (...) { ui.report_error = true; }
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
        if (ui->status)
            SetWindowTextW(ui->status, tr(ui->language, L"Windows could not process the command.",
                                                       L"Ошибка Windows при обработке команды."));
        return 0;
    }
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    Window ui;
    ui.language = load_language();
    if (const auto dc = GetDC(nullptr)) {
        ui.dpi = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX));
        ReleaseDC(nullptr, dc);
    }
    HIGHCONTRASTW contrast{sizeof(contrast)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0))
        ui.high_contrast = (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
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
    wc.lpszClassName = L"ScarlettGen2Utility";
    if (!RegisterClassW(&wc)) return 2;
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    constexpr DWORD extended = WS_EX_CONTROLPARENT;
    RECT area{0, 0, px(ui, client_width), px(ui, client_height)};
    AdjustWindowRectEx(&area, style, FALSE, extended);
    const auto hwnd = CreateWindowExW(extended, wc.lpszClassName, L"Scarlett Gen 2 Tool",
                                      style, CW_USEDEFAULT, CW_USEDEFAULT, area.right - area.left,
                                      area.bottom - area.top, nullptr, nullptr, instance, &ui);
    if (!hwnd) {
        if (ui.check.empty())
            MessageBoxW(nullptr, tr(ui.language, L"Could not create the window.", L"Не удалось создать окно."),
                        L"Scarlett Gen 2 Tool", MB_ICONERROR);
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
