#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Background file I/O thread, pinned to the little (Cortex-A520) cores so saving
// never competes with emulation (features.md §13).
namespace ds13r
{

class IoWorker
{
public:
    static IoWorker& Get();

    void Post(std::function<void()> job);
    // Runs the job on the I/O thread and waits for it to finish.
    void Run(std::function<void()> job);
    // Waits until every job posted so far has finished.
    void Drain();

private:
    IoWorker();
    void Loop();

    std::mutex lock;
    std::condition_variable cv;
    std::deque<std::function<void()>> jobs;
    std::thread thread;
};

// Writes a file atomically: data goes to "<path>.tmp", is fsynced, then renamed over the target.
bool WriteFileAtomic(const std::string& path, const void* data, size_t len);
bool ReadWholeFile(const std::string& path, std::vector<unsigned char>& out);
bool MakeDirs(const std::string& path);
bool FileExistsAt(const std::string& path);

}
