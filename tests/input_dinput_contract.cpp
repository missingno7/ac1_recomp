/* Reuse the fake native COM/guest-memory fixture to check the replay boundary. */
#define main ac1_native_dinput_contract_main
#include "../src/platform/tests/dinput8_get_device_state_contract.cpp"
#undef main

int main() {
    _putenv_s("AC1_INPUT_SCRIPT", "");
    _putenv_s("AC1_INPUT_RECORD", "");
    if (ac1_native_dinput_contract_main())
        return 1;
    FILE *file = std::fopen("work/input_dinput_contract.txt", "wb");
    if (!check(file != nullptr, "open script fixture"))
        return 1;
    std::fputs("at 0 key W down\nat 0 mouse right down\n", file);
    std::fclose(file);
    _putenv_s("AC1_INPUT_SCRIPT", "work/input_dinput_contract.txt");
    GuestObject object{};
    object.native = reinterpret_cast<IUnknown *>(&g_fake_device);
    object.kind = ObjectKind::DeviceW;
    object.input_kind = 1;
    g_objects.emplace(kFakeObject, object);
    const uint32_t calls = g_state_calls;
    const uint32_t args[] = {kFakeObject, 256, kStateBuffer};
    uint32_t result = 0xdeadbeef;
    g_state_hresult = DIERR_NOTACQUIRED;
    if (!check(ac1_dinput8_dispatch(device_token(9), args, 3, &result) == AC1_DINPUT8_HANDLED &&
                   result == DI_OK,
               "scripted keyboard works independently of native acquisition") ||
        !check(g_state_calls == calls, "physical GetDeviceState was never called") ||
        !check(g_guest[0x200 + DIK_W] == 0x80 && g_guest[0x200 + DIK_A] == 0,
               "guest receives only scripted keys"))
        return 1;
    g_objects.at(kFakeObject).input_kind = 2;
    const uint32_t mouse_args[] = {kFakeObject, 20, kStateBuffer};
    if (!check(ac1_dinput8_dispatch(device_token(9), mouse_args, 3, &result) ==
                       AC1_DINPUT8_HANDLED &&
                   result == DI_OK && g_state_calls == calls && g_guest[0x200 + 13] == 0x80,
               "guest mouse is scripted without physical state"))
        return 1;
    const uint32_t bad_args[] = {kFakeObject, 17, kStateBuffer};
    if (!check(ac1_dinput8_dispatch(device_token(9), bad_args, 3, &result) ==
                       AC1_DINPUT8_METHOD_UNSUPPORTED &&
                   std::strstr(ac1_dinput8_last_error(), "17"),
               "unexpected mouse layout is diagnosed"))
        return 1;
    ac1_dinput8_shutdown();
    _putenv_s("AC1_INPUT_SCRIPT", "");
    std::puts("input DirectInput contract passed: native forwarding preserved, replay ignores "
              "physical state, unsupported layouts diagnosed");
    return 0;
}
