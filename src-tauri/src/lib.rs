use std::sync::{Arc, Mutex};
use tokio::sync::broadcast;
use tokio::net::TcpListener;
use tokio_tungstenite::accept_async;
use futures_util::{SinkExt, StreamExt};

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

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .manage(Arc::new(Mutex::new(WsServer::default())))
        .invoke_handler(tauri::generate_handler![start_ws_server, broadcast_nav])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
