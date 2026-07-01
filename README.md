# zmk-steno-engine

Clean-room steno engine for ZMK keyboards. Split-storage architecture: full dictionary on peripheral (right) half, queried over BLE from central (left) half.

**Status:** Early development — not yet functional.

## Architecture

ZMK splits run two nRF52840 halves connected via BLE. The right (peripheral) half has ~533KB free flash — enough for a full steno dictionary. The left (central) half runs the steno engine.

```
┌─────────────┐  BLE GATT query  ┌──────────────┐
│  Left half   │ ───────────────► │  Right half   │
│  (central)   │ ◄─────────────── │ (peripheral)  │
│              │   translation     │               │
│  Steno engine│                  │  Dictionary    │
│  LRU cache   │                  │  (~533KB)      │
└─────────────┘                  └──────────────┘
```

**Key design decisions:**

- Dictionary stored on peripheral → no flash pressure on central
- BLE GATT custom service for stroke→translation queries
- LRU cache on central side → reduces BLE round-trips for common words
- Orthographic rules run on central after dictionary lookup

## License

[PolyForm Noncommercial 1.0.0](LICENSE)
