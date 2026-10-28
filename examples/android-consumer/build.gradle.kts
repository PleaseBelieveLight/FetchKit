plugins { id("com.android.application") version "9.4.1" }

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(25))
        vendor.set(JvmVendorSpec.ADOPTIUM)
    }
}

android {
    namespace = "com.luofelix.fetchkit.sizehost"
    compileSdk { version = release(37) }
    enableKotlin = false
    defaultConfig {
        applicationId = "com.luofelix.fetchkit.sizehost"
        minSdk = 24
        targetSdk = 37
        versionCode = 1
        versionName = providers.gradleProperty("fetchkit.version").get()
        ndk { abiFilters += providers.gradleProperty("fetchkit.abi").orElse("arm64-v8a").get() }
    }
    buildTypes {
        release {
            optimization {
                enable = true
                packageScope = setOf("com.luofelix.**", "okhttp3.**", "okio.**", "androidx.**", "kotlin.**", "kotlinx.**")
            }
            // Local acceptance APK only; never use this key for distribution.
            signingConfig = signingConfigs.getByName("debug")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    buildFeatures { buildConfig = false }
}

dependencies {
    implementation("com.luofelix.fetchkit:fetchkit:" + providers.gradleProperty("fetchkit.version").get())
}
