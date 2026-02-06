## 🛠 Testing & Performance

This server has been stress-tested to handle high concurrency. 

### 10k Connection Milestone
Using a Python-based multi-threaded stress tester, the server successfully handled **10,000 simultaneous connections** on a single thread.

* **Target:** 10,000 Clients
* **Result:** 100% success rate, 0 dropped packets.
* **Key Learning:** Increasing the system limit via `ulimit -n 20000` was required to allow the OS to handle the file descriptor load.

### How to Reproduce the Test
1. Increase the open file limit:
   ```bash
   ulimit -n 20000


### Sample output 

Client 9990 received: Hello from client! (ID: 9990)

Client 9987 received: Hello from client! (ID: 9987)

Client 9992 received: Hello from client! (ID: 9992)

Client 9991 received: Hello from client! (ID: 9991)

Client 9993 received: Hello from client! (ID: 9993)

Client 9994 received: Hello from client! (ID: 9994)

Client 9989 received: Hello from client! (ID: 9989)

Client 9995 received: Hello from client! (ID: 9995)

Client 9997 received: Hello from client! (ID: 9997)

Client 9998 received: Hello from client! (ID: 9998)

Client 10001 received: Hello from client! (ID: 10001)

Client 10000 received: Hello from client! (ID: 10000)

Client 9996 received: Hello from client! (ID: 9996)

Client 9999 received: Hello from client! (ID: 9999)

Test complete.

