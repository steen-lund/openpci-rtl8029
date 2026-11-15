# NE2000 Driver Test Procedure

## Test 1: Server Setup (macOS/Linux)

1. Open a terminal on your macOS machine
2. Navigate to the root of this repository

3. Build the server (if needed):
   ```bash
   make -C macOS/src -f Makefile.server
   ```

4. Run the server (it will show you its IP addresses):
   ```bash
   ./macOS/src/server
   ```

5. Note the IP address shown (example: 192.168.1.100)
   The server will wait for a client connection.

## Test 2: Enhanced Client Test (Amiga)

1. Copy `bin/client` to your Amiga
2. Copy `bin/openpci-rtl8029.device` to your Amiga's DEVS:Networks/
3. Configure your TCP/IP stack to use the driver
4. Assign an IP on the same subnet (e.g., 192.168.1.50)

5. Run the client:
   ```
   client SERVERIP <server_ip_address>
   ```

### Expected Output:

The test now performs a **comprehensive bidirectional** packet size test to verify both RX and TX paths:

**Client Output:**
```
Connecting to 192.168.1.100...
Connected!
Received handshake: Hello, Client!
Handshake complete. Receiving test packets...

Packet 1: Received 64 bytes (expected 64)
  OK: Pattern verified (0x41)
Packet 2: Received 100 bytes (expected 100)
  OK: Pattern verified (0x42)
Packet 3: Received 256 bytes (expected 256)
  OK: Pattern verified (0x43)
Packet 4: Received 512 bytes (expected 512)
  OK: Pattern verified (0x44)
Packet 5: Received 1024 bytes (expected 1024)
  OK: Pattern verified (0x45)
Packet 6: Received 1518 bytes (expected 1518)
  OK: Pattern verified (0x46)
Packet 7: Received 2048 bytes (expected 2048)
  OK: Pattern verified (0x47)
Packet 8: Received 4096 bytes (expected 4096)
  OK: Pattern verified (0x48)

RX test complete: 8 packets received, 0 errors

Now sending test packets to server...

TX Packet 1/8: Sending 64 bytes (pattern: 0x51)...
  Server ACK: RX-ACK:1

TX Packet 2/8: Sending 100 bytes (pattern: 0x52)...
  Server ACK: RX-ACK:2

[... continues for all 8 packet sizes ...]

TX test complete: 8 packets sent, 0 errors

Overall: RX=0 errors, TX=0 errors
```

**Server Output:**
```
Waiting for client connection...
Client connected from 192.168.1.50
Received handshake: Hello, Server! (14 bytes)
Handshake complete.

Sending test packet 1/8: 64 bytes (pattern: 0x41)...
  Client ACK: ACK:1

Sending test packet 2/8: 100 bytes (pattern: 0x42)...
  Client ACK: ACK:2

[... continues for all 8 packet sizes ...]

All test packets sent.

Now receiving test packets from client...

RX Packet 1: Received 64 bytes (expected 64)
  OK: Pattern verified (0x51)

RX Packet 2: Received 100 bytes (expected 100)
  OK: Pattern verified (0x52)

[... continues for all 8 packet sizes ...]

Client closed connection.

Client TX test complete: 8 packets received, 0 errors
```

### Test Coverage:

The enhanced bidirectional test verifies:

**Packet Sizes (both RX and TX):**
- **64 bytes**: Minimum Ethernet payload
- **100 bytes**: Small packet
- **256 bytes**: Single NE2000 page
- **512 bytes**: Two pages
- **1024 bytes**: Four pages
- **1518 bytes**: Maximum Ethernet frame size
- **2048 bytes**: Large packet (TCP fragmentation)
- **4096 bytes**: Very large packet (multiple TCP segments)

**RX Test (Server → Client):**
- Server sends 8 test packets with patterns 0x41-0x48 (A-H)
- Client receives and verifies size and pattern integrity
- Tests driver's receive path (RX ring buffer, DMA, packet header parsing)

**TX Test (Client → Server):**
- Client sends 8 test packets with patterns 0x51-0x58 (Q-X)
- Server receives and verifies size and pattern integrity
- Tests driver's transmit path (TX buffer, DMA, hardware transmission)

Each packet contains:
- 4-byte length prefix (network byte order)
- Size header string (e.g., "SIZE:1024:")
- Repeating byte pattern for verification

Both sides verify:
- Correct packet size received matches length prefix
- Size header matches actual data received
- Pattern integrity throughout the entire packet
- TCP stream reassembly (handles partial receives)

**Endianness Test (both directions):**
- After the packet tests, the client sends a length-prefixed, packed 22-byte struct
  (32-bit magic, 16-bit version, 32-bit counter, float, 64-bit timestamp) in network
  byte order; the server verifies each field and replies `ENDIAN-OK` / `ENDIAN-ERROR`
- The server then sends its own struct (different values) and the client verifies it
- Both sides print the raw bytes received, and `PASSED` / `FAILED` per direction

## Test 3: Basic Ping Test

Before running the client, try a simple ping:

```
ping <server_ip_address>
```

This will tell us if basic packet transmission/reception works.

## Debugging

If tests fail, check:

1. Are you seeing debug output? (KPrintF to serial)
2. Does `-> WRITE` appear in debug output?
3. Does `Packet sent` appear?
4. Are there interrupt messages (TX/RX)?
5. Any error messages?

## Diagnostics

- Run `s2stat` on the Amiga (twice, a few seconds apart) to see the driver's counters and
  what its unit task is doing.
- From the Mac, sweep frame sizes with ping: `ping -c 3 -s 1472 <amiga_ip>` is a full-size
  1514-byte frame; `-s 56` a small one.

After the endianness test the client runs a frame size test: UDP datagrams of exact sizes
(60-byte up to full 1514-byte frames) are echoed by the server and compared. Each size prints
`OK` or `LOST`; any `LOST` near 1514 bytes means full-size frames are not getting through.

Note: the test now sends 14 packet sizes per direction (odd and even), not the 8 shown in
the example output above, and the payload bytes vary instead of repeating one value.
