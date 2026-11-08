# Linux diagnostic stick for the ConnectX-3 Pro

A USB stick that boots Linux, runs the reference `mlx4` driver against the card,
collects what it reports, writes the findings back onto the stick, and reboots so
the machine returns to macOS. Nothing is installed anywhere, and nothing writes
to any disk other than the stick itself.

## Why this exists

On macOS the card reports its ownership word (`0x8069c`) and reset semaphore
(`0xf03fc`) as `0x01000000`. That is the state in which both FreeBSD's `mlx4` and
Mellanox's own driver refuse to drive the card, and neither a full power cycle nor
a real PCIe Function Level Reset clears it. `docs/M1-NOTES.md` sections 16 to 22
hold the measurements behind that statement.

One question cannot be answered from macOS: **does the reference Linux driver
drive this card?** If it does, the problem lives on the macOS side of the port and
the Linux driver becomes a worked example to read. If it refuses the card too,
then the card itself is the problem and no amount of driver work will help.

The same boot reads the firmware version and PSID with `mstflint`, which are the
prerequisites for ever being able to consider a firmware reflash.

## Building the stick

```sh
tools/cx3-mlx4/linux-diag/make-usb.sh                    # build, print the path
sudo tools/cx3-mlx4/linux-diag/make-usb.sh --write /dev/diskXsY
```

The builder downloads Alpine's standard ISO, verifies it against the published
SHA-256 before use, downloads the packages Alpine's own media does not carry,
builds the automation payload, and prints where it is.

`--write` only copies into a FAT32 partition named explicitly on the command
line. It does not repartition or erase anything.

## What happens when it boots

1. GRUB auto-boots Alpine from the stick (5 s menu, no input needed).
2. Alpine finds `cx3diag.apkovl.tar.gz` on the stick and applies it, which adds
   one boot service.
3. That service runs `cx3diag-run.sh`, which:
   - finds the stick again by its `CX3DIAG.MARK` file
   - identifies the Mellanox device and dumps its full PCI config space
   - records module state, then loads `mlx4_core` and `mlx4_en`
   - collects `dmesg`, the driver's verdict, IOMMU state and link state
   - runs `mstflint` and `mstconfig` before and after the driver binds
   - writes everything to `out/run-<timestamp>/` on the stick
   - syncs, writes `CX3DIAG.DONE`, and reboots

Progress is printed on screen throughout, so it is obvious whether it is working.

## Reading the result

Boot macOS, then open `out/run-*/` on the stick in Finder. Everything is plain
text:

| File | What it answers |
| --- | --- |
| `00-summary.txt` | kernel, boot command line, the device's BDF |
| `01-pci.txt` | `lspci -nnvvvxxxx`: identity, capabilities, full config space |
| `03-sysfs.txt` | raw config space plus link and resource state |
| `05-module-files.txt` | whether the `mlx4` modules are present at all |
| `06-mstflint-pre.txt` | firmware version and PSID, card not yet claimed |
| `10-modprobe.txt` | the module load attempt |
| `12-dmesg-mlx.txt` | the driver's verdict: the key file |
| `13-dmesg-errors.txt` | errors, IOMMU notes, anything that failed |
| `16-mstflint-post.txt` | firmware identity with a driver bound |

The decisive line is in `12-dmesg-mlx.txt`: either the driver takes the card and
creates a network interface, or it prints the same refusal the source documents
(`Multiple PFs not yet supported - Skipping PF`).

## Three details that are load-bearing

Easy to get wrong, and each one silently breaks the automation:

1. **`etc/.default_boot_services` must be present in the apkovl.** Alpine adds its
   default boot services — including `modloop`, which provides the kernel modules
   — only when either that marker exists or no apkovl is supplied. An apkovl
   without it means `mlx4` never appears, and the run would report a false
   negative.
2. **The apkovl is discovered automatically, not passed on the command line.**
   `nlplug-findfs` scans the boot media for `*.apkovl.tar.gz`. Passing a bare
   filename as `apkovl=` would fail, because init tests it relative to `/`.
3. **`lspci` and `mstflint` have to be bundled.** Alpine's standard media ships
   neither `pciutils` nor `mstflint`, and the live system has no network, so both
   are unpacked into the apkovl together with the two runtimes `mstflint` needs
   (`libstdc++`, `libgcc`), which the media also lacks.
