# tools — proofs, not demos

Each of these exists because a change here was too easy to get subtly
wrong to accept on inspection alone. They are meant to be run, and they
exit non-zero when they fail.

| | What it proves |
|---|---|
| `recip_proof.c` | The blur's multiply-and-shift replacement for integer division is **exact** — checked over every (window, sum) pair a blur can produce, all 4.2 million of them, for radius 1..128. Not a spot check. |
| `blur_equiv.c` | The rewritten blur matches the one it replaced pixel for pixel, on adversarial full-contrast noise across 9 radii × 8 region shapes including off-screen, 1×1 and 1-pixel-wide slivers. Also times both. |

```sh
cc -O2 -o /tmp/recip_proof tools/recip_proof.c && /tmp/recip_proof
cc -O2 -std=gnu11 -o /tmp/blur_equiv tools/blur_equiv.c src/aurshell/draw.c -lm && /tmp/blur_equiv
```

`blur_equiv` carries a verbatim copy of the *old* implementation, so it
keeps working as a reference after the original is gone from the tree.
It tolerates a difference of at most 2/255 per channel, which is what
reordering the separable passes costs in intermediate rounding; on real
frames the observed maximum is 1/255 on under 0.5% of subpixels.
