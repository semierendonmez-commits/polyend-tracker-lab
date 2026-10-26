// station_info: prints a station's tagline and theme accent from its simulator
// library (built by sdk/station/station.mk), for tools/patch_info.py.
//   station_info build/stations/flipper.so   ->  "<tagline>\n0xRRGGBB\n"
#include <dlfcn.h>

#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    void* h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        std::fprintf(stderr, "station_info: %s\n", dlerror());
        return 1;
    }
    using Fn = int (*)(char*, unsigned, unsigned*);
    Fn f = reinterpret_cast<Fn>(dlsym(h, "plat_station_describe"));
    char tag[128] = {};
    unsigned accent = 0;
    if (!f || !f(tag, sizeof tag, &accent)) return 1;
    std::printf("%s\n0x%06X\n", tag, accent & 0xFFFFFFu);
    return 0;
}
