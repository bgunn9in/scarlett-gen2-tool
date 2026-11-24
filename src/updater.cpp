#include "pal.h"

#include <algorithm>
#include <span>

namespace {
using Bytes = std::vector<BYTE>;
using View = std::span<const BYTE>;
constexpr unsigned old_build = 1051;
constexpr unsigned new_build = 1116;
constexpr DWORD old_image_bytes = 64000;
constexpr DWORD image_bytes = 64256;
constexpr DWORD segment_bytes = 262144;
constexpr DWORD env_bytes = 65536;
constexpr DWORD read_chunk = 1024;
constexpr DWORD program_chunk = 1024 - 12;
constexpr unsigned erase_poll_limit = 600;
constexpr char image_sha[] = "49782a53d27664c5d58494e69d347410e7996f2af1687bb3ea8e2820ab720c98";
constexpr char old_image_sha[] = "f8084934586ad894d5c740d4ac6d7232b00dd48da47f12f1c8206477261f021f";
constexpr char old_segment_sha[] = "9e0aab3c447573643371248494d4e94f2a3a8dd65716cab3a84e849b5ba80149";
constexpr wchar_t confirmation[] = L"--confirm-1235-8202-1051-to-1116";
constexpr DWORD upgrade_index = 1, env_index = 2;
constexpr char power_on[] = "PhantomPower=0";
constexpr char power_off[] = "PhantomPower=1";
static_assert(sizeof(power_on) == 15 && sizeof(power_off) == 15);

enum class Action { capability, global, segment, read, erase, progress, program, restart };
enum class Access { shared_read, exclusive_probe, flash };
struct Wire {
    std::uint16_t family, op;
    DWORD in, out;
};

template<class T> View bytes_of(const T& value) {
    return {reinterpret_cast<const BYTE*>(&value), sizeof(value)};
}

constexpr bool supported_build(unsigned version) {
    return version == old_build || version == new_build;
}

constexpr DWORD capacity(DWORD index) {
    return index == upgrade_index ? segment_bytes : index == env_index ? env_bytes : 0;
}

bool power_setting(View payload, bool enabled) {
    return payload.size() == sizeof(power_on) &&
           std::memcmp(payload.data(), enabled ? power_on : power_off, sizeof(power_on)) == 0;
}

Wire wire(Action action, View body, DWORD index, bool writing) {
    const auto size = static_cast<DWORD>(body.size());
    require(body.size() <= 1024, "command-payload-bound");
    switch (action) {
    case Action::capability:
        require(size == 2 && get<std::uint16_t>(body.data(), 0) == 4, "capability-command");
        return {0, 1, 2, 1};
    case Action::global:
        require(size == 0, "global-command");
        return {4, 0, 0, 16};
    case Action::segment:
        require(size == 4 && get<DWORD>(body.data(), 0) < 4, "segment-command");
        return {4, 1, 4, 24};
    case Action::read: {
        const auto limit = capacity(index);
        require(size == 12 && limit && get<DWORD>(body.data(), 0) == index, "read-index");
        const auto offset = get<DWORD>(body.data(), 4);
        const auto count = get<DWORD>(body.data(), 8);
        require(count > 0 && count <= read_chunk && offset <= limit &&
                count <= limit - offset, "read-bound");
        return {4, 5, 12, count};
    }
    case Action::erase:
    case Action::progress:
        require(writing && capacity(index) && size == 8 && get<DWORD>(body.data(), 0) == index &&
                get<DWORD>(body.data(), 4) == 0, "erase-progress-command");
        return {4, static_cast<std::uint16_t>(action == Action::erase ? 2 : 3), 8,
                action == Action::erase ? 0ul : 1ul};
    case Action::program: {
        require(writing && capacity(index) && size > 12 && get<DWORD>(body.data(), 0) == index &&
                get<DWORD>(body.data(), 8) == 0, "program-command");
        const auto offset = get<DWORD>(body.data(), 4);
        const auto count = size - 12;
        const auto in_image = [&](DWORD length) {
            return offset < length && offset % program_chunk == 0 &&
                   count == std::min(program_chunk, length - offset);
        };
        require(index == upgrade_index ? in_image(old_image_bytes) || in_image(image_bytes) :
                offset == 0 && (power_setting(body.subspan(12), true) || power_setting(body.subspan(12), false)),
                "program-bound");
        return {4, 4, size, 0};
    }
    case Action::restart:
        require(writing && capacity(index) && size == 0, "restart-command");
        return {0, 3, 0, 0};
    }
    fail("unknown-action");
}

View reply(View frame, DWORD returned, DWORD expected) {
    require(frame.size() == 8 + expected && returned == frame.size(), "PAL-completion-size");
    const auto status = get<Response>(frame.data(), 0);
    if (status.status != 0) fail("PAL-device-status", status.status);
    require(status.bytes == expected, "PAL-reply-length");
    return frame.subspan(8);
}

std::string sha256(View bytes) {
    Bytes object;
    Hash h;
    require(BCryptOpenAlgorithmProvider(&h.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0,
            "SHA256-provider");
    ULONG bytes_out = 0, object_size = 0;
    require(BCryptGetProperty(h.algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &bytes_out, 0) == 0 &&
            bytes_out == sizeof(object_size) && object_size <= 4096, "SHA256-object-size");
    object.resize(object_size);
    require(BCryptCreateHash(h.algorithm, &h.hash, object.data(), object_size, nullptr, 0, 0) == 0 &&
            BCryptHashData(h.hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) == 0,
            "SHA256-data");
    std::array<BYTE, 32> digest{};
    require(BCryptFinishHash(h.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0,
            "SHA256-finish");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto b : digest) {
        result += hex[b >> 4];
        result += hex[b & 15];
    }
    return result;
}

void verify_image(View image, unsigned version) {
    require(supported_build(version) && image.size() == (version == old_build ? old_image_bytes : image_bytes),
            "image-size");
    require(sha256(image) == (version == old_build ? old_image_sha : image_sha), "image-SHA256");
}

[[maybe_unused]] Bytes load_image(const wchar_t* path, unsigned version = new_build) {
    Handle file(CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) fail("image-open", GetLastError());
    LARGE_INTEGER size{};
    const auto length = version == old_build ? old_image_bytes : image_bytes;
    require(supported_build(version) && GetFileSizeEx(file.value, &size) && size.QuadPart == length, "image-size");
    Bytes image(length);
    DWORD count = 0;
    require(ReadFile(file.value, image.data(), length, &count, nullptr) && count == length,
            "image-read");
    verify_image(image, version);
    return image;
}

class Pal {
    Handle root_file;
    Handle usb_file;
    Handle device;
    Buffer root{};
    Buffer desc{};
    std::uint64_t token = 0;
    bool writing;

    static void pin(Handle& file, const wchar_t* name, const char* hash) {
        file.value = CreateFileW(system_driver(name).c_str(), GENERIC_READ, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
        if (file.value == INVALID_HANDLE_VALUE) fail("OEM-driver-open", GetLastError());
        check_hash(file.value, hash);
    }

public:
    explicit Pal(Access access) : writing(access == Access::flash) {
        pin(root_file, L"FocusriteUsbSwRoot.sys",
            "59ecc4d030e5dec83c1d948e807beba0006fa4e833a631b68804b8782ec42c76");
        pin(usb_file, L"FocusriteUsb.sys",
            "498f4644c737c829fdd8853f9be79a4fbf65c812b623818df611587779135b80");
        const auto path = pal_path();
        const bool exclusive = access != Access::shared_read;
        device.value = CreateFileW(path.c_str(), exclusive ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
            exclusive ? 0 : FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, nullptr);
        if (device.value == INVALID_HANDLE_VALUE) fail("PAL-open", GetLastError());
        root = query(device.value, Query::root, 0);
        token = selected(root);
        desc = query(device.value, Query::device_descriptor, token);
        descriptor(desc);
    }
    unsigned version() const { return get<std::uint16_t>(desc.data(), 20); }

    void stable() {
        const auto first = query(device.value, Query::root, 0);
        require(selected(first) == token, "selected-device-changed");
        const auto current = query(device.value, Query::device_descriptor, token);
        const auto last = query(device.value, Query::root, 0);
        require(selected(last) == token && std::memcmp(root.data() + 16, first.data() + 16, 32) == 0 &&
                std::memcmp(root.data() + 16, last.data() + 16, 32) == 0 &&
                std::memcmp(desc.data() + 8, current.data() + 8, 18) == 0, "device-changed");
    }

    Bytes send(Action action, View payload, DWORD index) {
        const auto shape = wire(action, payload, index, writing);
        std::array<BYTE, 16 + 1024> input{};
        const Request header{token, shape.family, shape.op, 0};
        std::memcpy(input.data(), &header, sizeof(header));
        if (!payload.empty()) std::memcpy(input.data() + sizeof(header), payload.data(), payload.size());
        Bytes output(8 + shape.out);
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.value) fail("PAL-event", GetLastError());
        OVERLAPPED pending{};
        pending.hEvent = event.value;
        DWORD returned = 0;
        if (!DeviceIoControl(device.value, ioctl, input.data(), 16 + shape.in,
                             output.data(), 8 + shape.out, &returned, &pending)) {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) fail("PAL-submit", error);
            const DWORD wait = WaitForSingleObject(event.value, 2000);
            if (wait != WAIT_OBJECT_0) {
                const DWORD wait_error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
                (void)CancelIoEx(device.value, &pending);
                // Keep all IRP storage alive until completion, even if cancel is slow.
                while (!GetOverlappedResult(device.value, &pending, &returned, TRUE)) {
                    const auto error_now = GetLastError();
                    if (error_now != ERROR_IO_PENDING && error_now != ERROR_IO_INCOMPLETE) break;
                }
                fail("PAL-deadline-after-submit", wait_error);
            }
        }
        if (!GetOverlappedResult(device.value, &pending, &returned, TRUE))
            fail("PAL-completion", GetLastError());
        const auto data = reply(output, returned, shape.out);
        return {data.begin(), data.end()};
    }
};

template<class Transport> DWORD locate_upgrade(Transport& device) {
    const std::uint16_t capability = 4;
    const auto cap = device.send(Action::capability, bytes_of(capability), 0);
    require(cap.size() == 1 && cap[0] != 0, "NVM-capability");
    const auto global = device.send(Action::global, {}, 0);
    require(global.size() == 16 && get<DWORD>(global.data(), 0) == 1048576 &&
            get<DWORD>(global.data(), 4) == 4, "unexpected-NVM-layout");
    constexpr const char* names[] = {"App_Gold", "App_Upgrade", "App_Env", "App_Settings"};
    constexpr DWORD sizes[] = {262144, segment_bytes, 65536, 262144};
    constexpr DWORD flags[] = {12, 2, 64, 48};
    DWORD upgrade = 4;
    for (DWORD index = 0; index < 4; ++index) {
        const auto data = device.send(Action::segment, bytes_of(index), index);
        require(data.size() == 24 && get<DWORD>(data.data(), 0) == sizes[index] &&
                get<DWORD>(data.data(), 4) == flags[index] &&
                std::memcmp(data.data() + 8, names[index], std::strlen(names[index]) + 1) == 0,
                "unexpected-NVM-segment");
        if (std::strcmp(names[index], "App_Upgrade") == 0) upgrade = index;
    }
    require(upgrade == 1, "missing-App_Upgrade");
    return upgrade;
}

template<class Transport> Bytes read_segment(Transport& device, DWORD index, DWORD length) {
    require(length > 0 && length <= capacity(index), "read-segment-length");
    Bytes result;
    result.reserve(length);
    for (DWORD offset = 0; offset < length; offset += read_chunk) {
        const std::array<DWORD, 3> header{index, offset, std::min(read_chunk, length - offset)};
        auto chunk = device.send(Action::read, bytes_of(header), index);
        require(chunk.size() == header[2], "readback-size");
        result.insert(result.end(), chunk.begin(), chunk.end());
    }
    return result;
}

template<class Transport> Bytes read_upgrade(Transport& device, DWORD index) {
    return read_segment(device, index, segment_bytes);
}

bool erased_tail(View segment, std::size_t written) {
    return written <= segment.size() &&
           std::all_of(segment.begin() + written, segment.end(), [](BYTE byte) { return byte == 0xff; });
}

bool segment_matches(View segment, View payload) {
    return erased_tail(segment, payload.size()) && std::equal(payload.begin(), payload.end(), segment.begin());
}

bool power_mode(View prefix) {
    if (power_setting(prefix, true)) return true;
    if (power_setting(prefix, false)) return false;
    fail("unknown-PhantomPower-setting");
}

template<class Transport> bool read_power(Transport& device) {
    (void)locate_upgrade(device);
    const auto prefix = read_segment(device, env_index, sizeof(power_on));
    device.stable();
    return power_mode(prefix);
}

template<class Transport> Bytes stable_segment(Transport& device, DWORD index) {
    (void)locate_upgrade(device);
    auto data = read_segment(device, index, capacity(index));
    require(data == read_segment(device, index, capacity(index)), "unstable-NVM-backup");
    device.stable();
    return data;
}

template<class Transport> Bytes prepare_power(Transport& device, bool expected) {
    auto data = stable_segment(device, env_index);
    require(power_mode(View(data).first(sizeof(power_on))) == expected, "power-setting-changed");
    require(erased_tail(data, sizeof(power_on)), "App_Env-contains-additional-data");
    return data;
}

template<class Transport> Bytes prepare_firmware(Transport& device, unsigned version) {
    require(supported_build(version) && device.version() == version, "firmware-changed");
    auto data = stable_segment(device, upgrade_index);
    const auto length = version == old_build ? old_image_bytes : image_bytes;
    const auto* expected = version == old_build ? old_image_sha : image_sha;
    require(sha256(View(data).first(length)) == expected && erased_tail(data, length),
            "unexpected-current-App_Upgrade");
    return data;
}

template<class Transport> Bytes preflight(Transport& device) {
    require(device.version() == old_build, "expected-current-build-1051");
    const auto index = locate_upgrade(device);
    const auto backup = read_upgrade(device, index);
    require(sha256(backup) == old_segment_sha, "unexpected-current-App_Upgrade");
    require(backup == read_upgrade(device, index), "unstable-App_Upgrade-backup");
    device.stable();
    return backup;
}

template<class Transport, class Stage, class Sleep>
void program_segment(Transport& device, DWORD index, View payload, Stage stage, Sleep sleep) {
    const auto length = static_cast<DWORD>(payload.size());
    require(capacity(index) && supported_build(device.version()), "segment-precondition");
    if (index == upgrade_index) {
        const unsigned target = length == old_image_bytes ? old_build : new_build;
        verify_image(payload, target);
        require(target != device.version(), "already-target-build");
    } else {
        (void)power_mode(payload);
    }
    device.stable();
    const std::array<DWORD, 2> erase{index, 0};
    stage("erase-requested");
    device.send(Action::erase, bytes_of(erase), index);
    bool erased = false;
    for (unsigned poll = 0; poll < erase_poll_limit; ++poll) {
        const auto progress = device.send(Action::progress, bytes_of(erase), index);
        require(progress.size() == 1, "erase-progress-size");
        if (progress[0] == 0xff) { erased = true; break; }
        sleep();
    }
    if (!erased) fail("erase-progress-deadline", ERROR_TIMEOUT);

    stage("programming");
    std::array<BYTE, 1024> packet{};
    for (DWORD offset = 0; offset < length; offset += program_chunk) {
        const auto count = std::min(program_chunk, length - offset);
        const std::array<DWORD, 3> header{index, offset, 0};
        std::memcpy(packet.data(), header.data(), sizeof(header));
        std::memcpy(packet.data() + sizeof(header), payload.data() + offset, count);
        device.send(Action::program, {packet.data(), sizeof(header) + count}, index);
    }

    stage("verifying-readback");
    const auto actual = read_segment(device, index, capacity(index));
    require(segment_matches(actual, payload), "program-readback-mismatch");
    device.stable();
    stage("restart-requested");
    device.send(Action::restart, {}, index);
}

template<class Transport, class Stage, class Sleep>
void program_upgrade(Transport& device, DWORD index, View image, Stage stage, Sleep sleep) {
    require(device.version() == old_build && index == upgrade_index && image.size() == image_bytes &&
            sha256(image) == image_sha, "flash-precondition");
    program_segment(device, index, image, stage, sleep);
}

void write_file(HANDLE file, View data) {
    DWORD written = 0;
    if (!WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr))
        fail("save-backup-or-state", GetLastError());
    if (written != data.size()) fail("short-file-write", ERROR_WRITE_FAULT);
    if (!FlushFileBuffers(file)) fail("flush-backup-or-state", GetLastError());
}

