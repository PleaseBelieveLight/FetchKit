plugins { id("com.android.application") version "9.4.1" }

val coroutinesAar = providers.gradleProperty("fetchkit.coroutinesAar").orNull
val fetchkitVersion = providers.gradleProperty("fetchkit.version")
    .orElse(providers.fileContents(layout.projectDirectory.file("../../VERSION")).asText.map { it.trim() })

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(25))
        vendor.set(JvmVendorSpec.ADOPTIUM)
    }
}

android {
    namespace = "com.luofelix.fetchkit.sizehost"
    compileSdk { version = release(37) }
    enableKotlin = coroutinesAar != null
    sourceSets.named("main") {
        if (coroutinesAar == null) java.srcDir("src/future/java")
    }
    defaultConfig {
        applicationId = "com.luofelix.fetchkit.sizehost"
        minSdk = 24
        targetSdk = 37
        versionCode = 1
        versionName = fetchkitVersion.get()
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

androidComponents {
    onVariants { variant ->
        if (coroutinesAar != null) variant.sources.kotlin?.addStaticSourceDirectory("src/coroutines/java")
    }
}

dependencies {
    implementation("com.luofelix.fetchkit:fetchkit:" + fetchkitVersion.get())
    if (coroutinesAar != null) {
        implementation(files(coroutinesAar))
        implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:" + providers.gradleProperty("fetchkit.coroutinesVersion").get())
    }
}
