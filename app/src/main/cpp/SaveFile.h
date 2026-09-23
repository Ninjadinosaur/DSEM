#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Battery-backed save memory (cartridge SRAM/EEPROM/flash, GBA saves, firmware).
// melonDS reports every write; we keep a copy and flush it to disk on the I/O thread
// once writes have settled, and immediately whenever the app is paused (features.md §6).
namespace ds13r
{

class SaveFile
{
public:
    explicit SaveFile(std::string path);
    ~SaveFile();

    const std::string& Path() const { return path; }

    // Called from the emulation thread through Platform::WriteNDSSave and friends.
    void OnWrite(const uint8_t* data, uint32_t len, uint32_t offset, uint32_t writelen);

    // Called once per frame: flushes if the game stopped writing a moment ago.
    void CheckFlush();

    // Writes any pending data and waits until it is on disk.
    void FlushNow();

    // Keeps up to `keep` timestamped copies of the existing file in backupDir (features.md §6).
    static void BackupExisting(const std::string& path, const std::string& backupDir, int keep);

private:
    void StartFlush(bool wait);

    std::string path;
    std::mutex lock;
    std::vector<uint8_t> buffer;
    bool dirty = false;
    std::chrono::steady_clock::time_point lastWrite;
};

}
