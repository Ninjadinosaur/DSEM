// usrcheat.dat import (features.md §8), using melonDS's own database reader.

#include "LogBuffer.h"

#include "ARDatabaseDAT.h"
#include "CRC32.h"
#include "NDS_Header.h"

#include <cstring>
#include <jni.h>
#include <string>
#include <unistd.h>
#include <vector>

using namespace melonDS;

namespace
{
bool ReadAt(int fd, void* dst, size_t len, off_t off)
{
    size_t done = 0;
    while (done < len)
    {
        ssize_t n = pread(fd, static_cast<u8*>(dst) + done, len - done, off + (off_t)done);
        if (n <= 0) return false;
        done += (size_t)n;
    }
    return true;
}

// Same checksum melonDS computes for a loaded cart (CartCommon::Checksum), but reading only
// the parts of the ROM it covers instead of the whole file.
bool RomChecksum(int fd, const NDSHeader& header, u32& out)
{
    std::vector<u8> buf(0x40);
    if (!ReadAt(fd, buf.data(), 0x40, 0)) return false;
    u32 crc = CRC32(buf.data(), 0x40);

    auto add = [&](u32 offset, u32 size) {
        if (size == 0 || size > 0x1000000) return size == 0;
        buf.resize(size);
        if (!ReadAt(fd, buf.data(), size, offset)) return false;
        crc = CRC32(buf.data(), (int)size, crc);
        return true;
    };
    if (!add(header.ARM9ROMOffset, header.ARM9Size) || !add(header.ARM7ROMOffset, header.ARM7Size)) return false;
    if (header.IsDSi())
    {
        if (!add(header.DSiARM9iROMOffset, header.DSiARM9iSize) || !add(header.DSiARM7iROMOffset, header.DSiARM7iSize)) return false;
    }
    out = crc;
    return true;
}

void Flatten(const ARCodeCat& cat, const std::string& path, std::vector<std::string>& out)
{
    for (const ARCodeItem& item : cat.Children)
    {
        if (const ARCode* code = std::get_if<ARCode>(&item))
        {
            std::string text;
            for (size_t i = 0; i + 1 < code->Code.size(); i += 2)
            {
                char line[32];
                snprintf(line, sizeof(line), "%08X %08X\n", code->Code[i], code->Code[i + 1]);
                text += line;
            }
            if (!text.empty()) text.pop_back();
            out.insert(out.end(), {"CODE", path, code->Name, code->Description, text, cat.OnlyOneCodeEnabled ? "1" : "0"});
        }
        else if (const ARCodeCat* sub = std::get_if<ARCodeCat>(&item))
        {
            Flatten(*sub, path.empty() ? sub->Name : path + " / " + sub->Name, out);
        }
    }
}

jstring NewUtf8(JNIEnv* env, const std::string& s)
{
    jbyteArray bytes = env->NewByteArray((jsize)s.size());
    env->SetByteArrayRegion(bytes, 0, (jsize)s.size(), reinterpret_cast<const jbyte*>(s.data()));
    jclass strCls = env->FindClass("java/lang/String");
    jmethodID ctor = env->GetMethodID(strCls, "<init>", "([BLjava/lang/String;)V");
    jstring charset = env->NewStringUTF("UTF-8");
    auto out = static_cast<jstring>(env->NewObject(strCls, ctor, bytes, charset));
    env->DeleteLocalRef(bytes);
    env->DeleteLocalRef(charset);
    env->DeleteLocalRef(strCls);
    return out;
}
}

// Returns records of 6 strings each:
//   ["DB", name, "", "", "", ""]
//   ["ENTRY", entry name, "1" if the checksum matches this exact ROM, "", "", ""]
//   ["CODE", folder path, name, description, code text, "1" if only one code in the folder may be on]
// or null if the database can't be read or has nothing for this game.
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_micah_ds13r_emu_NativeBridge_nativeCheatDbLookup(JNIEnv* env, jclass, jstring dbPath, jint romFd)
{
    const char* p = env->GetStringUTFChars(dbPath, nullptr);
    std::string path = p ? p : "";
    if (p) env->ReleaseStringUTFChars(dbPath, p);

    NDSHeader header {};
    if (!ReadAt(romFd, &header, sizeof(header), 0))
    {
        LOGE("Cheat import: could not read the ROM header");
        return nullptr;
    }
    u32 checksum = 0;
    bool haveChecksum = RomChecksum(romFd, header, checksum);

    ARDatabaseDAT db(path);
    if (db.Error)
    {
        LOGE("Cheat import: %s is not a valid usrcheat.dat", path.c_str());
        return nullptr;
    }

    u32 code = header.GameCodeAsU32();
    ARDatabaseEntryList entries = db.GetEntriesByGameCode(code);
    if (entries.empty()) return nullptr;

    std::vector<std::string> out {"DB", db.GetDBName(), "", "", "", ""};
    for (const ARDatabaseEntry& e : entries)
    {
        bool match = haveChecksum && e.Checksum == checksum;
        out.insert(out.end(), {"ENTRY", e.Name, match ? "1" : "0", "", "", ""});
        Flatten(e.RootCat, "", out);
    }

    jclass strCls = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray((jsize)out.size(), strCls, nullptr);
    for (size_t i = 0; i < out.size(); i++)
    {
        jstring s = NewUtf8(env, out[i]);
        env->SetObjectArrayElement(arr, (jsize)i, s);
        env->DeleteLocalRef(s);
    }
    LOGI("Cheat import: %zu entries for %.4s (checksum %08X)", entries.size(), header.GameCode, checksum);
    return arr;
}
