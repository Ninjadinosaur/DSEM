# JNI: native code looks these up by name.
-keep class com.micah.ds13r.emu.NativeBridge { *; }
-keep class * implements com.micah.ds13r.emu.NativeBridge$Callbacks { *; }
-keepclasseswithmembernames class * { native <methods>; }
