// Compiled without the global arch flags: it has to run on a CPU the rest of the build cannot.
#if defined(GDC_REQUIRE_X86_64_V3)

#include <cstdio>
#include <cstdlib>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

void cpuid(unsigned leaf, unsigned subleaf, unsigned r[4])
{
#if defined(_MSC_VER)
    int out[4];
    __cpuidex(out, int(leaf), int(subleaf));
    for (int i = 0; i < 4; ++i) {
        r[i] = unsigned(out[i]);
    }
#else
    __cpuid_count(leaf, subleaf, r[0], r[1], r[2], r[3]);
#endif
}

unsigned long long xgetbv0()
{
#if defined(_MSC_VER)
    return _xgetbv(0);
#else
    unsigned lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (unsigned long long)hi << 32 | lo;
#endif
}

bool hasX86_64V3()
{
    unsigned r[4];
    cpuid(0, 0, r);
    const unsigned maxLeaf = r[0];
    if (maxLeaf < 7) {
        return false;
    }
    cpuid(1, 0, r);
    const unsigned ecx1 = r[2];
    const bool v2 = (ecx1 & (1u << 0)) && (ecx1 & (1u << 9)) && (ecx1 & (1u << 13)) && (ecx1 & (1u << 19))
        && (ecx1 & (1u << 20)) && (ecx1 & (1u << 23));
    const bool v3a = (ecx1 & (1u << 12)) && (ecx1 & (1u << 22)) && (ecx1 & (1u << 29));
    const bool osxsave = (ecx1 & (1u << 27)) && (ecx1 & (1u << 28));
    if (!v2 || !v3a || !osxsave || (xgetbv0() & 6) != 6) {
        return false;
    }
    cpuid(7, 0, r);
    const unsigned ebx7 = r[1];
    if (!(ebx7 & (1u << 3)) || !(ebx7 & (1u << 5)) || !(ebx7 & (1u << 8))) {
        return false;
    }
    cpuid(0x80000000u, 0, r);
    if (r[0] < 0x80000001u) {
        return false;
    }
    cpuid(0x80000001u, 0, r);
    return (r[2] & (1u << 5)) != 0;
}

const char kMessage[] =
    "goDragonCooker needs a CPU with x86-64-v3 support (AVX2, FMA, BMI2): "
    "Intel Haswell (2013), AMD Zen (2017) or newer.";

void checkCpu()
{
    if (hasX86_64V3()) {
        return;
    }
    std::fputs(kMessage, stderr);
    std::fputs("\n", stderr);
#if defined(_WIN32)
    if (!GetConsoleWindow()) {
        MessageBoxA(nullptr, kMessage, "goDragonCooker", MB_OK | MB_ICONERROR);
    }
    ExitProcess(1);
#else
    std::_Exit(1);
#endif
}

} // namespace

#if defined(_MSC_VER)
#pragma warning(disable : 4073)
#pragma init_seg(lib)
static const int cpuChecked = (checkCpu(), 0);
#else
__attribute__((constructor(101))) static void runCpuCheck() { checkCpu(); }
#endif

#endif
