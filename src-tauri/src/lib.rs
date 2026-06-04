use std::sync::{Arc, Mutex};
use tokio::sync::broadcast;
use tokio::net::TcpListener;
use tokio_tungstenite::accept_async;
use futures_util::SinkExt;

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
                    if let Ok(ws) = accept_async(stream).await {
                        let (mut write, _) = futures_util::StreamExt::split(ws);
                        while let Ok(msg) = rx.recv().await {
                            if write
                                .send(tokio_tungstenite::tungstenite::Message::Text(msg.into()))
                                .await
                                .is_err()
                            {
                                break;
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

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .manage(Arc::new(Mutex::new(WsServer::default())))
        .invoke_handler(tauri::generate_handler![start_ws_server, broadcast_nav])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
