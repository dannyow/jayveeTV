# cad

Printable parts for the case: a pyramid base and a head on a friction hinge, in the spirit
of the JVC 3100R Video Capsule. Exported from Fusion; each file sits on the bed (z = 0).
The same files, with photos, are [on Printables](https://www.printables.com/model/1862118-jayveetv-retro-tv-case-for-the-waveshare-esp32-c6).

| file | colour | print | notes |
|---|---|---|---|
| `jayveetv-white-base-head.stl` | white | 1 | base and head together, **print in place**: the hinge comes off the bed working. Two bodies in one file: do not separate them |
| `jayveetv-black-head-lid.stl` | black | 1 | outer face on the bed, clips up, as exported; no supports |
| `jayveetv-black-base-lid.stl` | black | 1 | outer face on the bed, as exported; the undercut still needs **supports** from the slicer |
| `jayveetv-black-button.stl` | black | 3 | flange down, stem up, as exported (the chamfer under the flange is there for this orientation); no supports |

No supports are modelled in: the base lid needs them from the slicer; nothing else does.

The hinge is deliberately tight (0.2–0.25 mm between head and base along the hinge arm,
down to ~0.1 mm in places) so the head holds any angle.

## Also needed

- The module: Waveshare ESP32-C6-Touch-AMOLED-2.16 (see the main README).
- A USB-C power pigtail pair: a **male** USB-C plug with ~70 mm leads (into the module)
  and a **female** 2-pin USB-C panel socket with ~100 mm leads (in the base), 24 AWG.
  These are **power only**: flash the firmware over the module's own USB-C before
  assembly (or with the head lid off).

## Assembly

Nothing else is needed: no screws, no glue.

1. **Flash the module first** (main README, Install): the pigtails carry power only.
2. **Socket:** press the female USB-C socket into its seat at the back of the white base.
3. **Wires:** run the leads through the channel in the hinge arm and join them colour to
   colour, red to red and black to black. Solder them, or twist them together, and
   insulate the joints (heat-shrink tube, or tape).
4. **Buttons:** drop the three buttons into the head from the inside; their flange keeps
   them in.
5. **Module:** plug the male USB-C into the module and seat the module in the head,
   screen out.
6. **Lids:** press the head lid and the base lid on.

Licence: **CC BY-SA 4.0**, full terms and attribution wording in
[`LICENSE.md`](LICENSE.md).
