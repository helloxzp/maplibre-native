# xunji MapLibre Android Maven 仓库

孤立分支，只放 Maven 仓库文件，不含源码（源码在 ppp 分支）。由 ppp 分支的 `xunji-release` workflow 发布。

```kotlin
repositories {
    maven("https://raw.githubusercontent.com/helloxzp/maplibre-native/maven")
}
dependencies {
    implementation("org.maplibre.gl:android-sdk:13.5.1-xunji.1")
}
```

版本：`<上游 Android 版本>-xunji.<n>`。
