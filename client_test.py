import socket
import threading
import time

# Server configuration
HOST = '127.0.0.1'
PORT = 8080
NUM_CLIENTS = 5
MESSAGE = b"yo whats app!"

def simulate_client(client_id):
    try:
        # Create a socket
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((HOST, PORT))
        
        # Send data
        s.sendall(MESSAGE + f" (ID: {client_id})".encode())
        
        # Receive echo
        data = s.recv(1024)
        print(f"Client {client_id} received: {data.decode()}")
        
        # Keep connection open for a bit to test concurrency
        time.sleep(2) 
        s.close()
    except Exception as e:
        print(f"Client {client_id} error: {e}")

# Create and start threads
threads = []
print(f"Starting {NUM_CLIENTS} clients...")

for i in range(NUM_CLIENTS):
    t = threading.Thread(target=simulate_client, args=(i,))
    threads.append(t)
    t.start()

# Wait for all threads to finish
for t in threads:
    t.join()

print("Test complete.")
