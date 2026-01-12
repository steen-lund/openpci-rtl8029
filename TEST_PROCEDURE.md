# NE2000 Driver Test Procedure

## Test 1: Server Setup (macOS/Linux)

1. Open a terminal on your macOS machine
2. Navigate to the driver directory:
   ```bash
   cd /Users/steenlund/Amiga/openpci-rtl8029-src
   ```

3. Build the server (if needed):
   ```bash
   cd macOS/src
   make
   cd ../..
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

The test will now perform a comprehensive packet size test:

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
Connection closed by server.

Test complete: 8 packets received, 0 errors
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

[... continues for all packet sizes ...]

All test packets sent.
```

### Test Coverage:

The enhanced test verifies:
- **64 bytes**: Minimum Ethernet payload
- **100 bytes**: Small packet
- **256 bytes**: Single NE2000 page
- **512 bytes**: Two pages
- **1024 bytes**: Four pages
- **1518 bytes**: Maximum Ethernet frame size
- **2048 bytes**: Large packet (TCP fragmentation)
- **4096 bytes**: Very large packet (multiple TCP segments)

Each packet contains:
- Size header (e.g., "SIZE:1024:")
- Repeating byte pattern for verification (A-Z pattern based on packet number)

The client verifies:
- Correct packet size received
- Pattern integrity throughout the packet
- Reports any errors

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

## Current Known Issues

- DMA reads are returning 0x0000/0xFFFF patterns
- This suggests cache barriers aren't working correctly
- Buffer overflow is occurring
- May need different approach to cache coherency
