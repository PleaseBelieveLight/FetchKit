plugins {
    alias(libs.plugins.android.library)
    `maven-publish`
}

abstract class GenerateProjectLicense : DefaultTask() {
    @get:InputFile
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val licenseFile: RegularFileProperty

    @get:OutputDirectory
    abstract val outputDirectory: DirectoryProperty

    @TaskAction
    fun generate() {
        val destination = outputDirectory.file("META-INF/fetchkit/LICENSE").get().asFile
        destination.parentFile.mkdirs()
        licenseFile.get().asFile.copyTo(destination, overwrite = true)
    }
}

val projectLicense = rootProject.layout.projectDirectory.file("LICENSE")

val sdkVersion = providers.fileContents(rootProject.layout.projectDirectory.file("VERSION")).asText.get().trim()
require(sdkVersion.matches(Regex("[0-9]+[.][0-9]+[.][0-9]+")))
group = "com.luofelix.fetchkit"
version = sdkVersion

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
            //noinspection ChromeOsAbiSupport
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
    publishing {
        singleVariant("release") { withSourcesJar() }
    }
}

androidComponents {
    onVariants { variant ->
        val licenseResources =
            tasks.register<GenerateProjectLicense>("generate${variant.name.replaceFirstChar { it.uppercase() }}ProjectLicense") {
                description = ""
                licenseFile.set(projectLicense)
            }
        variant.sources.resources?.addGeneratedSourceDirectory(licenseResources, GenerateProjectLicense::outputDirectory)
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

extensions.configure<PublishingExtension> {
    publications {
        register<MavenPublication>("release") {
            groupId = project.group.toString()
            artifactId = "fetchkit"
            version = sdkVersion
            pom {
                name.set("FetchKit")
                description.set("C++ resource downloader with Android Kotlin/JNI integration")
                licenses {
                    license {
                        name.set("The Apache License, Version 2.0")
                        url.set("https://www.apache.org/licenses/LICENSE-2.0.txt")
                        distribution.set("repo")
                    }
                }
            }
        }
    }
    project.afterEvaluate {
        val releaseComponent = components["release"]
        publications.named<MavenPublication>("release") {
            from(releaseComponent)
        }
    }
    repositories {
        maven {
            name = "preview"
            url = uri(rootProject.layout.buildDirectory.dir("m5-maven"))
        }
    }
}

tasks.withType<Jar>().configureEach {
    if (name == "sourceReleaseJar") {
        from(projectLicense) { into("META-INF/fetchkit") }
    }
}

tasks.withType<PublishToMavenRepository>().configureEach {
    val publicationContainsProbe = platformProbe
    val publicationAbis = selectedAbis.toSet()
    doFirst {
        require(!publicationContainsProbe) { "Publication must exclude experimental probes" }
        require(publicationAbis == setOf("arm64-v8a", "x86_64")) {
            "Publication requires both supported ABIs"
        }
    }
}
