import com.github.jengelman.gradle.plugins.shadow.tasks.ShadowJar

plugins {
    `java`
    `maven-publish`
    id("com.github.johnrengelman.shadow") version Versions.shadowJar
}

dependencies {
    implementation(project(":realm-compiler"))
}

tasks {
    named<ShadowJar>("shadowJar") {
        archiveClassifier.set("")
        this.destinationDirectory.set(file("$buildDir/libs"))
        relocate("org.jetbrains.kotlin.com.intellij", "com.intellij")
        dependencies {
            exclude {
                it.moduleName != "realm-compiler"
            }
        }
    }
}
tasks {
    named("jar") {
        actions.clear()
        dependsOn (
            shadowJar
        )
    }
}

publishing {
    publications {
        register("compilerPluginShaded", MavenPublication::class) {
            project.shadow.component(this)
            groupId = Realm.group
            artifactId = Realm.compilerPluginIdNative
            version = Realm.version
        }
    }
}