## mbed wrapper for [SPIFFS (SPI Flash File System)](https://github.com/pellepl/spiffs)

This is the mbed wrapper for [SPIFFS](https://github.com/pellepl/spiffs),
a wear-leveled SPI flash file system.

SPIFFS comes with the following features:

- **Static wear leveling** - SPIFFS is a logging filesystem that cycles
  evenly through all blocks on the underlying storage, utilizing all of the
  erase cycles available on the storage blocks. SPIFFS is designed to
  maximize the longevity of the underlying storage.

- **Power-loss resilient** - SPIFFS uses built in filesystem consistency
  checks to recover after an unexpected power-loss. By default SPIFFS is
  resilient to power-loss. This check can be skipped when a power-loss
  is known to not have occured.

- **Low RAM/ROM usage** - SPIFFS is designed to work in a limited amount
  of memory, prioritizing low RAM usage. SPIFFS can use statically allocated
  buffers and memory consumption is independent from the number of files.

SPIFFS comes with the following limitations:

- **Limited to SPI flash** - SPIFFS relies on bit-twiddling tricks that
  are dependent on the byte-level nanding behaviour of NOR flash. This
  reduces the number of erases SPIFFS needs to perform, but limits SPIFFS
  to SPI flash. If you need to support other devices, consider using the
  [FATFileSystem](https://github.com/ARMmbed/mbed-os/blob/mbed-os-5.5/features/filesystem/fat/FATFileSystem.h)
  found in mbed OS.

- **No directory support** - SPIFFS currently does not support directories.
  If you need directories, consider using the [LittleFileSystem](https://github.com/armmbed/mbed-littlefs) or [FATFileSystem](https://github.com/ARMmbed/mbed-os/blob/mbed-os-5.5/features/filesystem/fat/FATFileSystem.h).

- **No error detection** - SPIFFS does not handle errors that may develop on
  the underlying storage. Instead SPIFFS relies on wear leveling to delay
  the chance that errors occur for as long as possible. If the underlying
  medium is especially prone to developing errors, consider using the
  [LittleFileSystem](https://github.com/armmbed/mbed-littlefs).

## Usage

If you are already using a filesystem in mbed, adopting SPIFFS should
just require a name change to use the [SPIFFileSystem](SPIFFileSystem.h)
class.

Here is a simple example that updates a file named "boot_count" every time
the application runs:
``` c++
#include "SPIFFileSystem.h"
#include "SPIFBlockDevice.h"

// Physical block device, can be any device that supports the BlockDevice API
SPIFBlockDevice bd(PTE2, PTE4, PTE1, PTE5);

// Storage for the littlefs
SPIFFileSystem fs("fs");

// Entry point
int main() {
    // Mount the filesystem
    int err = fs.mount(&bd);
    if (err) {
        // Reformat if we can't mount the filesystem,
        // this should only happen on the first boot
        SPIFFileSystem::format(&bd);
        fs.mount(&bd);
    }

    // Read the boot count
    uint32_t boot_count = 0;
    FILE *f = fopen("/fs/boot_count", "r+");
    if (!f) {
        // Create the file if it doesn't exist
        f = fopen("/fs/boot_count", "w+");
    }
    fread(&boot_count, sizeof(boot_count), 1, f);

    // Update the boot count
    boot_count += 1;
    rewind(f);
    fwrite(&boot_count, sizeof(boot_count), 1, f);

    // Remember that storage may not be updated until the file
    // is closed successfully
    fclose(f);

    // Release any resources we were using
    fs.unmount();

    // Print the boot count
    printf("boot_count: %ld\n", boot_count);
}
```
