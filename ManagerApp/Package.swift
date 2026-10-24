// swift-tools-version: 5.5
// AES67 Manager App Package

import PackageDescription

let package = Package(
    name: "AES67Manager",
    platforms: [
        .macOS(.v11)
    ],
    products: [
        .executable(
            name: "AES67Manager",
            targets: ["AES67Manager"]
        )
    ],
    targets: [
        .executableTarget(
            name: "AES67Manager",
            path: ".",
            sources: [
                "ManagerMain.swift"
            ]
        )
    ]
)
