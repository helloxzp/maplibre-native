// swift-tools-version:5.9
//
// MapLibre iOS（xunji fork）的 SwiftPM 发布清单。
//
// 这个 spm 分支是孤立分支，只放本文件，不含源码 —— 源码在 ppp 分支。
// 每次发版由 ppp 分支的 xunji-release workflow 构建 xcframework、上传到同名 Release，
// 再更新这里的 url / checksum 并打 semver 标签（<上游版本>-xunji.<n>）。
import PackageDescription

let package = Package(
    name: "MapLibre",
    platforms: [.iOS(.v12)],
    products: [.library(name: "MapLibre", targets: ["MapLibre"])],
    targets: [
        .binaryTarget(
            name: "MapLibre",
            url: "https://github.com/helloxzp/maplibre-native/releases/download/6.29.0-xunji.1/MapLibre.dynamic.xcframework.zip",
            checksum: "4ea184073d04f29984045c3146ac3075fab9aa9f29a40d73c7c605ac69d9dcce"
        ),
    ]
)
