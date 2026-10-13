plugins {
    id("com.android.application")
}

// Release-keystore signing, wired to environment variables rather than local.properties, exactly as
// apps/demos/android does it (the same four names): CI decodes a keystore secret to a runner-temp
// file and exports them before assembleRelease. Absent locally and on ordinary CI, where every build
// path degrades to the debug keystore. The app is sideloaded (planning/hearth-android.md, D14), so a
// release key only matters for update-signature continuity.
val releaseKeystorePath = System.getenv("ANDROID_KEYSTORE_PATH")
val releaseKeystorePassword = System.getenv("ANDROID_KEYSTORE_PASSWORD")
val releaseKeyAlias = System.getenv("ANDROID_KEY_ALIAS")
val releaseKeyPassword = System.getenv("ANDROID_KEY_PASSWORD")
val releaseSigningAvailable = !releaseKeystorePath.isNullOrBlank() &&
    !releaseKeystorePassword.isNullOrBlank() &&
    !releaseKeyAlias.isNullOrBlank() &&
    !releaseKeyPassword.isNullOrBlank()

android {
    namespace = "com.iclforge.hearth"
    // As the Shield app: 36 is the newest platform in the SDK repository, 34 the target actually
    // exercised, so the app runs as the Shield's own system image runs it.
    compileSdk = 36

    defaultConfig {
        // What the app is installed as, and so the name of its data directory and of its
        // instrumentation package (<id>.test). It follows the repository's account, as the macOS
        // bundle ids and the Linux desktop ids do, and is not the package of the code: the
        // namespace above stays com.iclforge.hearth, which the JNI names of the native code spell
        // (tools/checks/check_android_jni.py). No release has carried this app.
        applicationId = "com.iainchesworthlabs.hearth"
        // 26 (Oreo): notification channels and the foreground-service start the sink needs, and
        // the floor of AAudio for the pass-through output that comes after this skeleton. Android
        // TV hardware from 2017 on is above it.
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0-alpha.1"

        // The instrumented tests (src/androidTest): connectedDebugAndroidTest needs a runner declared
        // before Gradle will run anything under that source set.
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        externalNativeBuild {
            cmake {
                // c++_shared, as the Shield app: the libraries linked into the one shared object
                // are static, and a static STL would duplicate global state if anything else in
                // the process pulled libc++ in too.
                //
                // Everything vcpkg needs (the toolchain chain, the triplet, the "hearth" feature,
                // the repository's overlay ports) is src/main/cpp/CMakeLists.txt's, which is also
                // where it is explained; this file adds only what AGP must be told.
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    // The NDK ships no clang-scan-deps (see apps/demos/android/app/build.gradle.kts).
                    "-DCMAKE_CXX_SCAN_FOR_MODULES=OFF"
                )
                // The one library the app loads. Without it AGP builds every target of the
                // repository root and packages each shared object that comes out, none of which
                // iclforge_hearth_jni needs (it links the sink and the protocol statically): about
                // 5 MB of ac3, ac4, containers and the rest in an app that decodes nothing yet.
                targets += listOf("iclforge_hearth_jni")
            }
        }

        ndk {
            // arm64 is every Android TV box worth the name (the 2017 Shield on). x86_64, for the
            // emulator the instrumented tests of a later phase run on, needs the vcpkg triplet
            // x64-android and is added with them.
            abiFilters += listOf("arm64-v8a")
        }
    }

    // NDK r28c, pinned as the Shield app pins it: a build failure means something changed, not
    // that a different NDK was picked.
    ndkVersion = "28.2.13676358"

    signingConfigs {
        if (releaseSigningAvailable) {
            create("release") {
                storeFile = file(releaseKeystorePath!!)
                storePassword = releaseKeystorePassword
                keyAlias = releaseKeyAlias
                keyPassword = releaseKeyPassword
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            // The repository asks for 3.28...4.3; 3.31.6 is what the SDK manager has for the 3.x line.
            version = "3.31.6"
        }
    }

    buildTypes {
        debug {
            // AGP's debug default is CMAKE_BUILD_TYPE=Debug (-O0), which for this app is also the
            // build type vcpkg would build every port a second time for. RelWithDebInfo keeps the
            // APK debuggable and uses the release builds of the ports.
            externalNativeBuild {
                cmake {
                    // HEARTH_ANDROID_HARNESS compiles the loopback harness's JNI entry in, for the
                    // instrumented tests; the release build has none.
                    arguments += listOf("-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DHEARTH_ANDROID_HARNESS=ON")
                }
            }
            // x86_64 as well as arm64, for the emulator the CI's instrumented tests run on (KVM is only
            // there for x86 images on the hosted runners). arm64-v8a is listed again here so that this
            // build type keeps it whether AGP merges the two lists or this one wins. A local run on a
            // device builds one ABI with -Pandroid.injected.build.abi=arm64-v8a.
            ndk {
                abiFilters += listOf("arm64-v8a", "x86_64")
            }
        }
        release {
            isMinifyEnabled = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfig = if (releaseSigningAvailable) {
                signingConfigs.getByName("release")
            } else {
                signingConfigs.getByName("debug")
            }
            externalNativeBuild {
                cmake {
                    arguments += listOf("-DCMAKE_BUILD_TYPE=Release")
                }
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    // The screens are platform Views and the service, the notification and NsdManager are the
    // platform's: the app has no dependency of its own. These are the instrumented tests', the
    // versions the Shield app's are at.
    androidTestImplementation("junit:junit:4.13.2")
    androidTestImplementation("androidx.test.ext:junit:1.3.0")
    androidTestImplementation("androidx.test:runner:1.7.0")
    androidTestImplementation("androidx.test:core:1.7.0")
}

// The fixtures the instrumented tests play, from the repository's testdata/ (a few hundred KB, staged
// here rather than copied into the tree, so that there is one copy of each).
val harnessFixtures = tasks.register<Copy>("stageHarnessFixtures") {
    val testdata = rootDir.resolve("../../../testdata")
    from(testdata.resolve("external-baseline")) {
        include("ac3-51-448/dee.ac3", "eac3-51-256/dee.ec3")
        into("external-baseline")
    }
    from(testdata.resolve("object-fixture")) {
        include("dee_joc_514.ec3")
        into("object-fixture")
    }
    // The lyric view's fixtures: the karaoke examples' documents (planning/hearth-sink-screen.md,
    // Conformance).
    from(rootDir.resolve("../../../examples/karaoke")) {
        include("**/*.lrc")
        into("karaoke")
    }
    // The conformance fixtures and the frames the library is held to for them (libs/sink/tests).
    from(rootDir.resolve("../../../libs/sink/tests/lyric-frames")) {
        include("*.lrc", "*.txt")
        into("lyric-frames")
    }
    into(layout.buildDirectory.dir("harness-assets"))
}
// The family's faces, from the shared theme (apps/shared/theme/assets/fonts), so that there is one copy
// of each (planning/hearth-sink-screen.md, Tokens): Archivo for Latin, Noto Sans Hebrew and Arabic for
// the scripts a phone may lack, with their licences beside them as the SIL Open Font License asks.
// Greek, Cyrillic and CJK fall back to the system's faces.
val stagedFonts = tasks.register<Copy>("stageFonts") {
    from(rootDir.resolve("../../../apps/shared/theme/assets/fonts")) {
        include(
            "Archivo-Regular.ttf", "Archivo-Medium.ttf", "Archivo-SemiBold.ttf", "Archivo-ExtraBold.ttf",
            "NotoSansHebrew.ttf", "NotoSansArabic.ttf",
            "OFL.txt", "NotoSansHebrew-OFL.txt", "NotoSansArabic-OFL.txt"
        )
        into("fonts")
    }
    into(layout.buildDirectory.dir("font-assets"))
}

// AGP 9 takes no Provider in the source set API: the variant's assets get each directory by path, and
// the tasks above are made dependencies of the tasks that merge them.
androidComponents {
    onVariants { variant ->
        variant.sources.assets?.addStaticSourceDirectory(
            layout.buildDirectory.dir("font-assets").get().asFile.absolutePath
        )
        variant.androidTest?.sources?.assets?.addStaticSourceDirectory(
            layout.buildDirectory.dir("harness-assets").get().asFile.absolutePath
        )
    }
}
// Every task that reads the assets directory by path, the lint model's among them, runs after the copy.
tasks.matching {
    (it.name.startsWith("merge") && it.name.endsWith("Assets")) || it.name.contains("lint", ignoreCase = true)
}.configureEach {
    dependsOn(stagedFonts)
}
tasks.matching { it.name.startsWith("merge") && it.name.endsWith("AndroidTestAssets") }.configureEach {
    dependsOn(harnessFixtures)
}

// SonarCloud text:S8569 - pin resolved dependency versions so a build is reproducible from the
// committed lockfile. Regenerate with `./gradlew :app:dependencies --write-locks` after changing a
// dependency; a build then fails if the resolution drifts from gradle.lockfile.
dependencyLocking {
    lockAllConfigurations()
}

// This app has no dependency of its own, so what the lockfile holds is AGP's: the lint tool
// (androidLintTool) and the unified test platform's work action, build-time only and not in the APK.
// Their pinned releases carry advisories OSV-Scanner reports, so each is forced to the release that
// fixes it, as apps/demos/android/app/build.gradle.kts forces the same set, with the Netty artifacts
// and the protobuf pair that AGP's unified test platform brings for connectedDebugAndroidTest (its
// own configurations are created only inside that task, behind a device check, so the lock lines for
// them are written by running it against a device with --write-locks, as the Shield app's comment
// says at length).
configurations.all {
    resolutionStrategy {
        force(
            "io.netty:netty-buffer:4.2.18.Final",
            "io.netty:netty-codec:4.2.18.Final",
            "io.netty:netty-codec-http:4.2.18.Final",
            "io.netty:netty-codec-http2:4.2.18.Final",
            "io.netty:netty-codec-socks:4.2.18.Final",
            "io.netty:netty-common:4.2.18.Final",
            "io.netty:netty-handler:4.2.18.Final",
            "io.netty:netty-handler-proxy:4.2.18.Final",
            "io.netty:netty-resolver:4.2.18.Final",
            "io.netty:netty-transport:4.2.18.Final",
            "io.netty:netty-transport-native-unix-common:4.2.18.Final",
            // The protobuf-java and protobuf-kotlin pair stays matched: GHSA-735f-pc8j-v9w8.
            "com.google.protobuf:protobuf-java:4.36.2",
            "com.google.protobuf:protobuf-kotlin:4.36.2",
            "commons-io:commons-io:2.22.0",                  // GHSA-78wr-2p64-hpwj
            "org.apache.commons:commons-lang3:3.21.0",       // GHSA-j288-q9x7-2f5v
            "org.apache.httpcomponents:httpclient:4.5.14",   // GHSA-7r82-7xv7-xcpj
            // One release of the three so the set stays matched; 1.86 fixes GHSA-9pwp-9qqc-pr26,
            // GHSA-qp49-qgx5-5m26 and GHSA-c3fc-8qff-9hwx (bcprov) and GHSA-wg6q-6289-32hp (bcpkix).
            "org.bouncycastle:bcprov-jdk18on:1.86",
            "org.bouncycastle:bcpkix-jdk18on:1.86",
            "org.bouncycastle:bcutil-jdk18on:1.86"
        )
    }
}
