use std::process::Command;
use std::sync::{Arc, Mutex};
use tokio::sync::broadcast;
use tokio::net::TcpListener;
use tokio_tungstenite::accept_async;
use futures_util::{SinkExt, StreamExt};

fn normalize_mac_address(input: &str) -> Result<String, String> {
    let compact: String = input
        .chars()
        .filter(|ch| ch.is_ascii_hexdigit())
        .map(|ch| ch.to_ascii_uppercase())
        .collect();

    if compact.len() != 12 {
        return Err("Bluetooth MAC must contain 12 hex digits".into());
    }

    let pairs = compact
        .as_bytes()
        .chunks(2)
        .map(|chunk| std::str::from_utf8(chunk).unwrap())
        .collect::<Vec<_>>();

    Ok(pairs.join(":"))
}

fn run_bash(script: &str, envs: &[(&str, &str)]) -> Result<String, String> {
    let mut command = Command::new("bash");
    command.arg("-lc").arg(script);
    for (key, value) in envs {
        command.env(key, value);
    }

    let output = command
        .output()
        .map_err(|error| format!("Failed to run shell command: {error}"))?;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr).trim().to_string();
        let stdout = String::from_utf8_lossy(&output.stdout).trim().to_string();
        let details = if stderr.is_empty() { stdout } else { stderr };
        return Err(if details.is_empty() {
            "Command failed without output".into()
        } else {
            details
        });
    }

    Ok(String::from_utf8_lossy(&output.stdout).trim().to_string())
}

fn send_rfcomm_payload(mac_address: &str, payload: &str) -> Result<(), String> {
    let rfcomm_script = r#"
set -euo pipefail
printf '%s\n' "$PAIRING_PAYLOAD" | timeout 20s rfcomm connect 0 "$PAIRING_MAC" 1
"#;
    run_bash(
        rfcomm_script,
        &[("PAIRING_MAC", mac_address), ("PAIRING_PAYLOAD", payload)],
    )?;

    Ok(())
}

struct WsServer {
    tx: Option<broadcast::Sender<String>>,
}

impl Default for WsServer {
    fn default() -> Self {
        Self { tx: None }
    }
}

// Starts a WebSocket broadcast server on port 9001.
// Safe to call multiple times — only starts once.
#[tauri::command]
async fn start_ws_server(
    state: tauri::State<'_, Arc<Mutex<WsServer>>>,
) -> Result<(), String> {
    let already_running = state.lock().unwrap().tx.is_some();
    if already_running {
        return Ok(());
    }

    let (tx, _) = broadcast::channel::<String>(32);
    state.lock().unwrap().tx = Some(tx.clone());

    tokio::spawn(async move {
        let listener = TcpListener::bind("0.0.0.0:9001")
            .await
            .expect("Failed to bind port 9001");

        loop {
            if let Ok((stream, _)) = listener.accept().await {
                let mut rx = tx.subscribe();
                tokio::spawn(async move {
                    use tokio_tungstenite::tungstenite::Message;
                    if let Ok(ws) = accept_async(stream).await {
                        let (mut write, mut read) = ws.split();
                        loop {
                            tokio::select! {
                                // Outgoing: forward broadcast nav messages to this client
                                msg = rx.recv() => {
                                    match msg {
                                        Ok(text) => {
                                            if write.send(Message::Text(text.into())).await.is_err() {
                                                break; // client gone
                                            }
                                        }
                                        // Slow client fell behind — skip dropped msgs, keep connection
                                        Err(broadcast::error::RecvError::Lagged(_)) => continue,
                                        // Sender dropped (server shutting down)
                                        Err(broadcast::error::RecvError::Closed) => break,
                                    }
                                }
                                // Incoming: drive the protocol (ping/pong handled by tungstenite),
                                // and detect client close. Without this the socket isn't polled
                                // and the connection dies.
                                incoming = read.next() => {
                                    match incoming {
                                        Some(Ok(Message::Text(text))) => {
                                            let _ = tx.send(text.to_string());
                                        }
                                        Some(Ok(Message::Close(_))) | None => break,
                                        Some(Err(_)) => break,
                                        Some(Ok(_)) => {} // ignore client data/ping (auto-ponged)
                                    }
                                }
                            }
                        }
                    }
                });
            }
        }
    });

    Ok(())
}

// Broadcasts a JSON nav state string to all connected Pi displays.
#[tauri::command]
async fn broadcast_nav(
    payload: String,
    state: tauri::State<'_, Arc<Mutex<WsServer>>>,
) -> Result<(), String> {
    let tx = state.lock().unwrap().tx.clone();
    if let Some(tx) = tx {
        let _ = tx.send(payload);
    }
    Ok(())
}

#[tauri::command]
async fn send_pi_wifi_credentials(
    mac_address: String,
    ssid: String,
    password: String,
) -> Result<String, String> {
    let ssid = ssid.trim().to_string();
    if ssid.is_empty() {
        return Err("SSID cannot be empty".into());
    }

    if password.is_empty() {
        return Err("Password cannot be empty".into());
    }

    let normalized_mac = normalize_mac_address(&mac_address)?;
    let payload = format!("{}_{}", ssid, password);

    send_rfcomm_payload(&normalized_mac, &payload)?;

    Ok(format!(
        "Credentials sent to {} on RFCOMM channel 1",
        normalized_mac
    ))
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .manage(Arc::new(Mutex::new(WsServer::default())))
        .invoke_handler(tauri::generate_handler![
            start_ws_server,
            broadcast_nav,
            send_pi_wifi_credentials
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
