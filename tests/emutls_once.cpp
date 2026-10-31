// std::call_once keeps its callable in libstdc++'s thread_local state: the
// inline call_once here and the DLL's __once_proxy must resolve it alike.
#include <mutex>
#include <string>

static thread_local std::string text = "thread";

extern "C" int emutls_call_once_check(void)
{
    static std::once_flag flag;
    int calls = 0;
    for (int i = 0; i < 3; ++i) std::call_once(flag, [&calls] { ++calls; });
    text += "-local";
    return calls == 1 && text == "thread-local";
}
