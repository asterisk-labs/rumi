#include "rumi/rumi.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <unistd.h>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size)
{
    char path[] = "/tmp/rumi-index-fuzz-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) return 0;

    std::FILE* file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        unlink(path);
        return 0;
    }
    (void)std::fwrite(data, 1, size, file);
    (void)std::fclose(file);

    unsigned char* header = nullptr;
    std::size_t header_size = 0;
    if (rumi_index_file(path, &header, &header_size) == RUMI_OK) {
        rumi_free(header);
    }
    rumi_clear_error();
    (void)unlink(path);
    return 0;
}
