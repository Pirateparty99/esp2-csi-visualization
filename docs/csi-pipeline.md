# The CSI pipeline

How one CSI reading travels from the radio to the heatmap, and every place it
can be dropped along the way.

## State machine

```
   ┌──────────────────────────────────────────────────────────────────────┐
   │ ON A NODE                                                            │
   │                                                                      │
   │   [1] frame arrives on the radio                                     │
   │        │                                                             │
   │        │  CSI only exists for RECEIVED frames. A silent channel      │
   │        │  produces nothing -- this is why filler traffic exists.     │
   │        ▼                                                             │
   │   [2] PHY computes channel estimate                                  │
   │        │                                                             │
   │        ▼                                                             │
   │   [3] _wifi_csi_cb()            csi_component.h                      │
   │        │   runs on the Wi-Fi task, holding the CSI mutex.            │
   │        │   MUST NOT BLOCK: stalling here wedges Wi-Fi RX.            │
   │        │                                                             │
   │        ├─── SEND_CSI_TO_SERIAL ──▶ printf CSV  (separate format)     │
   │        │                                                             │
   │        ▼                                                             │
   │   [4] dispatch                                                       │
   │        │                                                             │
   │        ├── SEND_CSI_TO_MESH ─┐            ├── else SEND_CSI_TO_UDP ─┐│
   │        │                     │            │                         ││
   │        ▼                     │            ▼                         ││
   │   [5a] am I root? ───yes───────────────▶ [5b] rate limit 20Hz        ││
   │        │ no                  │                │  excess DROPPED      ││
   │        ▼                     │                ▼                      ││
   │   [6] csi_to_json()          │           sendto() ──────────────┐    ││
   │        │  stamps "node"      │                                  │    ││
   │        ▼                     │                                  │    ││
   │   [7] xQueueSend (depth 8)   │                                  │    ││
   │        │  FULL -> DROPPED, counted as s_mesh_csi_dropped        │    ││
   │        ▼                                                        │    ││
   │   [8] mesh_tx_task  ── sole owner of esp_mesh_send() ──┐        │    ││
   │        │  drains CSI first; emits HEARTBEAT when idle  │        │    ││
   │        ▼                                               │        │    ││
   │   [9] esp_mesh_send(NONBLOCK) ──▶ parent ──▶ ... ──▶ root       │    ││
   └────────────────────────────────────────────────────────┼────────┼────┘
                                                            │        │
   ┌────────────────────────────────────────────────────────┼────────┼────┐
   │ ON THE MESH ROOT                                       ▼        │    │
   │  [10] mesh_root_rx_task: esp_mesh_recv()                        │    │
   │         ├── HEARTBEAT marker ──▶ DROPPED (never leaves mesh)    │    │
   │         └── CSI JSON ──▶ csi_udp_sender_send_raw() ─────────────┤    │
   └─────────────────────────────────────────────────────────────────┼────┘
                                                                     │
                                     UDP :5566                       │
                                          ▼◀───────────────────────────────┘
   ┌──────────────────────────────────────────────────────────────────────┐
   │ ON THE PI / COLLECTOR                                                │
   │  [11] receive datagram                                               │
   │  [12] parse JSON; no "node" field -> stale firmware, SKIP            │
   │  [13] key by directed link (node, mac)                               │
   │  [14] compare against rti_baseline.json                              │
   │  [15] render heatmap                                                 │
   └──────────────────────────────────────────────────────────────────────┘
```

## Where readings get dropped

Each of these is deliberate. When the collector sees fewer readings than
expected, work down this list.

| # | Stage | Cause | Visible as |
|---|---|---|---|
| 1 | No RX | Channel is idle; nothing to measure | Silence. The reason filler traffic exists at all |
| 5b | UDP rate limit | More than 20 captures/sec on a direct-UDP node | Steady cap, not an error |
| 7 | Queue full | Mesh TX slower than capture rate | `dropped` in the node's `tx:` log line |
| 9 | Send failure | Upstream window closed, no route yet | `failed` with an error code in `tx:` |
| 10 | Heartbeat filter | Filler correctly discarded at the root | `dropped N heartbeats` in the root's log |
| 12 | Missing `node` | Node still on pre-`node`-field firmware | Collector skips; reflash that node |

## Counters

Both ends log every 5 seconds, and the two should roughly agree:

```
# leaf
mesh_csi: tx: 69 CSI, 69 heartbeats, 0 failed (last 0x0), 0 dropped, layer:2 root:0

# root
mesh_root_rx: forwarded 66 CSI packets, dropped 66 heartbeats
```

A leaf reporting sends while the root forwards nothing means the mesh is not
delivering. A root forwarding while the collector sees nothing means the
problem is on the network path, not the firmware.

## Filler traffic

CSI only exists for frames a node *receives*, so an idle deployment measures
nothing. Two mechanisms keep links busy, and neither carries sensing data:

- **Mesh heartbeat** — a marker packet a leaf sends when its CSI queue is
  empty. It keeps the parent/child link producing frames. The root drops it,
  so it never reaches the collector.
- **Station traffic** — in `active_sta`, ordinary uplink traffic to the AP
  serves the same purpose, and with `CSI_PROMISCUOUS` a node's uplink is also
  what its neighbours measure. No extra transmissions needed.

A root once pinged the UDP target as filler; that was removed because the
1-byte datagrams landed on the same port as real readings and made up roughly
a quarter of the collector's stream.

> **Consequence:** a lone node with no peers generates almost no CSI, since
> nothing is transmitting for it to measure.

## CSI JSON format

Each UDP datagram is one reading:

```json
{"type":"CSI_DATA","node":"70:4b:ca:27:17:a0","mac":"b4:bf:e9:60:4a:7d","rssi":-54,"len":384,"csi":[66,32,4,...]}
```

| Field | Description |
|---|---|
| `type` | Always `CSI_DATA`. |
| `node` | MAC of the node that **observed** this reading (its STA interface). |
| `mac` | MAC of the peer that **transmitted** the measured frame. |
| `rssi` | Received signal strength of that frame, in dBm. |
| `len` | Length of the CSI buffer as reported by the driver. |
| `csi` | Up to 128 signed values (interleaved I/Q pairs). |

`node` and `mac` together name a **directed link**: `node` heard a frame from
`mac`. Both are needed — `mac` alone identifies the transmitter, not the
reporter, and the datagram's source IP is no help either, since on a mesh every
reading arrives from the root regardless of which node captured it.

> Readings produced before the `node` field was added lack it entirely.
> Collectors should skip those rather than misattribute them, and any node still
> sending them needs reflashing.

The serial output (`SEND_CSI_TO_SERIAL`) is a separate, wider CSV format
inherited from the upstream toolkit and does **not** match the JSON above:

```
type,role,mac,rssi,rate,sig_mode,mcs,bandwidth,smoothing,not_sounding,aggregation,stbc,fec_coding,sgi,noise_floor,ampdu_cnt,channel,secondary_channel,local_timestamp,ant,sig_len,rx_state,real_time_set,real_timestamp,len,CSI_DATA
```
