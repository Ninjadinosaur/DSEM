// JNI entry points for com.micah.ds13r.emu.NativeBridge.

#include "EmuSession.h"
#include "LogBuffer.h"

#include <android/native_window_jni.h>
#include <jni.h>
#include <string>
#include <vector>

using namespace ds13r;

namespace
{
JavaVM* gVm = nullptr;

std::string ToString(JNIEnv* env, jstring s)
{
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}

EmuSession& S()
{
    return EmuSession::Get();
}

// NewStringUTF only accepts "modified UTF-8" and aborts on e.g. 4-byte sequences (emoji in a
// file name), so strings go through Java's own UTF-8 decoder instead.
jstring NewJavaString(JNIEnv* env, const std::string& s)
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

#define JNI_FN(ret, name) extern "C" JNIEXPORT ret JNICALL Java_com_micah_ds13r_emu_NativeBridge_##name

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*)
{
    gVm = vm;
    return JNI_VERSION_1_6;
}

JNI_FN(void, nativeInit)(JNIEnv* env, jclass, jobject activity, jstring filesDir, jobject callbacks)
{
    HostCallbacks cb;
    cb.vm = gVm;
    cb.bridge = env->NewGlobalRef(callbacks);
    jclass cls = env->GetObjectClass(callbacks);
    cb.onMicRequest = env->GetMethodID(cls, "onMicRequest", "(Z)V");
    cb.onRumble = env->GetMethodID(cls, "onRumble", "(I)V");
    cb.onRtcOffset = env->GetMethodID(cls, "onRtcOffset", "(J)V");
    cb.onEmuStopped = env->GetMethodID(cls, "onEmuStopped", "(I)V");
    cb.onThermal = env->GetMethodID(cls, "onThermal", "(II)V");
    S().SetCallbacks(cb);
    S().Init(gVm, env->NewGlobalRef(activity), ToString(env, filesDir));
}

JNI_FN(void, nativeSetConfigInt)(JNIEnv* env, jclass, jstring key, jlong value)
{
    S().Config().SetInt(ToString(env, key), value);
}

JNI_FN(void, nativeSetConfigFloat)(JNIEnv* env, jclass, jstring key, jdouble value)
{
    S().Config().SetFloat(ToString(env, key), value);
}

JNI_FN(void, nativeSetConfigString)(JNIEnv* env, jclass, jstring key, jstring value)
{
    S().Config().SetString(ToString(env, key), ToString(env, value));
}

JNI_FN(jstring, nativeLoadGame)(JNIEnv* env, jclass, jint fd, jstring fileName, jstring gameKey)
{
    std::string err = S().LoadGame(fd, ToString(env, fileName), ToString(env, gameKey));
    return err.empty() ? nullptr : NewJavaString(env, err);
}

JNI_FN(jstring, nativeBootFirmware)(JNIEnv* env, jclass)
{
    std::string err = S().BootFirmware();
    return err.empty() ? nullptr : NewJavaString(env, err);
}

JNI_FN(jstring, nativeLoadGbaRom)(JNIEnv* env, jclass, jint fd, jstring fileName)
{
    std::string err = S().LoadGbaSlotRom(fd, ToString(env, fileName));
    return err.empty() ? nullptr : NewJavaString(env, err);
}

JNI_FN(void, nativeStart)(JNIEnv*, jclass)
{
    S().Start();
}

JNI_FN(void, nativeSetPaused)(JNIEnv*, jclass, jboolean paused)
{
    S().SetPaused(paused);
}

JNI_FN(void, nativeReset)(JNIEnv*, jclass)
{
    S().Reset();
}

JNI_FN(void, nativeStop)(JNIEnv*, jclass)
{
    S().Stop();
}

JNI_FN(void, nativeSetSurface)(JNIEnv* env, jclass, jobject surface)
{
    ANativeWindow* window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    S().SetSurface(window);
    // The presenter holds its own reference while it uses the window.
    if (window) ANativeWindow_release(window);
}

JNI_FN(void, nativeSetLayout)(JNIEnv* env, jclass, jfloatArray data)
{
    PresentLayout layout;
    jsize n = env->GetArrayLength(data);
    std::vector<float> v((size_t)n);
    env->GetFloatArrayRegion(data, 0, n, v.data());
    // 7 floats per quad: screen, x, y, w, h, rotation, opacity
    for (jsize i = 0; i + 7 <= n; i += 7)
    {
        ScreenQuad q;
        q.screen = (int)v[i];
        q.x = v[i + 1];
        q.y = v[i + 2];
        q.w = v[i + 3];
        q.h = v[i + 4];
        q.rotation = (int)v[i + 5];
        q.opacity = v[i + 6];
        layout.quads.push_back(q);
    }
    S().SetLayout(layout);
}

JNI_FN(void, nativeSetPresentSettings)(JNIEnv*, jclass, jint filter, jboolean colorCorrect, jint background)
{
    PresentSettings s;
    s.filter = static_cast<ScreenFilter>(filter);
    s.colorCorrection = colorCorrect;
    s.backgroundArgb = (uint32_t)background;
    S().SetPresentSettings(s);
}

