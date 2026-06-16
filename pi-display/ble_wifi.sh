#!/bin/bash
# Description: Standard Bluetooth Serial Port Listener for Wi-Fi Provisioning

echo "Initializing Bluetooth Wi-Fi Provisioner..."

# Reset background daemon loops to avoid org.bluez.Error.Busy lockouts
sudo systemctl restart bluetooth
sleep 1

# Wake up the hardware controller cleanly
sudo rfkill unblock bluetooth
sudo bluetoothctl power on
sleep 1

# Configure network visibility parameters
sudo bluetoothctl discoverable on
sudo bluetoothctl pairable on
sudo bluetoothctl system-alias "Pi-Provisioner"

# Open standard Serial Link (RFCOMM Channel 1)
sudo rfcomm watch hci0 1 /bin/bash -c '
    echo "Tauri client connected!"
    
    # Extract line stream using the internal serial frame anchor
    read -r incoming_data < /dev/rfcomm0
    echo "Received Payload string: \$incoming_data"
    
    # Isolate tokens using the underscore delimiter pattern
    SSID=\$(echo "\$incoming_data" | cut -d'\''_'\'' -f1)
    PASSWORD=\$(echo "\$incoming_data" | cut -d'\''_'\'' -f2)
    
    echo "Executing connection profile for: \$SSID..."
    nmcli device wifi connect "\$SSID" password "\$PASSWORD"
'
