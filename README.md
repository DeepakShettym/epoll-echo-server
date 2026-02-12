# High-Performance Epoll Key-Value Store

A lightweight, event-driven **in-memory key-value store** written in C using the Linux `epoll` API.  
The server supports a custom text protocol with **SET, GET, DEL**, **TTL expiration**, and **O(1) LRU eviction**.

This project demonstrates core systems programming concepts including **non-blocking sockets**,  
**stream parsing**, **hash table design**, and **cache eviction policies**.

---

## 🚀 Features

* **Epoll-based:** Scalable I/O multiplexing via Linux `epoll`
* **Non-blocking I/O:** Uses `O_NONBLOCK` for responsiveness
* **Custom Protocol:** Simple text commands (`SET`, `GET`, `DEL`)
* **Stream Parsing:** Handles TCP packet fragmentation correctly
* **Hash Table Storage:** O(1) average lookup
* **TTL Support:** Key expiration using timestamps
* **LRU Eviction:** O(1) least-recently-used removal
* **Single-threaded Event Loop:** Efficient and predictable behavior

---

## 🧠 Architecture

**Storage Engine**

* Hash Table → Fast key lookup
* Doubly Linked List → LRU tracking
* Expiry Timestamp → TTL logic

Each key is tracked in:

1. Hash bucket (indexing)
2. LRU list (recency ordering)

---

## 📡 Protocol

Simple text-based protocol:

SET key value [EX ttl]
GET key
DEL key

Each command must end with:

\n

Example:

SET name Deepak
GET name
DEL name
SET token abc123 EX 5

---

## ⚙️ Requirements

* Linux OS (epoll is Linux-specific)
* GCC Compiler
* netcat / telnet for testing

---

## 🛠 Compilation

```bash
gcc -o my_tcp_server main.c

## ▶️ Run

./my_tcp_server 8080

 or run client_test.py (check if updated to latest) 

## 🔌 Connect (netcat)

nc localhost 8080

## ✅ Sample Output 

deepak@deepak-ThinkPad-E14-Gen-6:~/networking$ nc localhost 8080

set name ronaldo

OK

set country portugal

OK

set club alnasar

OK

get name

ronaldo

set code suii

OK

get country

Key not found

// Since MAX_KV = 3 and we accessed "name",

// it moved to the LRU head.

// "country" became the LRU tail and was evicted.


del name

DELETED

get name

Key not found



