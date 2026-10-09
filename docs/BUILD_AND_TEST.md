# Build & Test

The supported build is Linux/x86-64 with GCC 14 or newer, clang's BPF backend, CMake 3.20 or newer, pkg-config, and libbpf development files. The running kernel must support AF_XDP and the busy-poll socket options configured in `main.cpp`.

On Ubuntu, install the build dependencies and tools for the veth smoke test:

```bash
sudo apt-get install cmake clang gcc-14 g++-14 pkg-config \
    libbpf-dev libelf-dev zlib1g-dev iproute2 ethtool python3
```

From the repository root:

```bash
cmake -S . -B build -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 1
```

This builds `afxdp_receiver` and `xdp_prog.bpf.o`. CMake also generates the build information embedded in the executable. Always run from the directory containing the matching BPF object:

```bash
cd build
sudo ./afxdp_receiver -i eth0 -f 192.0.2.10 -q 0
```

Replace the interface, source IP, and queue with the configured flow. Root is the simplest way to provide the socket, BPF, and memory-locking privileges. Startup raises `RLIMIT_MEMLOCK`; the environment must permit it.

| Option | Meaning |
|---|---|
| `-i` | Interface; defaults to `eth0` |
| `-f` | Required source IPv4 address to capture |
| `-q` | RX queue; defaults to 0 and must match the flow's queue |
| `-c` | Worker CPU; otherwise one is selected during startup and pinned |
| `-r` | Request `SCHED_FIFO` priority 99 for the worker |
| `--no-hugepages` | Use anonymous UMEM mapping without requesting huge pages |
| `--no-zerocopy` | Force copy mode |

The startup log reports copy versus zero-copy and native versus generic XDP attachment. Verify these actual modes before comparing runs. Keep the binary and BPF object together; a service should set `WorkingDirectory` to their directory.

For a separate sanitizer build, use `-DCMAKE_BUILD_TYPE=Debug -DAFXDP_SANITIZER=address` with a different build directory. The address option enables ASan and UBSan; the supported thread option enables TSan and UBSan. Use Release without sanitizers for measurements.

The following functional check creates an isolated veth pair. It exercises UDP reflection, not physical-NIC performance. The reflector preserves input checksums, so this lab disables TX checksum offload to supply completed checksums in the packet bytes. See the kernel's [checksum-offload documentation](https://docs.kernel.org/networking/checksum-offloads.html).

```bash
sudo ip netns add xdptest
sudo ip link add veth0 type veth peer name veth1
sudo ip link set veth1 netns xdptest
sudo ip addr add 10.10.0.1/24 dev veth0
sudo ip netns exec xdptest ip addr add 10.10.0.2/24 dev veth1
sudo ethtool -K veth0 tx off
sudo ip netns exec xdptest ethtool -K veth1 tx off
sudo ip link set veth0 up
sudo ip netns exec xdptest ip link set veth1 up
sudo ip netns exec xdptest ip link set lo up
```

Start the receiver from `build/`:

```bash
sudo ./afxdp_receiver -i veth0 -f 10.10.0.2 --no-zerocopy --no-hugepages
```

Once it reports `Running`, send a UDP probe from a second terminal:

```bash
sudo ip netns exec xdptest python3 - <<'PY'
import socket

payload = b"afxdp reflection check"
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
    sock.settimeout(2)
    sock.sendto(payload, ("10.10.0.1", 9000))
    reply, peer = sock.recvfrom(2048)
    assert reply == payload and peer == ("10.10.0.1", 9000), (reply, peer)
    print("UDP reflection passed")
PY
```

A valid reply confirms the round trip. `rx_frames_processed`, `tx_frames_submitted`, and `tx_frames_reclaimed` should increase. No UDP server is needed on port 9000: the AF_XDP callback reflects the packet. ICMP `ping` does not test this UDP-only callback. A probe sent during the one-second warmup may produce no latency samples.

Stop the receiver with Ctrl+C before deleting the namespace; deleting its veth endpoint removes the pair:

```bash
sudo ip netns del xdptest
```

Common setup failures:

| Symptom | Check |
|---|---|
| `setrlimit(RLIMIT_MEMLOCK)` fails | Process/container memory-locking permissions and limits |
| Busy-poll `setsockopt` fails | The reported errno, kernel support, and required privileges |
| AF_XDP bind fails | Interface, queue, driver support, and another socket bound to the queue |
| BPF object cannot be opened or expected maps are missing | Working directory and whether the object matches the executable |
| No RX processing | Source-IP filter, interface, and queue steering |
| RX increases but no UDP reply | `unsupported_frames`, TX rejection/error counters, and the packet contract in the README |

For a physical NIC, configure RSS or explicit steering so the flow reaches `-q`. Driver/kernel support determines available operating modes; busy polling alone does not establish IRQ suppression. The [benchmark guide](BENCHMARKING.md) describes the recorded outputs and their limits.