class Backup {
    Handle journal;
public:
    void save(const std::wstring& dir, const wchar_t* filename, View data) {
        if (!CreateDirectoryW(dir.c_str(), nullptr)) fail("create-new-backup-directory", GetLastError());
        Handle file(CreateFileW((dir + L"\\" + filename).c_str(), GENERIC_WRITE, 0,
                                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.value == INVALID_HANDLE_VALUE) fail("create-backup", GetLastError());
        write_file(file.value, data);
        journal.value = CreateFileW((dir + L"\\update-state.txt").c_str(), GENERIC_WRITE,
                                    FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (journal.value == INVALID_HANDLE_VALUE) fail("create-journal", GetLastError());
    }
    void stage(const char* name) {
        const std::string line = std::string(name) + "\r\n";
        write_file(journal.value, {reinterpret_cast<const BYTE*>(line.data()), line.size()});
    }
};

#ifndef SCARLETT_GUI
int flash(View image, const wchar_t* backup_dir) {
    Backup saved;
    const char* phase = "preflight";
    const auto stage = [&](const char* name) {
        saved.stage(name);
        phase = name;
        std::printf("%s\n", name);
        std::fflush(stdout);
    };
    try {
        {
            Pal device(Access::flash);
            const auto backup = preflight(device);
            saved.save(backup_dir, L"App_Upgrade-1051.bin", backup);
            stage("backup-saved");
            program_upgrade(device, 1, image, stage, [] { Sleep(100); });
        }
        stage("awaiting-1116");
        // shortcut: ordinary PAL reconnect can require a physical USB replug.
        // Report that separately from a failed program/readback.
        for (unsigned i = 0; i < 30; ++i) {
            Sleep(1000);
            bool verified = false;
            try {
                Pal current(Access::shared_read);
                current.stable();
                verified = current.version() == new_build;
            } catch (const Error&) {
                // Device/service can be absent while it re-enumerates.
            }
            if (verified) {
                stage("verified-1116");
                return 0;
            }
        }
        std::fputs("Image readback matched. Activation unverified; reconnect once and use --detect. Do not reflash.\n",
                   stderr);
        return 5;
    } catch (const Error& e) {
        std::fprintf(stderr, "Stopped after %s: %s (%lu). No automatic retry or rollback.\n",
                     phase, e.step, e.code);
        return 3;
    } catch (...) {
        std::fprintf(stderr, "Unexpected failure after %s. Check the journal before any further action.\n", phase);
        return 4;
    }
}

#include "self-test.inc"
#endif
} // namespace

#ifndef SCARLETT_GUI
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 2 && std::wcscmp(argv[1], L"--detect") == 0) {
            Pal device(Access::shared_read);
            device.stable();
            std::printf("Scarlett 2i2 Gen 2 1235:8202, firmware %u (0x%04X)\n",
                        device.version(), device.version());
            return 0;
        }
        if (argc == 2 && std::wcscmp(argv[1], L"--read") == 0) {
            Pal device(Access::shared_read);
            const auto index = locate_upgrade(device);
            const auto first = read_upgrade(device, index);
            const auto second = read_upgrade(device, index);
            require(first == second, "read-only-pass-mismatch");
            device.stable();
            const bool contains1116 = sha256(View(first).first(image_bytes)) == image_sha;
            std::printf("{\"vid_pid\":\"1235:8202\",\"firmware\":%u,\"segment\":\"App_Upgrade\","
                        "\"index\":%lu,\"bytes\":%lu,\"passes_match\":true,\"sha256\":\"%s\","
                        "\"contains_1116_image\":%s}\n", device.version(), index, segment_bytes,
                        sha256(first).c_str(), contains1116 ? "true" : "false");
            return 0;
        }
        if (argc == 3 && std::wcscmp(argv[1], L"--verify-image") == 0) {
            (void)load_image(argv[2]);
            std::printf("Verified 1235:8202 firmware 1116, %lu bytes, SHA-256 %s\n", image_bytes, image_sha);
            return 0;
        }
        if (argc == 3 && std::wcscmp(argv[1], L"--preflight") == 0) {
            (void)load_image(argv[2]);
            Pal device(Access::exclusive_probe);
            (void)preflight(device);
            std::puts("PREFLIGHT PASS: exclusive PAL, 1235:8202, current 1051, verified original App_Upgrade and target 1116.");
            return 0;
        }
        if (argc == 3 && std::wcscmp(argv[1], L"--self-test") == 0)
            return self_test(load_image(argv[2]));
        if (argc == 4 && std::wcscmp(argv[1], L"--self-test-full") == 0)
            return full_test(load_image(argv[2], old_build), load_image(argv[3]));
        if (argc == 5 && std::wcscmp(argv[1], L"--flash") == 0 && std::wcscmp(argv[4], confirmation) == 0)
            return flash(load_image(argv[2]), argv[3]);

        std::fputs("Usage: scarlett1116 --detect | --read | --verify-image <bin> | --preflight <bin> | --self-test <bin>\n"
                   "       scarlett1116 --self-test-full <1051-bin> <1116-bin>\n"
                   "       scarlett1116 --flash <bin> <new-backup-dir> --confirm-1235-8202-1051-to-1116\n",
                   argc == 1 ? stdout : stderr);
        return argc == 1 ? 0 : 2;
    } catch (const Error& e) {
        std::fprintf(stderr, "%s (%lu)\n", e.step, e.code);
        return 3;
    } catch (...) {
        std::fputs("Unexpected error.\n", stderr);
        return 4;
    }
}
#endif
