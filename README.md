# zmk-steno-engine

Clean-room stenography engine for [ZMK Firmware](https://zmk.dev). Dictionary-based lookup with multi-stroke support, optimized for nRF52840 flash constraints.

**Status:** Early development — basic single/multi-stroke lookup works, formatter not yet implemented.

## Features

- Standard 23-key steno layout
- Sorted-array dictionary with binary search lookup
- Multi-stroke support with configurable timeout
- All-up chord detection
- HID keyboard output (ASCII)
- Build-time dictionary compilation from Plover JSON
- Test dictionary included for development

## Building

Add as a ZMK module in your `west.yml`:

```yaml
manifest:
  remotes:
    - name: zmk-steno-engine
      url-base: https://github.com/afiqzudinhadi
  projects:
    - name: zmk-steno-engine
      remote: zmk-steno-engine
      revision: optimize-dict
```

Enable in your `.conf`:

```
CONFIG_STENO_ENGINE=y
```

Use in your keymap:

```dts
#include <dt-bindings/zmk/steno_keys.h>

/ {
    keymap {
        steno_layer {
            bindings = <
                &steno STENO_SL  &steno STENO_TL  &steno STENO_PL  ...
            >;
        };
    };
};
```

## Dictionary Compiler

```bash
# Compile test dictionary
python3 tools/compile_simple.py dicts/test.json -o steno_dict.bin --stats

# Compile Plover dictionary (trimmed)
python3 tools/compile_simple.py plover-main.json -o steno_dict.bin --max-entries 120000 --stats
```

## License

[PolyForm Noncommercial 1.0.0](LICENSE)
