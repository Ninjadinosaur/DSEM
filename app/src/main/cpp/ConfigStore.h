#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

// Settings pushed down from the Kotlin side (which owns persistence, per-game overrides
// and the settings UI). Keys are plain strings such as "jit.enabled" or "video.scale".
namespace ds13r
{

class ConfigStore
{
public:
    void SetInt(const std::string& key, int64_t v)
    {
        std::lock_guard<std::mutex> l(lock);
        ints[key] = v;
    }
    void SetFloat(const std::string& key, double v)
    {
        std::lock_guard<std::mutex> l(lock);
        floats[key] = v;
    }
    void SetString(const std::string& key, const std::string& v)
    {
        std::lock_guard<std::mutex> l(lock);
        strings[key] = v;
    }

    int64_t GetInt(const std::string& key, int64_t def) const
    {
        std::lock_guard<std::mutex> l(lock);
        auto it = ints.find(key);
        return it == ints.end() ? def : it->second;
    }
    bool GetBool(const std::string& key, bool def) const { return GetInt(key, def ? 1 : 0) != 0; }
    double GetFloat(const std::string& key, double def) const
    {
        std::lock_guard<std::mutex> l(lock);
        auto it = floats.find(key);
        return it == floats.end() ? def : it->second;
    }
    std::string GetString(const std::string& key, const std::string& def) const
    {
        std::lock_guard<std::mutex> l(lock);
        auto it = strings.find(key);
        return it == strings.end() ? def : it->second;
    }

private:
    mutable std::mutex lock;
    std::map<std::string, int64_t> ints;
    std::map<std::string, double> floats;
    std::map<std::string, std::string> strings;
};

}
