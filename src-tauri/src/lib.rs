// Streaming to the ESP32 display happens entirely on the Android side over BLE
// (see PiNavigationService.kt). No desktop transport / WebSocket server is needed.
#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
