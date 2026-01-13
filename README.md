# Adornap-REDIS

**Adornap-REDIS** is a *work-in-progress* Redis-inspired key-value server written in **C++** running on a Linux (WSL) environment.

This project is being built following the excellent guide at  
https://build-your-own.org/redis/#table-of-contents — which teaches how to build a Redis server from scratch in C/C++. 

---

## Overview

Adornap-REDIS aims to replicate and explore the internals of an in-memory key-value store similar to the open-source Redis server — but built from scratch as a learning and engineering exercise.

The goals include:

- ✔ Basic TCP listener and session management  
- ✔ Simple protocol parsing  
- ✔ Custom hash table for key-value storage  
- 🚧 Incremental rehashing (WIP)  
- 🚧 Command execution and RESP support  
- 🚧 More Redis-like commands

This project is not a full Redis implementation yet — it’s designed to teach backend, systems, and networking fundamentals in modern C++.

---

## Why Build This?

Redis is one of the most influential backend infrastructure components in modern systems — used for caching, message brokering, analytics, and more. Understanding how it works from scratch gives you:

- Deep insight into network programming
- Understanding of event loops and non-blocking IO
- Real-world systems design patterns
- Data structure and memory management skills

This is inspired by the *Build Your Own Redis* tutorial which focuses on event loops, server basics, hash tables, protocol parsing, and more. :contentReference[oaicite:2]{index=2}

---

## Current Features

✔ Non-blocking server using `poll()`  
✔ Custom hash map implementation  
✔ Basic GET / SET / DEL command support  
✔ Interactive TCP client (simple testing)

> Note: This project is actively in progress; more features will be added over time.

---

## How to Build

Make sure you have a C++ compiler installed in your UNIX environment (e.g., `g++`).

```bash
g++ -Wall -Wextra -std=c++17 \
    server.cpp hashtable.cpp client.cpp \
    -o adornap_redis

```

Run with:
```bash
./adornap_redis
```

