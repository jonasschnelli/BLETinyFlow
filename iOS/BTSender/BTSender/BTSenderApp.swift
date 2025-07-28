//
//  BTSenderApp.swift
//  BTSender
//
//  Created by Jonas Schnelli on 7/26/25.
//

import SwiftUI

@main
struct BTSenderApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
        #if os(macOS)
        .windowStyle(.titleBar)
        .frame(minWidth: 400, minHeight: 300)
        #endif
    }
}
