import java.util.Properties
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
}

fun readLocalProperties(root: File): Properties {
    val properties = Properties()
    val file = root.resolve("local.properties")
    if (file.isFile) {
        file.inputStream().use(properties::load)
    }
    return properties
}

val localProperties = readLocalProperties(rootProject.projectDir)
val sdkRoot =
    System.getenv("ANDROID_HOME")
        ?: System.getenv("ANDROID_SDK_ROOT")
        ?: localProperties.getProperty("sdk.dir")
        ?: "/home/izzy/Android/Sdk"

val ndkToolchainFile =
    file("$sdkRoot/ndk/30.0.15729638/build/cmake/android.toolchain.cmake").invariantSeparatorsPath
val vcpkgToolchainFile =
    file("${rootProject.projectDir.parentFile.resolve("vcpkg/scripts/buildsystems/vcpkg.cmake").path}")
        .invariantSeparatorsPath
val androidRoot = rootProject.projectDir

android {
    namespace = "com.izzy2lost.neonsaturn"
    compileSdk = 36
    buildToolsVersion = "36.1.0"
    ndkVersion = "30.0.15729638"

    defaultConfig {
        applicationId = "com.izzy2lost.neonsaturn"
        minSdk = 28
        targetSdk = 36
        versionCode = 1
        versionName = "1.0.0"

        ndk {
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++20")
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchainFile",
                    "-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=$ndkToolchainFile",
                    "-DVCPKG_MANIFEST_DIR=${androidRoot.invariantSeparatorsPath}",
                    "-DVCPKG_TARGET_TRIPLET=arm64-android-neonsaturn",
                    "-DVCPKG_OVERLAY_TRIPLETS=${androidRoot.resolve("vcpkg-triplets").invariantSeparatorsPath}",
                    "-DYmir_ENABLE_DEVLOG=OFF",
                    "-DYmir_ENABLE_IMGUI_DEMO=OFF",
                    "-DYmir_ENABLE_IPO=OFF",
                    "-DYmir_ENABLE_SANDBOX=OFF",
                    "-DYmir_ENABLE_TESTS=OFF",
                    "-DYmir_ENABLE_UPDATE_CHECKS=OFF",
                    "-DYmir_ENABLE_YMDASM=OFF"
                )
            }
        }
    }

    buildFeatures {
        prefab = true
    }

    buildTypes {
        debug {
            isMinifyEnabled = false
        }
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            // The packaged .so is stripped, so keep a symbol file alongside the build for
            // symbolicating native crash reports.
            ndk {
                debugSymbolLevel = "SYMBOL_TABLE"
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlin {
        compilerOptions {
            jvmTarget.set(JvmTarget.JVM_17)
        }
    }

    sourceSets.named("main") {
        kotlin.directories.add("src/main/kotlin")
    }

    externalNativeBuild {
        cmake {
            path = file("../CMakeLists.txt")
            version = "3.30.3"
        }
    }

    packaging {
        jniLibs {
            useLegacyPackaging = false
        }
    }
}

dependencies {
    implementation(files("libs/SDL3-3.4.0.aar"))
    implementation("androidx.activity:activity-ktx:1.9.3")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.documentfile:documentfile:1.0.1")
    implementation("androidx.recyclerview:recyclerview:1.3.2")
    implementation("com.google.android.material:material:1.12.0")
    implementation("io.coil-kt:coil:2.7.0")
}
