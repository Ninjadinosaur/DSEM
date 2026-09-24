#include "DsCore.h"
#include "vk/GPU_Vulkan.h"
#include "ConfigStore.h"
#include "GLPresenter.h"
#include "IoWorker.h"
#include "LogBuffer.h"

#include "Args.h"
#include "DSi.h"
#include "FreeBIOS.h"
#include "GBACart.h"
#include "GPU.h"
#include "GPU_OpenGL.h"
#include "GPU_Soft.h"
#include "NDS.h"
#include "NDSCart.h"
#include "Platform.h"
#include "RTC.h"
#include "SPI.h"
#include "SPI_Firmware.h"
#include "SPU.h"
#include "Savestate.h"

#include <algorithm>
#include <cstring>
#include <ctime>

using namespace melonDS;

namespace ds13r
{

namespace
{
template <typename Image>
std::unique_ptr<Image> LoadBiosFile(const std::string& path)
{
    std::vector<unsigned char> data;
    if (path.empty() || !ReadWholeFile(path, data) || data.size() < sizeof(Image)) return nullptr;
    auto img = std::make_unique<Image>();
    memcpy(img->data(), data.data(), img->size());
    return img;
}

constexpr size_t kWfcSettingsSize = 3 * (sizeof(Firmware::WifiAccessPoint) + sizeof(Firmware::ExtendedWifiAccessPoint));

// Nickname and message arrive as UTF-16 code units in hex, avoiding JNI's modified UTF-8.
std::u16string DecodeUtf16Hex(const std::string& hex)
{
    std::u16string out;
    for (size_t i = 0; i + 4 <= hex.size(); i += 4) out += (char16_t)std::stoi(hex.substr(i, 4), nullptr, 16);
    return out;
}

// The DS user profile (features.md §1): nickname, message, language, colour, birthday.
void ApplyFirmwareProfile(Firmware& fw, const ConfigStore& cfg)
{
    auto& data = fw.GetEffectiveUserData();

    std::u16string name = DecodeUtf16Hex(cfg.GetString("fw.nickname16", ""));
    if (!name.empty())
    {
        size_t n = std::min<size_t>(name.size(), 10);
        memset(data.Nickname, 0, sizeof(data.Nickname));
        memcpy(data.Nickname, name.data(), n * sizeof(char16_t));
        data.NameLength = (u16)n;
    }

    std::u16string msg = DecodeUtf16Hex(cfg.GetString("fw.message16", ""));
    if (!msg.empty())
    {
        size_t n = std::min<size_t>(msg.size(), 26);
        memset(data.Message, 0, sizeof(data.Message));
        memcpy(data.Message, msg.data(), n * sizeof(char16_t));
        data.MessageLength = (u16)n;
    }

    int lang = (int)cfg.GetInt("fw.language", -1);
    if (lang >= 0 && lang <= 7)
    {
        auto language = static_cast<Firmware::Language>(lang);
        bool extlang = language >= Firmware::Language::Chinese;
        data.Settings &= ~Firmware::Language::Reserved;
        data.Settings |= extlang ? Firmware::Language::English : language;
        data.ExtendedSettings.ExtendedLanguage = language;
        auto& header = fw.GetHeader();
        if (extlang && !(header.ConsoleType & 0x40))
        {
            header.ConsoleType = header.ConsoleType == 0xFF ? 0x43 : (header.ConsoleType | 0x43);
            data.ExtendedSettings.Unknown0 = 0x01;
            data.ExtendedSettings.SupportedLanguageMask = 0x7F;
        }
    }

    int color = (int)cfg.GetInt("fw.color", -1);
    if (color >= 0 && color < 16) data.FavoriteColor = (u8)color;
    int month = (int)cfg.GetInt("fw.birthMonth", 0);
    int day = (int)cfg.GetInt("fw.birthDay", 0);
    if (month >= 1 && month <= 12) data.BirthdayMonth = (u8)month;
    if (day >= 1 && day <= 31) data.BirthdayDay = (u8)day;

    fw.UpdateChecksums();
}

bool ParseArCode(const std::string& text, std::vector<u32>& out)
{
    out.clear();
    std::string word;
    auto flush = [&]() {
        if (word.empty()) return true;
        if (word.size() != 8) return false;
        out.push_back((u32)strtoul(word.c_str(), nullptr, 16));
        word.clear();
        return true;
    };
    for (char c : text)
    {
        if (isxdigit((unsigned char)c))
            word += c;
        else if (!flush())
            return false;
    }
    return flush() && !out.empty() && out.size() % 2 == 0;
}
}

DsCore::DsCore(CoreHost& h) : host(h)
{
}

DsCore::~DsCore()
{
    Shutdown();
}

bool DsCore::CreateConsole(std::string& error)
{
    ConfigStore& config = host.Config();
    const std::string& filesDir = host.FilesDir();
    consoleType = (int)config.GetInt("emu.consoleType", 0);
    bool externalBios = config.GetBool("emu.externalBios", false);

    NDSArgs args;
    if (externalBios)
    {
        auto arm9 = LoadBiosFile<ARM9BIOSImage>(config.GetString("bios.arm9", ""));
        auto arm7 = LoadBiosFile<ARM7BIOSImage>(config.GetString("bios.arm7", ""));
        if (!arm9 || !arm7)
        {
            error = "The BIOS files set in Settings could not be read. Turn off \"Use real BIOS\" or pick the files again.";
            return false;
        }
        args.ARM9BIOS = std::move(arm9);
        args.ARM7BIOS = std::move(arm7);
    }

    // Firmware: a real dump if configured, otherwise melonDS generates one.
    std::string fwPath = config.GetString(consoleType == 1 ? "bios.dsiFirmware" : "bios.firmware", "");
    std::vector<unsigned char> fwData;
    if (externalBios && !fwPath.empty() && ReadWholeFile(fwPath, fwData) && !fwData.empty())
    {
        args.Firmware = Firmware(fwData.data(), (u32)fwData.size());
        if (!args.Firmware.Buffer())
        {
            error = "The firmware file set in Settings is not valid.";
            return false;
        }
        if (config.GetBool("fw.override", true)) ApplyFirmwareProfile(args.Firmware, config);
    }
    else
    {
        args.Firmware = Firmware(consoleType);
        // Keep Wi-Fi settings (e.g. a WFC server) across sessions.
        std::vector<unsigned char> wfc;
        if (ReadWholeFile(filesDir + "/wfcsettings.bin", wfc) && wfc.size() >= kWfcSettingsSize)
            memcpy(args.Firmware.GetExtendedAccessPointPosition(), wfc.data(), kWfcSettingsSize);
        ApplyFirmwareProfile(args.Firmware, config);
    }

    if (config.GetBool("jit.enabled", true))
    {
        JITArgs jit;
        jit.MaxBlockSize = (unsigned)config.GetInt("jit.maxBlockSize", 32);
        jit.LiteralOptimizations = config.GetBool("jit.literal", true);
        jit.BranchOptimizations = config.GetBool("jit.branch", true);
        jit.FastMemory = config.GetBool("jit.fastmem", true);
        args.JIT = jit;
    }
    else
    {
        args.JIT = std::nullopt;
    }

    args.Interpolation = static_cast<AudioInterpolation>(config.GetInt("audio.interpolation", 0));
    args.BitDepth = static_cast<AudioBitDepth>(config.GetInt("audio.bitDepth", 0));
    args.OutputSampleRate = 48000;

    if (config.GetBool("debug.gdb", false))
    {
        GDBArgs gdb;
        gdb.PortARM9 = (u16)config.GetInt("debug.gdbPort9", 3333);
        gdb.PortARM7 = (u16)config.GetInt("debug.gdbPort7", 3334);
        gdb.ARM9BreakOnStartup = config.GetBool("debug.gdbBreak", false);
        gdb.ARM7BreakOnStartup = false;
        args.GDB = gdb;
    }

    nds.reset();

    if (consoleType == 1)
    {
        DSiArgs dsi {std::move(args), nullptr, nullptr, std::nullopt, std::nullopt, false};
        auto arm9i = LoadBiosFile<DSiBIOSImage>(config.GetString("bios.dsiArm9", ""));
        auto arm7i = LoadBiosFile<DSiBIOSImage>(config.GetString("bios.dsiArm7", ""));
        if (!arm9i || !arm7i)
        {
            error = "DSi mode needs the DSi BIOS files. Add them in Settings > DSi.";
            return false;
        }
        dsi.ARM9iBIOS = std::move(arm9i);
        dsi.ARM7iBIOS = std::move(arm7i);

        std::string nandPath = config.GetString("bios.dsiNand", "");
        if (!nandPath.empty())
        {
            Platform::FileHandle* nandFile = Platform::OpenFile(nandPath, Platform::FileMode::ReadWriteExisting);
            if (nandFile)
            {
                dsi.NANDImage = DSi_NAND::NANDImage(nandFile, &dsi.ARM7iBIOS->data()[0x8308]);
                if (!*dsi.NANDImage)
                {
                    error = "The DSi NAND file could not be opened. Check it was dumped with the matching BIOS.";
                    return false;
                }
            }
        }
        if (!dsi.NANDImage)
        {
            error = "DSi mode needs a DSi NAND dump. Add it in Settings > DSi.";
            return false;
        }

        if (config.GetBool("dsi.sdEnabled", true))
        {
            FATStorageArgs sd {filesDir + "/sd/dsi_sd.img", (u64)config.GetInt("dsi.sdSizeMB", 0) * 1024 * 1024,
                               config.GetBool("dsi.sdReadOnly", false), std::nullopt};
            dsi.DSiSDCard = FATStorage(sd);
        }
        dsi.DSPHLE = config.GetBool("dsi.dspHle", true);
        nds = std::make_unique<DSi>(std::move(dsi), this);
    }
    else
    {
        nds = std::make_unique<NDS>(std::move(args), this);
    }

    nds->Reset();
    activeRenderer = 0; // a new console starts on the software renderer
    return true;
}

void DsCore::ApplySettings()
{
    if (!nds) return;
    ConfigStore& config = host.Config();

    // 0 = software, 1 = OpenGL ES, 2 = OpenGL ES compute. The GL renderers share the presenter's
    // context, which is current on the emulation thread.
    int want = (int)config.GetInt("video.renderer", 1);
    if (want != activeRenderer)
    {
        if (want == 3 && !host.Vulkan())
            want = 1; // no Vulkan: fall back to OpenGL ES
        if (want == 0)
        {
            nds->SetRenderer(std::make_unique<SoftRenderer>(*nds));
        }
        else if (want == 3)
        {
            // Vulkan: 2D layers, sprites, compositing and capture on the GPU (3D: phase 3).
            nds->SetRenderer(std::make_unique<VulkanRenderer>(*nds, *host.Vulkan()));
        }
        else
        {
            if (host.Presenter()) host.Presenter()->MakeCurrent();
            nds->SetRenderer(std::make_unique<GLRenderer>(*nds, want == 2));
        }
        activeRenderer = want;
        LOGI("DS renderer: %s", want == 0 ? "software" : want == 1 ? "OpenGL ES" : want == 2 ? "OpenGL ES compute"
                                                                           : "Vulkan");
    }

    RendererSettings rs {};
    rs.ScaleFactor = RenderScale();
    rs.Threaded = config.GetBool("video.threaded3D", true);
    rs.HiresCoordinates = config.GetBool("video.hiresCoords", true);
    rs.BetterPolygons = config.GetBool("video.betterPolygons", false);
    nds->GetRenderer().SetRenderSettings(rs);

    nds->SPU.SetInterpolation(static_cast<AudioInterpolation>(config.GetInt("audio.interpolation", 0)));
    nds->SPU.SetDegrade10Bit(static_cast<AudioBitDepth>(config.GetInt("audio.bitDepth", 0)));
}

int DsCore::RenderScale() const
{
    if (activeRenderer == 0) return 1;
    const ConfigStore& config = host.Config();
    int scale = (int)config.GetInt("video.scale", 4);
    int cap = (int)config.GetInt("video.scaleCap", 0); // set by the thermal policy
    if (cap > 0) scale = std::min(scale, cap);
    return std::max(1, scale);
}

void DsCore::ApplyRtc()
{
    if (!nds) return;
    time_t now = time(nullptr) + (time_t)host.Config().GetInt("rtc.offset", 0);
    tm t;
    localtime_r(&now, &t);
    nds->RTC.SetDateTime(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
}

void DsCore::SyncClock()
{
    if (host.Config().GetBool("rtc.sync", false)) ApplyRtc();
}

std::string DsCore::LoadGame(std::unique_ptr<uint8_t[]> rom, uint32_t romLen, const std::string& fileName, const std::string& key)
{
    ConfigStore& config = host.Config();
    const std::string& filesDir = host.FilesDir();
    std::string error;

    FlushSaves();
    ndsSave.reset();
    gbaSave.reset();

    gameKey = key;
    romName = fileName;

    std::string savePath = filesDir + "/saves/" + gameKey + ".sav";
    SaveFile::BackupExisting(savePath, filesDir + "/backups/" + gameKey, (int)config.GetInt("saves.backupCount", 10));

    std::vector<unsigned char> sav;
    std::unique_ptr<u8[]> sram;
    u32 sramLen = 0;
    if (ReadWholeFile(savePath, sav) && !sav.empty())
    {
        sramLen = (u32)sav.size();
        sram = std::make_unique<u8[]>(sramLen);
        memcpy(sram.get(), sav.data(), sramLen);
    }

    NDSCart::NDSCartArgs cartArgs;
    if (config.GetBool("dldi.enabled", true))
    {
        cartArgs.SDCard = FATStorageArgs {filesDir + "/sd/dldi.img", (u64)config.GetInt("dldi.sizeMB", 0) * 1024 * 1024,
                                          config.GetBool("dldi.readOnly", false), std::nullopt};
    }
    cartArgs.SRAM = std::move(sram);
    cartArgs.SRAMLength = sramLen;

    auto cart = NDSCart::ParseROM(std::move(rom), romLen, this, std::move(cartArgs));
    if (!cart) return "This file is not a valid DS game.";

    if (!CreateConsole(error)) return error;

    nds->SetNDSCart(std::move(cart));
    ApplySettings();
    nds->Reset();

    if (config.GetBool("emu.directBoot", true) || nds->NeedsDirectBoot())
        nds->SetupDirectBoot(romName);

    if (consoleType == 0) nds->SPI.GetPowerMan()->SetBatteryLevelOkay(true);
    ApplyRtc();

    ndsSave = std::make_unique<SaveFile>(savePath);
    // RunFrame does nothing until the console is started.
    nds->Start();
    LOGI("DS: loaded %s, %u bytes, save %u bytes", romName.c_str(), romLen, sramLen);
    return {};
}

std::string DsCore::BootFirmware()
{
    std::string error;
    if (!CreateConsole(error)) return error;
    if (nds->NeedsDirectBoot())
    {
        nds.reset();
        return "Booting to the DS menu needs real BIOS and firmware dumps (Settings > BIOS & firmware).";
    }
    gameKey = "firmware";
    romName.clear();
    ApplySettings();
    nds->Reset();
    ApplyRtc();
    nds->Start();
    return {};
}

std::string DsCore::InsertGbaCart(std::unique_ptr<uint8_t[]> rom, uint32_t len, const std::string& fileName)
{
    if (!nds || consoleType != 0) return "GBA cartridges only work in DS mode.";
    std::string savePath = host.FilesDir() + "/saves/" + fileName + ".gba.sav";
    std::vector<unsigned char> sav;
    std::unique_ptr<u8[]> sram;
    u32 sramLen = 0;
    if (ReadWholeFile(savePath, sav) && !sav.empty())
    {
        sramLen = (u32)sav.size();
        sram = std::make_unique<u8[]>(sramLen);
        memcpy(sram.get(), sav.data(), sramLen);
    }
    auto cart = GBACart::ParseROM(std::move(rom), len, std::move(sram), sramLen, this);
    if (!cart) return "This file is not a valid GBA game.";
    nds->SetGBACart(std::move(cart));
    gbaSave = std::make_unique<SaveFile>(savePath);
    LOGI("DS GBA slot: inserted %s", fileName.c_str());
    return {};
}

void DsCore::RunFrame(const CoreInput& input)
{
    if (!nds) return;
    if (input.lidRequest >= 0) nds->SetLidClosed(input.lidRequest == 1);
    // melonDS keys are active-low.
    nds->SetKeyMask(~input.keys & 0xFFF);
    if (input.touching)
        nds->TouchScreen((u16)input.touchX, (u16)input.touchY);
    else
        nds->ReleaseScreen();

    if (nds->GetRenderer().NeedsShaderCompile())
    {
        int current = 0, count = 0;
        nds->GetRenderer().ShaderCompileStep(current, count);
        LOGI("Compiling GPU shaders %d/%d", current, count);
        return;
    }

    nds->RunFrame();
}

bool DsCore::GetFrame(FrameInfo& out)
{
    if (!nds) return false;
    void* top = nullptr;
    void* bottom = nullptr;
    out.screenCount = 2;
    if (nds->GPU.GetFramebuffers(&top, &bottom))
    {
        if (!top || !bottom) return false;
        out.hardware = false;
        out.screens[0] = top;
        out.screens[1] = bottom;
        out.width = 256;
        out.height = 192;
        out.bgra = true;
        return true;
    }
    if (!top) return false;
    // Hardware renderer: a 2-layer texture array (top, bottom) at the upscaled size.
    int scale = RenderScale();
    out.hardware = true;
    if (activeRenderer == 3)
    {
        out.vkTexture = top;
        out.width = 256 * scale;
        out.height = 192 * scale;
        return true;
    }
    out.texture = *static_cast<GLuint*>(top);
    out.width = 256 * scale;
    out.height = 192 * scale;
    return true;
}

void DsCore::Reset()
{
    if (!nds) return;
    nds->Reset();
    if (!romName.empty() && (host.Config().GetBool("emu.directBoot", true) || nds->NeedsDirectBoot()))
        nds->SetupDirectBoot(romName);
    ApplyRtc();
    nds->Start();
}

void DsCore::Shutdown()
{
    FlushSaves();
    if (nds) nds->Stop();
    nds.reset();
    ndsSave.reset();
    gbaSave.reset();
}

void DsCore::CheckFlush()
{
    if (ndsSave) ndsSave->CheckFlush();
    if (gbaSave) gbaSave->CheckFlush();
}

void DsCore::FlushSaves()
{
    if (ndsSave) ndsSave->FlushNow();
    if (gbaSave) gbaSave->FlushNow();
}

bool DsCore::SaveState(std::vector<uint8_t>& out)
{
    if (!nds) return false;
    Savestate state;
    if (state.Error) return false;
    nds->DoSavestate(&state);
    state.Finish();
    if (state.Error) return false;
    const uint8_t* p = static_cast<const uint8_t*>(state.Buffer());
    out.assign(p, p + state.Length());
    return true;
}

bool DsCore::LoadState(const uint8_t* data, size_t len)
{
    if (!nds) return false;
    // In load mode melonDS only reads the buffer; its constructor just isn't const-correct.
    Savestate state(const_cast<uint8_t*>(data), (u32)len, false);
    return !state.Error && nds->DoSavestate(&state) && !state.Error;
}

size_t DsCore::CaptureState(uint8_t* buffer, size_t capacity)
{
    if (!nds) return 0;
    Savestate state(buffer, (u32)capacity, true);
    nds->DoSavestate(&state);
    state.Finish();
    return state.Error ? 0 : state.Length();
}

size_t DsCore::MaxStateSize() const
{
    return Savestate::DEFAULT_SIZE;
}

void DsCore::SetCheats(const std::vector<std::string>& codes)
{
    if (!nds) return;
    nds->AREngine.Cheats.clear();
    for (const std::string& text : codes)
    {
        ARCode code {};
        if (!ParseArCode(text, code.Code))
        {
            LOGW("DS cheats: skipped an invalid Action Replay code");
            continue;
        }
        code.Enabled = true;
        nds->AREngine.Cheats.push_back(std::move(code));
    }
    LOGI("DS cheats: %zu active", nds->AREngine.Cheats.size());
}

void DsCore::SetAudioSkew(double skew)
{
    if (nds) nds->SPU.SetOutputSkew(skew);
}

int DsCore::AudioFill()
{
    return nds ? nds->SPU.GetOutputSize() : 0;
}

int DsCore::ReadAudio(int16_t* out, int frames)
{
    // Called on the audio thread; EmuSession guarantees the core outlives any read.
    return nds ? nds->SPU.ReadOutput(out, frames) : 0;
}

void DsCore::SetSolarLevel(int delta)
{
    if (!nds) return;
    int input = delta > 0 ? GBACart::Input_SolarSensorUp : GBACart::Input_SolarSensorDown;
    for (int i = 0; i < std::abs(delta); i++) nds->GBACartSlot.SetInput(input, true);
}

void DsCore::OnNdsSaveWrite(const uint8_t* data, uint32_t len, uint32_t off, uint32_t wlen)
{
    if (ndsSave) ndsSave->OnWrite(data, len, off, wlen);
}

void DsCore::OnGbaSaveWrite(const uint8_t* data, uint32_t len, uint32_t off, uint32_t wlen)
{
    if (gbaSave) gbaSave->OnWrite(data, len, off, wlen);
}

void DsCore::OnFirmwareWrite()
{
    // With generated firmware only the Wi-Fi settings matter (e.g. a custom WFC DNS server).
    if (!nds) return;
    Firmware& fw = nds->GetFirmware();
    std::vector<uint8_t> wfc(kWfcSettingsSize);
    memcpy(wfc.data(), fw.GetExtendedAccessPointPosition(), kWfcSettingsSize);
    std::string path = host.FilesDir() + "/wfcsettings.bin";
    IoWorker::Get().Post([path, wfc] { WriteFileAtomic(path, wfc.data(), wfc.size()); });
}

void DsCore::OnDateTimeWrite(int year, int month, int day, int hour, int minute, int second)
{
    tm t {};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_sec = second;
    t.tm_isdst = -1;
    int64_t offset = (int64_t)mktime(&t) - (int64_t)time(nullptr);
    host.Config().SetInt("rtc.offset", offset);
    host.OnRtcOffset(offset);
}

}
