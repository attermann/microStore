## Problem

<!-- What is broken or missing? Who is affected? How can it be reproduced? -->

## Solution

<!-- How does this change solve it, why this approach, any tradeoffs or limitations? -->

## Compatibility

- [ ] No change to the on-disk format (or: a compatibility/migration path is described below and added to CHANGELOG)
- [ ] Crash safety unchanged (or: power-loss behavior at each step is explained below)
- [ ] No breaking public API change (or: described below and added to CHANGELOG)
- [ ] Still header-only, C++14, no new dependencies
- [ ] No board- or variant-specific code; any platform-specific code is unavoidable, isolated (adapter or guard), and explained below

## Testing

Platforms tested (and how):

- [ ] native (`pio test -e native`)
- [ ] ESP32 (board / filesystem: )
- [ ] nRF52 (board / filesystem: )

Platforms not tested, and why:

## Docs

- [ ] README / `docs/` / CHANGELOG updated where user-visible behavior, build flags, configuration macros or APIs changed

<!-- Please read CONTRIBUTING.md and .github/PR_POLICY.md before submitting. One logical concern per PR. -->
