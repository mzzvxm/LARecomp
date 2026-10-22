# Patches to the ReXGlue SDK

Changes LARecomp needs in the SDK itself. Apply from the SDK checkout, then rebuild and reinstall the SDK:

```powershell
git apply path\to\larecomp\patches\rexglue-vblank-resync.patch
```

## rexglue-vblank-resync.patch

Fixes the long-session frame rate collapse. Written against ReXGlue v0.10.0 (`f5337cdc947ff6d4c4196737e2c807a48f2a1fc2`); it touches one loop in `src/graphics/graphics_system.cpp`.

The vblank worker compared two unsigned guest tick counts. The guest tick count can step backwards when clock scaling re-bases, the difference then wraps to about 1.8e19, and the catch-up loop fires vblank interrupts without end. The patch takes the difference as signed, resynchronises when it is negative, and never replays more than a quarter second of missed vblanks.

With it applied, `--clock_no_scaling=true` is no longer needed. Analysis in [`documentation/TECHNICAL_NOTES.md`](../documentation/TECHNICAL_NOTES.md), section 4. The patch comes from [mcla-recomp](https://github.com/holdmysocks/mcla-recomp).
