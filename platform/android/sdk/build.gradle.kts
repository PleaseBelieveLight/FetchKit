plugins {
    alias(libs.plugins.android.library)
}

val platformProbe = providers.gradleProperty("fetchkit.probes").orNull == "true"
val probeBackend = providers.gradleProperty("fetchkit.probeBackend").orNull ?: "platform"
require(probeBackend in setOf("platform", "okhttp"))
val selectedAbis = providers.gradleProperty("fetchkit.abis").orNull
    ?.split(",") ?: listOf("arm64-v8a", "x86_64")
require(selectedAbis.isNotEmpty() && selectedAbis.all { it in setOf("arm64-v8a", "x86_64") })

android {
    namespace = "com.luofelix.fetchkit"
    compileSdk {
        version = release(37)
    }
    ndkVersion = libs.versions.ndk.get()

    defaultConfig {
        minSdk = 24
        consumerProguardFiles("consumer-rules.pro")

        ndk {
            abiFilters += selectedAbis
        }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DFETCHKIT_BUILD_CLI=OFF",
                    "-DANDROID_STL=c++_static",
                    "-DFETCHKIT_ENABLE_PLATFORM_PROBE=$platformProbe"
                )
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("CMakeLists.txt")
            version = libs.versions.cmake.get()
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    buildFeatures {
        buildConfig = false
    }
}

androidComponents {
    onVariants { variant ->
        if (platformProbe) {
            variant.sources.kotlin?.addStaticSourceDirectory("src/probe/java")
            variant.sources.kotlin?.addStaticSourceDirectory(
                if (probeBackend == "okhttp") "src/probeOkHttp/java" else "src/probePlatform/java"
            )
        }
    }
}

dependencies {
    implementation(libs.okhttp)
    testImplementation(libs.junit)
}
