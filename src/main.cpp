#include <cstring>
#include <utility>

#include "Frontend.hpp"

int main(int argc, char* argv[]) {
    FrontendOptions opts;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--demo") == 0) {
            opts.demo = true;
        } else if (std::strcmp(argv[i], "--trace") == 0) {
            opts.trace = true;
        } else if ((std::strcmp(argv[i], "-b") == 0 ||
                    std::strcmp(argv[i], "--bios") == 0) &&
                   i + 1 < argc) {
            opts.biosPath = argv[++i];
        } else {
            opts.romPath = argv[i];
        }
    }

    Frontend frontend(std::move(opts));
    return frontend.run();
}
