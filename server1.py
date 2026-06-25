import asyncio
import websockets
import json
import csv
import time
import sys

# ===== CONFIGURATION =====
HOST = "0.0.0.0"
PORT = 1234
FILENAME = "eeg_stream.csv"

# Global Stats
received_packets = 0
last_seq = -1
drops = 0

async def handler(websocket):
    global received_packets, last_seq, drops
    
    print(f"[+] Client connected: {websocket.remote_address}")
    
    # Open CSV in buffered mode for performance
    with open(FILENAME, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["timestamp", "packet_seq", "raw_data"])

        try:
            async for message in websocket:
                recv_time = time.time()
                
                # Fast JSON parse
                try:
                    data = json.loads(message)
                    
                    # Handle both single objects and list batches
                    if isinstance(data, list):
                        for item in data:
                            process_packet(item, recv_time, writer)
                    else:
                        process_packet(data, recv_time, writer)

                except json.JSONDecodeError:
                    continue

        except websockets.exceptions.ConnectionClosed:
            print("[-] Connection closed")

def process_packet(data, recv_time, writer):
    global received_packets, last_seq, drops
    
    # Assuming the ESP32 sends a "seq" field in the JSON
    seq = data.get("seq", 0) 
    received_packets += 1

    # Drop Detection
    if last_seq != -1 and seq != (last_seq + 1):
        gap = seq - last_seq - 1
        drops += gap
        print(f"    [!] Gap detected! Lost {gap} packets. Total drops: {drops}")
    last_seq = seq

    # Write to CSV
    writer.writerow([recv_time, seq, json.dumps(data)])

    # Periodic status update
    if received_packets % 200 == 0:
        print(f"[*] Pkts: {received_packets} | Total Drops: {drops}")

async def main():
    print(f"[*] Starting raw logger on ws://{HOST}:{PORT}")
    # Set ping_interval to None if you want to eliminate all extra traffic
    async with websockets.serve(handler, HOST, PORT, ping_interval=None):
        await asyncio.Future()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print(f"\n[*] Server stopped. Total packets: {received_packets}, Total drops: {drops}")