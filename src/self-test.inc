struct Fake {
    enum class Fault { none, erase_rejected, never_ready, program_timeout, corrupt_readback, restart_timeout };
    Fault fault = Fault::none;
    unsigned build = old_build;
    unsigned target_build = new_build;
    DWORD target_index = upgrade_index;
    DWORD target_size = image_bytes;
    bool changed = false;
    bool wrong_layout = false;
    bool erased = false;
    unsigned erases = 0, polls = 0, programs = 0, reads = 0, restarts = 0;
    DWORD next_offset = 0;
    Bytes flash_bytes = Bytes(segment_bytes, 0x5a);
    Bytes environment = Bytes(env_bytes, 0xff);

    unsigned version() const { return build; }
    void stable() const { require(!changed, "fake-device-changed"); }

    Bytes send(Action action, View payload, DWORD index) {
        const auto shape = wire(action, payload, index, true);
        auto& memory = index == env_index ? environment : flash_bytes;
        switch (action) {
        case Action::capability:
            require(shape.family == 0 && shape.op == 1 && payload[0] == 4 && payload[1] == 0, "fake-capability-wire");
            return {1};
        case Action::global: {
            require(shape.family == 4 && shape.op == 0, "fake-global-wire");
            const std::array<DWORD, 4> global{1048576, wrong_layout ? 3ul : 4ul, 0, 0};
            const auto data = bytes_of(global);
            return {data.begin(), data.end()};
        }
        case Action::segment: {
            require(shape.family == 4 && shape.op == 1 && get<DWORD>(payload.data(), 0) == index, "fake-segment-wire");
            constexpr const char* names[] = {"App_Gold", "App_Upgrade", "App_Env", "App_Settings"};
            const std::array<DWORD, 2> header[]{{262144, 12}, {262144, 2}, {65536, 64}, {262144, 48}};
            Bytes result(24);
            std::memcpy(result.data(), header[index].data(), 8);
            std::memcpy(result.data() + 8, names[index], std::strlen(names[index]) + 1);
            return result;
        }
        case Action::erase:
            require(shape.family == 4 && shape.op == 2 && index == target_index && erases == 0, "fake-erase-wire");
            ++erases;
            if (fault == Fault::erase_rejected) fail("fake-erase-rejected", 5);
            return {};
        case Action::progress:
            require(shape.family == 4 && shape.op == 3 && index == target_index &&
                    erases == 1 && programs == 0, "fake-progress-wire");
            ++polls;
            if (polls == 1 || fault == Fault::never_ready) return {1};
            erased = true;
            reads = 0;
            std::fill(memory.begin(), memory.end(), BYTE{0xff});
            return {0xff};
        case Action::program: {
            const auto offset = get<DWORD>(payload.data(), 4);
            require(shape.family == 4 && shape.op == 4 && index == target_index &&
                    erased && offset == next_offset, "fake-program-wire-order");
            const auto data = payload.subspan(12);
            std::copy(data.begin(), data.end(), memory.begin() + offset);
            next_offset += static_cast<DWORD>(data.size());
            ++programs;
            // This chunk reached the device before the transport reported failure.
            if (fault == Fault::program_timeout && programs == 3) fail("fake-program-timeout-after-submit", ERROR_TIMEOUT);
            return {};
        }
        case Action::read: {
            require(shape.family == 4 && shape.op == 5, "fake-read-wire");
            const auto offset = get<DWORD>(payload.data(), 4);
            ++reads;
            auto start = memory.begin() + offset;
            Bytes result(start, start + shape.out);
            if (fault == Fault::corrupt_readback && erased && offset == 0) result[0] ^= 1;
            return result;
        }
        case Action::restart:
            require(shape.family == 0 && shape.op == 3 && index == target_index && next_offset == target_size &&
                    reads == capacity(index) / read_chunk, "fake-restart-before-verified");
            ++restarts;
            if (fault == Fault::restart_timeout) fail("fake-restart-timeout", ERROR_TIMEOUT);
            if (index == upgrade_index) build = target_build;
            return {};
        }
        fail("unknown-fake-action");
    }
};

template<class F> void expect_error(const char* step, F fn) {
    bool threw = false;
    try { fn(); }
    catch (const Error& e) {
        require(std::strcmp(e.step, step) == 0, "unexpected-self-test-error");
        threw = true;
    }
    require(threw, "self-test-expected-error");
}

