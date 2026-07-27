# Architecture

A Raspberry Pi runs the access point and every ESP32 associates to it. Each node
captures CSI, serializes it to JSON, and sends it over UDP to the Pi on port
5566. The Pi optionally relays that stream on to a workstation for analysis.

```
                      deployment nodes (6)
        ┌─────┬─────┬─────┬─────┬─────┬─────┐
        │ ESP │ ESP │ ESP │ ESP │ ESP │ ESP │
        └──┬──┴──┬──┴──┬──┴──┬──┴──┬──┴──┬──┘
           └─────┴─────┴──┬──┴─────┴─────┘
                          │
                          │   CSI JSON over UDP :5566
                          ▼
            ┌───────────────────────────┐
            │  Raspberry Pi             │
            │  AP + DHCP                │
            │  192.168.4.1   channel 11 │
            └─────────────┬─────────────┘
                          │  optional relay :5566
                          ▼
              visualizations/rti-aggregator.py
```

Two link types are measured, distinguished by the `node` and `mac` fields in
each reading (see [CSI pipeline](csi-pipeline.md#csi-json-format)):

```
  node = observer,  mac = Pi AP     ->  node <-> AP link    (primary, always present)
  node = observer,  mac = peer node ->  node <-> node link  (requires CSI_PROMISCUOUS)
```

`node <-> AP` links form a star: every node measures its own path to the Pi.
Those alone give poor tomographic coverage, since all paths radiate from one
point. `node <-> node` links cross the room and disambiguate position far
better, which is why `CSI_PROMISCUOUS` is on by default.

## Firmware modes

| Mode | Project | When to use |
|---|---|---|
| **active_sta** *(recommended)* | `active_sta` | All nodes within range of the Pi. Stable link geometry, no relay, no root election. With `CSI_PROMISCUOUS` it yields both link types. |
| active_ap | `active_ap` | An ESP32 acts as the AP instead of the Pi. |
| wifi-mesh | `wifi-mesh` | Only when some nodes cannot reach the Pi directly and need multi-hop relay. |

**On mesh:** it works and delivers CSI end to end, but ESP-MESH picks its own
parent/child tree and rearranges it on re-parenting and root re-election. The
root role is not stable across reboots. Since the measured links *are* the tree,
the sensing baseline moves whenever the topology changes, which invalidates
calibration. Prefer `active_sta` unless multi-hop is genuinely required.
