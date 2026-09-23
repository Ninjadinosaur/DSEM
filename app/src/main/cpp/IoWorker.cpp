#include "IoWorker.h"
#include "CpuTopology.h"
#include "LogBuffer.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ds13r
{

IoWorker& IoWorker::Get()
{
    static IoWorker worker;
    return worker;
}

IoWorker::IoWorker()
{
    thread = std::thread([this] { Loop(); });
    thread.detach();
}

void IoWorker::Loop()
{
    pthread_setname_np(pthread_self(), "ds13r-io");
    PinCurrentThread(CoreClass::Little);

    for (;;)
    {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> l(lock);
            cv.wait(l, [this] { return !jobs.empty(); });
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        job();
    }
}

void IoWorker::Post(std::function<void()> job)
{
    {
        std::lock_guard<std::mutex> l(lock);
        jobs.push_back(std::move(job));
    }
    cv.notify_one();
}

void IoWorker::Run(std::function<void()> job)
{
    std::promise<void> done;
    auto fut = done.get_future();
    Post([&] {
        job();
        done.set_value();
    });
    fut.wait();
}

void IoWorker::Drain()
{
    Run([] {});
}

bool MakeDirs(const std::string& path)
{
    std::string cur;
    for (size_t i = 0; i < path.size(); i++)
    {
        cur += path[i];
        bool atSep = path[i] == '/' && i > 0;
        bool atEnd = i == path.size() - 1;
        if (atSep || atEnd)
        {
            if (mkdir(cur.c_str(), 0770) != 0 && errno != EEXIST)
            {
                LOGE("mkdir %s failed: %s", cur.c_str(), strerror(errno));
                return false;
            }
        }
    }
    return true;
}

bool FileExistsAt(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool WriteFileAtomic(const std::string& path, const void* data, size_t len)
{
    std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
    if (fd < 0)
    {
        LOGE("open %s failed: %s", tmp.c_str(), strerror(errno));
        return false;
    }

    const unsigned char* p = static_cast<const unsigned char*>(data);
    size_t left = len;
    while (left > 0)
    {
        ssize_t n = write(fd, p, left);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            LOGE("write %s failed: %s", tmp.c_str(), strerror(errno));
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        p += n;
        left -= (size_t)n;
    }

    fsync(fd);
    close(fd);

    if (rename(tmp.c_str(), path.c_str()) != 0)
    {
        LOGE("rename %s failed: %s", path.c_str(), strerror(errno));
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool ReadWholeFile(const std::string& path, std::vector<unsigned char>& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0)
    {
        fclose(f);
        return false;
    }
    out.resize((size_t)len);
    bool ok = len == 0 || fread(out.data(), (size_t)len, 1, f) == 1;
    fclose(f);
    return ok;
}

}