JNI_FN(void, nativeSetKeys)(JNIEnv*, jclass, jint mask)
{
    S().SetKeys((uint32_t)mask);
}

JNI_FN(void, nativeSetTouch)(JNIEnv*, jclass, jboolean down, jint x, jint y)
{
    S().SetTouch(down, x, y);
}

JNI_FN(void, nativeSetLidClosed)(JNIEnv*, jclass, jboolean closed)
{
    S().SetLidClosed(closed);
}

JNI_FN(void, nativeSetBlow)(JNIEnv*, jclass, jboolean active)
{
    S().SetBlow(active);
}

JNI_FN(void, nativeSetMicAllowed)(JNIEnv*, jclass, jboolean allowed)
{
    S().SetMicAllowed(allowed);
}

JNI_FN(void, nativeSetGuitarKeys)(JNIEnv*, jclass, jint mask)
{
    S().SetGuitarKeys((uint32_t)mask);
}

JNI_FN(void, nativeSetMotion)(JNIEnv* env, jclass, jfloatArray values)
{
    float v[6] = {};
    env->GetFloatArrayRegion(values, 0, 6, v);
    S().SetMotion(v, v + 3);
}

JNI_FN(void, nativeSetSolar)(JNIEnv*, jclass, jint delta)
{
    S().SetSolarLevel(delta);
}

JNI_FN(void, nativeSetSpeedMode)(JNIEnv*, jclass, jint mode)
{
    S().SetSpeedMode(static_cast<SpeedMode>(mode));
}

JNI_FN(void, nativeFrameAdvance)(JNIEnv*, jclass)
{
    S().FrameAdvance();
}

JNI_FN(jboolean, nativeSaveState)(JNIEnv* env, jclass, jstring path, jstring thumbPath)
{
    return S().SaveState(ToString(env, path), ToString(env, thumbPath));
}

JNI_FN(jboolean, nativeLoadState)(JNIEnv* env, jclass, jstring path)
{
    return S().LoadState(ToString(env, path));
}

JNI_FN(jboolean, nativeUndoLoadState)(JNIEnv*, jclass)
{
    return S().UndoLoadState();
}

JNI_FN(jboolean, nativeUndoSaveState)(JNIEnv* env, jclass, jstring path)
{
    return S().UndoSaveState(ToString(env, path));
}

JNI_FN(void, nativeSetRewinding)(JNIEnv*, jclass, jboolean active)
{
    S().SetRewinding(active);
}

JNI_FN(jintArray, nativeScreenshot)(JNIEnv* env, jclass)
{
    std::vector<uint32_t> pixels;
    int w = 0, h = 0, screens = 0;
    if (!S().Screenshot(pixels, w, h, screens)) return nullptr;
    jintArray out = env->NewIntArray((jsize)(pixels.size() + 3));
    jint dims[3] = {w, h, screens};
    env->SetIntArrayRegion(out, 0, 3, dims);
    env->SetIntArrayRegion(out, 3, (jsize)pixels.size(), reinterpret_cast<const jint*>(pixels.data()));
    return out;
}

JNI_FN(void, nativeSetCheats)(JNIEnv* env, jclass, jobjectArray codes)
{
    std::vector<std::string> list;
    jsize n = codes ? env->GetArrayLength(codes) : 0;
    for (jsize i = 0; i < n; i++)
    {
        auto s = static_cast<jstring>(env->GetObjectArrayElement(codes, i));
        list.push_back(ToString(env, s));
        env->DeleteLocalRef(s);
    }
    S().SetCheats(list);
}

JNI_FN(void, nativeSetLight)(JNIEnv*, jclass, jfloat level)
{
    S().SetLight(level);
}

JNI_FN(jstring, nativeGetSystem)(JNIEnv* env, jclass)
{
    return NewJavaString(env, S().SystemName());
}

JNI_FN(void, nativeApplyLiveSettings)(JNIEnv*, jclass)
{
    S().ApplyLiveSettings();
}

JNI_FN(jfloatArray, nativeGetStats)(JNIEnv* env, jclass)
{
    EmuStats s = S().GetStats();
    float v[9] = {s.fps, s.speed, s.frameTimeMs, s.presentFps, s.thermalHeadroom, (float)s.thermalStatus,
                  (float)s.renderScale, s.rewindSeconds, s.rewindMemoryMb};
    jfloatArray out = env->NewFloatArray(9);
    env->SetFloatArrayRegion(out, 0, 9, v);
    return out;
}

JNI_FN(jobjectArray, nativeGetLog)(JNIEnv* env, jclass)
{
    std::vector<std::string> lines = LogSnapshot();
    jclass strCls = env->FindClass("java/lang/String");
    jobjectArray out = env->NewObjectArray((jsize)lines.size(), strCls, nullptr);
    for (size_t i = 0; i < lines.size(); i++)
    {
        jstring s = NewJavaString(env, lines[i]);
        env->SetObjectArrayElement(out, (jsize)i, s);
        env->DeleteLocalRef(s);
    }
    return out;
}

JNI_FN(void, nativeClearLog)(JNIEnv*, jclass)
{
    LogClear();
}