int self_test(Bytes image) {
    Fake good;
    require(locate_upgrade(good) == 1, "fake-layout");
    std::vector<std::string> stages;
    const auto record = [&](const char* stage) { stages.emplace_back(stage); };
    const auto skip_sleep = [] {};
    program_upgrade(good, 1, image, record, skip_sleep);
    const std::vector<std::string> expected{"erase-requested", "programming", "verifying-readback", "restart-requested"};
    require(stages == expected && good.erases == 1 && good.polls == 2 && good.programs == 64 &&
            good.next_offset == image_bytes && good.restarts == 1 && good.build == 1116 &&
            std::equal(image.begin(), image.end(), good.flash_bytes.begin()), "fake-happy-path");

    Fake reader;
    const auto index = locate_upgrade(reader);
    require(read_upgrade(reader, index) == reader.flash_bytes &&
            reader.erases == 0 && reader.programs == 0 && reader.restarts == 0, "fake-read-only");
    reader.wrong_layout = true;
    expect_error("unexpected-NVM-layout", [&] { (void)locate_upgrade(reader); });

    const auto exercise = [&](Fake::Fault fault, const char* failure, unsigned programs, unsigned restarts) {
        Fake target;
        target.fault = fault;
        expect_error(failure, [&] { program_upgrade(target, 1, image, [](const char*) {}, skip_sleep); });
        require(target.erases == 1 && target.programs == programs && target.restarts == restarts &&
                target.build == old_build, "fake-abort-sequence");
    };
    exercise(Fake::Fault::erase_rejected, "fake-erase-rejected", 0, 0);
    exercise(Fake::Fault::never_ready, "erase-progress-deadline", 0, 0);
    exercise(Fake::Fault::program_timeout, "fake-program-timeout-after-submit", 3, 0);
    exercise(Fake::Fault::corrupt_readback, "program-readback-mismatch", 64, 0);
    exercise(Fake::Fault::restart_timeout, "fake-restart-timeout", 64, 1);

    Fake wrong;
    expect_error("flash-precondition", [&] { program_upgrade(wrong, 3, image, record, skip_sleep); });
    wrong.build = new_build;
    expect_error("flash-precondition", [&] { program_upgrade(wrong, 1, image, record, skip_sleep); });
    wrong.build = old_build;
    wrong.changed = true;
    expect_error("fake-device-changed", [&] { program_upgrade(wrong, 1, image, record, skip_sleep); });
    wrong.changed = false;
    image[0] ^= 1;
    expect_error("flash-precondition", [&] { program_upgrade(wrong, 1, image, record, skip_sleep); });
    require(wrong.erases == 0, "fake-preflight-no-erase");

    expect_error("erase-progress-command", [] {
        const std::array<DWORD, 2> erase{1, 0};
        (void)wire(Action::erase, bytes_of(erase), 1, false);
    });
    expect_error("read-bound", [] {
        const std::array<DWORD, 3> bad{1, segment_bytes - 1, 2};
        (void)wire(Action::read, bytes_of(bad), 1, false);
    });
    expect_error("program-bound", [] {
        const std::array<DWORD, 4> bad{1, image_bytes, 0, 0};
        (void)wire(Action::program, bytes_of(bad), 1, true);
    });
    const Response ok{0, 0}, rejected{5, 0}, wrong_length{0, 1};
    require(reply(bytes_of(ok), 8, 0).empty(), "zero-payload-reply");
    expect_error("PAL-completion-size", [&] { (void)reply(bytes_of(ok), 7, 0); });
    expect_error("PAL-device-status", [&] { (void)reply(bytes_of(rejected), 8, 0); });
    expect_error("PAL-reply-length", [&] { (void)reply(bytes_of(wrong_length), 8, 0); });
    std::puts("SELF-TEST PASS: model-bound preflight, erase/poll, 64 chunks, full readback, restart, abort paths");
    return 0;
}

int full_test(View previous, const Bytes& current) {
    self_test(current);
    const auto skip_sleep = [] {};
    const auto skip_stage = [](const char*) {};

    for (const auto version : {old_build, new_build}) {
        Fake device;
        device.build = version == old_build ? new_build : old_build;
        device.target_build = version;
        const View start = version == old_build ? View(current) : previous;
        const View target = version == old_build ? previous : View(current);
        std::fill(device.flash_bytes.begin(), device.flash_bytes.end(), BYTE{0xff});
        std::copy(start.begin(), start.end(), device.flash_bytes.begin());
        const auto original_env = device.environment;
        device.target_size = static_cast<DWORD>(target.size());
        require(prepare_firmware(device, device.build) == device.flash_bytes &&
                device.erases == 0, "fake-firmware-backup");
        program_segment(device, upgrade_index, target, skip_stage, skip_sleep);
        require(device.build == version && segment_matches(device.flash_bytes, target) &&
                device.environment == original_env && device.erases == 1 && device.restarts == 1,
                "fake-firmware-target");
    }

    for (const bool enabled : {false, true}) {
        Fake device;
        device.build = new_build;
        device.target_index = env_index;
        device.target_size = sizeof(power_on);
        const auto unmodified = device.flash_bytes;
        const auto before = bytes_of(enabled ? power_on : power_off);
        const auto after = bytes_of(enabled ? power_off : power_on);
        std::copy(before.begin(), before.end(), device.environment.begin());
        require(read_power(device) == enabled && prepare_power(device, enabled) == device.environment &&
                !device.erases && !device.programs && !device.restarts, "fake-power-read");
        expect_error("power-setting-changed", [&] { (void)prepare_power(device, !enabled); });
        device.environment.back() = 0;
        expect_error("App_Env-contains-additional-data", [&] { (void)prepare_power(device, enabled); });
        require(!device.erases, "fake-extra-data-no-erase");
        device.environment.back() = 0xff;
        program_segment(device, env_index, after, skip_stage, skip_sleep);
        require(read_power(device) == !enabled && segment_matches(device.environment, after) &&
                device.build == new_build && device.flash_bytes == unmodified &&
                device.erases == 1 && device.programs == 1 && device.restarts == 1, "fake-power-updated");
    }

    Fake bad;
    bad.build = new_build;
    expect_error("unknown-PhantomPower-setting", [&] { (void)read_power(bad); });
    expect_error("unexpected-current-App_Upgrade", [&] { (void)prepare_firmware(bad, new_build); });
    require(!bad.erases, "fake-unexpected-state-no-erase");
    bad.target_index = env_index;
    bad.target_size = sizeof(power_on);
    std::copy_n(reinterpret_cast<const BYTE*>(power_off), sizeof(power_off), bad.environment.begin());
    (void)prepare_power(bad, false);
    bad.fault = Fake::Fault::corrupt_readback;
    expect_error("program-readback-mismatch", [&] {
        program_segment(bad, env_index, bytes_of(power_on), skip_stage, skip_sleep);
    });
    require(bad.erases == 1 && bad.programs == 1 && bad.restarts == 0, "fake-power-write-failure");
    std::puts("GUI TRANSPORT TEST PASS: 1051 -> 1116, 1116 -> 1051, mode read and both toggles, safe refusal and failed write");
    return 0;
}
