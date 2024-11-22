# P2P File Sharing System

## Overview
This project implements a peer-to-peer (P2P) file sharing system consisting of an index server and multiple peer clients. The system enables peers to share and download files from each other, with the index server facilitating peer discovery and content registration.

## Key Components

### Index Server
- Maintains a registry of available content and hosting peers
- Handles content registration and deregistration
- Implements load balancing by tracking peer usage
- Uses UDP for communication with peers
- Provides content listing and peer lookup services

### Peer Client
- Acts as both content server and client
- Can register files for sharing
- Downloads content from other peers
- Uses TCP for file transfers between peers
- Uses UDP for communication with index server

## Communication Protocol
1. **UDP Protocol (Peer ↔ Index Server)**
   - Registration (R): Register content for sharing
   - Search (S): Look up content location
   - List (O): Request available content
   - Deregister (T): Remove shared content
   - Acknowledgment (A): Confirm operations
   - Error (E): Signal operation failures

2. **TCP Protocol (Peer ↔ Peer)**
   - Used for actual file transfers
   - Implements reliable data transmission
   - Handles large file transfers in chunks

## Key Features
- Distributed content hosting
- Load balancing across multiple peers
- Fault tolerance with retry mechanisms
- Support for multiple simultaneous transfers
- Automatic content replication (downloaded content becomes available for sharing)
- Real-time content listing updates

## Implementation Details
- Written in C for POSIX-compliant systems
- Uses socket programming for network communication
- Implements custom PDU (Protocol Data Unit) structures
- Provides robust error handling and timeout mechanisms
- Supports concurrent connections through process forking

## Usage Scenario
1. Peer A registers content with index server
2. Peer B queries index server for content
3. Index server provides Peer A's address to Peer B
4. Peer B downloads content from Peer A via TCP
5. Peer B automatically becomes a new host for the content
6. Subsequent peers can download from either Peer A or B

This implementation creates a scalable and resilient file-sharing network where content availability improves as more peers download and share files.
