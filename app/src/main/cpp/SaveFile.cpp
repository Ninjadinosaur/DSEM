#include "SaveFile.h"
#include "IoWorker.h"
#include "LogBuffer.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <memory>
#include <unistd.h>

namespace ds13r
{

namespace
{
// Games often write save data in many small bursts; wait this long after the
// last write before touching the disk.
constexpr auto kFlushDelay = std::chrono::milliseconds(1000);
}

SaveFile::SaveFile(std::string p) : path(std::move(p))
{
}

SaveFile::~SaveFile()
{
    FlushNow();
}

void SaveFile::OnWrite(const uint8_t* data, uint32_t len, uint32_t offset, uint32_t writelen)
{
    std::lock_guard<std::mutex> l(lock);
    if (buffer.size() != len || writelen == 0 || offset >= len)
    {
        buffer.assign(data, data + len);
    }
    else
    {
        uint32_t n = std::min(writelen, len - offset);
        memcpy(buffer.data() + offset, data + offset, n);
        // Writes can wrap around the end of the save memory.
        if (n < writelen) memcpy(buffer.data(), data, std::min(writelen - n, len));
    }
    dirty = true;
    lastWrite = std::chrono::steady_clock::now();
}

void SaveFile::CheckFlush()
{
    bool due;
    {
        std::lock_guard<std::mutex> l(lock);
        due = dirty && (std::chrono::steady_clock::now() - lastWrite) >= kFlushDelay;
    }
    if (due) StartFlush(false);
}

void SaveFile::FlushNow()
{
    StartFlush(true);
}

void SaveFile::StartFlush(bool wait)
{
    std::shared_ptr<std::vector<uint8_t>> snapshot;
    {
        std::lock_guard<std::mutex> l(lock);
        if (dirty)
        {
            snapshot = std::make_shared<std::vector<uint8_t>>(buffer);
            dirty = false;
        }
    }

    if (snapshot)
    {
        std::string p = path;
        auto job = [p, snapshot] {
            if (WriteFileAtomic(p, snapshot->data(), snapshot->size()))
                LOGI("Saved %zu bytes to %s", snapshot->size(), p.c_str());
        };
        if (wait)
            IoWorker::Get().Run(job);
        else
            IoWorker::Get().Post(job);
    }
    else if (wait)
    {
        // An earlier asynchronous flush may still be running.
        IoWorker::Get().Drain();
    }
}

void SaveFile::BackupExisting(const std::string& path, const std::string& backupDir, int keep)
{
    std::vector<unsigned char> data;
    if (!ReadWholeFile(path, data) || data.empty()) return;

    MakeDirs(backupDir);

    std::vector<std::string> names;
    if (DIR* d = opendir(backupDir.c_str()))
    {
        while (dirent* e = readdir(d))
        {
            if (e->d_name[0] != '.') names.emplace_back(e->d_name);
        }
        closedir(d);
    }
    std::sort(names.begin(), names.end());

    // Skip the backup if the newest one is identical.
    if (!names.empty())
    {
        std::vector<unsigned char> newest;
        if (ReadWholeFile(backupDir + "/" + names.back(), newest) && newest == data) return;
    }

    char stamp[32];
    time_t now = time(nullptr);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
    std::string name = std::string(stamp) + ".sav";
    WriteFileAtomic(backupDir + "/" + name, data.data(), data.size());
    names.push_back(name);

    while ((int)names.size() > keep)
    {
        unlink((backupDir + "/" + names.front()).c_str());
        names.erase(names.begin());
    }
}

}
