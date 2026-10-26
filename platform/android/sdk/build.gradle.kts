plugins {
    alias(libs.plugins.android.library)
}

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
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DFETCHKIT_BUILD_CLI=OFF",
                    "-DANDROID_STL=c++_static"
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
