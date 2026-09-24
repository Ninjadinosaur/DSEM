plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
    alias(libs.plugins.kotlin.serialization)
}

android {
    namespace = "com.micah.ds13r"
    compileSdk = 37
    // NDK r28+ is required for 16 KB page-size aligned native libraries (features.md §14).
    ndkVersion = "30.0.16248370"

    defaultConfig {
        applicationId = "com.micah.ds13r"
        // The only target is the OnePlus 13R, which shipped with Android 15.
        minSdk = 35
        targetSdk = 37
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            // 64-bit ARM only: no 32-bit or x86 builds are needed for the 13R.
            abiFilters += "arm64-v8a"
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
                )
                cppFlags += listOf("-std=c++17")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "4.1.2"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            // Signed with the debug key so release builds can be installed on the phone directly.
            signingConfig = signingConfigs.getByName("debug")
        }
        debug {
            isJniDebuggable = true
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_21
        targetCompatibility = JavaVersion.VERSION_21
    }

    buildFeatures {
        compose = true
        prefab = true
        buildConfig = true
    }

    packaging {
        // Keep native libraries uncompressed and page-aligned in the APK (16 KB page support).
        jniLibs {
            useLegacyPackaging = false
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Nintendo 3DS engine: Azahar's libretro core, built as its own shared library with its own
// toolchain settings (C++20, static libc++) and loaded only when a 3DS game starts.
// ---------------------------------------------------------------------------------------------
val isWindows = System.getProperty("os.name").startsWith("Windows")
val exe = if (isWindows) ".exe" else ""
val sdkDir: File = androidComponents.sdkComponents.sdkDirectory.get().asFile
val ndkDir = File(sdkDir, "ndk/${android.ndkVersion}")
val cmakeExe = File(sdkDir, "cmake/4.1.2/bin/cmake$exe").absolutePath
val ninjaExe = File(sdkDir, "cmake/4.1.2/bin/ninja$exe").absolutePath
val azaharSrc = rootProject.file("third_party/azahar")
val azaharBuild = layout.buildDirectory.dir("azahar/build").get().asFile
val azaharJniLibs = layout.buildDirectory.dir("azahar/jniLibs").get().asFile
val azaharExports = file("src/main/azahar/exports.map")
val azaharPatches = file("src/main/azahar/patches")

// Azahar is a git submodule pinned to an upstream release; our few fixes live here as patches
// and are applied to the checkout once (a patch that already applies in reverse is skipped).
val patchAzahar = tasks.register("patchAzahar") {
    description = "Applies app/src/main/azahar/patches to the Azahar submodule."
    inputs.dir(azaharPatches)
    doLast {
        if (!File(azaharSrc, "CMakeLists.txt").exists()) {
            throw GradleException("third_party/azahar is empty: run `git submodule update --init --recursive`")
        }
        fun git(vararg args: String): Int =
            ProcessBuilder(listOf("git", "-C", azaharSrc.absolutePath) + args).inheritIO().start().waitFor()
        azaharPatches.listFiles { f -> f.extension == "patch" }!!.sorted().forEach { patch ->
            val alreadyApplied = ProcessBuilder("git", "-C", azaharSrc.absolutePath, "apply", "--reverse", "--check", patch.absolutePath)
                .redirectErrorStream(true).start().waitFor() == 0
            if (!alreadyApplied && git("apply", patch.absolutePath) != 0) {
                throw GradleException("Could not apply ${patch.name} to third_party/azahar")
            }
        }
    }
}

val configureAzahar = tasks.register<Exec>("configureAzahar") {
    description = "Configures the Azahar (3DS) libretro core for arm64 Android."
    dependsOn(patchAzahar)
    // Also re-configure when a patch changed: Azahar hashes its shader generator sources only at
    // configure time (GenerateSCMRev), and that hash is what makes old shader caches regenerate.
    onlyIf {
        val scmRev = File(azaharBuild, "src/common/scm_rev.cpp")
        val newestPatch = azaharPatches.listFiles { f -> f.extension == "patch" }?.maxOfOrNull { it.lastModified() } ?: 0L
        !File(azaharBuild, "build.ninja").exists() || !scmRev.exists() || newestPatch > scmRev.lastModified()
    }
    doFirst { azaharBuild.mkdirs() }
    commandLine(
        cmakeExe, "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=$ninjaExe",
        "-DCMAKE_TOOLCHAIN_FILE=${File(ndkDir, "build/cmake/android.toolchain.cmake").absolutePath}",
        "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-35", "-DANDROID_STL=c++_static",
        "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
        "-DCMAKE_BUILD_TYPE=Release", "-DENABLE_LIBRETRO=ON", "-DENABLE_TESTS=OFF",
        "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,--version-script=${azaharExports.absolutePath}",
        // Its third-party dependencies use old cmake_minimum_required values; not our code.
        "-Wno-dev", "-Wno-deprecated",
        "-S", azaharSrc.absolutePath, "-B", azaharBuild.absolutePath,
    )
}

val compileAzahar = tasks.register<Exec>("compileAzahar") {
    description = "Builds azahar_libretro.so (incremental)."
    dependsOn(configureAzahar)
    commandLine(cmakeExe, "--build", azaharBuild.absolutePath, "--target", "citra_libretro", "--config", "Release")
}

val buildAzahar = tasks.register<Exec>("buildAzahar") {
    description = "Strips the 3DS core (hundreds of MB of debug info) into jniLibs as libazahar_libretro.so."
    dependsOn(compileAzahar)
    val out = File(azaharJniLibs, "arm64-v8a")
    doFirst { out.mkdirs() }
    val strip = File(ndkDir, "toolchains/llvm/prebuilt/${if (isWindows) "windows-x86_64" else "linux-x86_64"}/bin/llvm-strip$exe")
    commandLine(
        strip.absolutePath, "--strip-unneeded",
        "-o", File(out, "libazahar_libretro.so").absolutePath,
        File(azaharBuild, "bin/Release/azahar_libretro.so").absolutePath,
    )
}

android.sourceSets.getByName("main").jniLibs.directories.add(azaharJniLibs.absolutePath)
tasks.matching { it.name.startsWith("merge") && it.name.endsWith("JniLibFolders") }.configureEach { dependsOn(buildAzahar) }

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.navigation.compose)
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.androidx.lifecycle.process)
    implementation(libs.androidx.documentfile)
    implementation(libs.androidx.datastore.preferences)
    implementation(libs.kotlinx.serialization.json)
    implementation(libs.kotlinx.coroutines.android)

    implementation(platform(libs.compose.bom))
    implementation(libs.compose.ui)
    implementation(libs.compose.ui.graphics)
    implementation(libs.compose.ui.tooling.preview)
    implementation(libs.compose.material3)
    implementation(libs.compose.material.icons.extended)
    debugImplementation(libs.compose.ui.tooling)

    implementation(libs.oboe)
    implementation(libs.games.frame.pacing)
}
