#include "fetchkit/version.h"

#include <cstdio>
#include <cstring>

int main(int argc, char* argv[]) {
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("%s\n", fetchkit::version());
        return 0;
    }

    if (argc != 1) {
        std::fprintf(stderr, "Usage: fetchkit_cli [--version]\n");
        return 2;
    }

    std::printf("FetchKit %s\n", fetchkit::version());
    std::printf("SDK scaffold: download functionality is not implemented yet.\n");
    return 0;
}
