# Contributing

## Fix Inclusion Guidelines

* Fixes must be real engine fixes, no workarounds **even if the result is the same as a native fix**
    * For example, `Stuck Underwater Fix` by `wSkeever` would be considered a workaround since it does not fix the original bug, instead it runs a continuous timer to patch the state at runtime. **The mod is still good and valuable, it's just out-of-scope for this project, mods like these will likely be recommended to be installed alongside OpenFixes**.
* "Extensions" or features that are not necessarily fixes are allowed if they do not change gameplay or make a design deicision
    * For example, `SSE Display Tweaks` by `slavicpotato1` has havok timing fixes, which was never technically a "bug" because the engine was always designed to work at maximum 60 fps. However, it would qualify as in-scope because it extends the engine in a way that does not influence gameplay.
* Some bug fix mods include gameplay/design changes as optional addons - those parts of the mods are not in-scope for this project.
    * For example, `Scrambled Bugs` by `Magicockerel`
* Fixes that require interacting with the GPU stack or injecting shaders are out-of-scope
    * These sorts of fixes can easily conflict with Community Shaders, those fixes should be implemented in Community Shaders instead.
* Code ported to this project must have the approval of their original authors regardless of their mod's license.
    * A different implementation of the same bug would be permitted.
* Fixes cannot have settings. All fixes will have just one single on/off toggle.
* Any fixes contributed must either inherit the project's `GPL-3.0` license or supply their own license that is compatible with `GPL-3.0`.
    * Should there be a case where an author allows their module to be included in OpenFixes but either has no license or a license that is not compatible with `GPL-3.0`, the author must provide a license grant and license their code under a `GPL-3.0` compatible license within this repository. See the `License Grant Template` section below. The license grant must apply to everyone, be irrevocable, and come from every copyright holder.
* Each fix should be placed under their relevant category folder (for example `src/Fixes/Crashes/<Fix Name>`). Each fix should have at most one `.cpp` source file and one `.h` header file for their fix, along with any required license files.
* Each fix should be only one fix. This may result in single mods being split across multiple modules at times.
* Never allocate trampoline in a module, this is done automatically. Use the `Hooks.h` helpers instead for all code that goes through the game.
* Shared/common code that is used across multiple fixes should be contributed globally to `Util.h`

## For Mod Authors

When a potential existing SKSE bug fix plugin is identified, its author(s) will be asked if the OpenFixes project can merge their mod into OpenFixes. The author has 3 general options (not literally, mainly in terms of how the port will be treated).

1. "I will contribute my mod, `[MOD NAME]` as a module in OpenFixes directly."
    1. You can also express an interest in being the primary maintainer of your module. This means OpenFixes maintainers will generally defer to you for modifications to your code. However, this is not a guarantee, and it can't be put in an enforceable license due to the parent OpenFixes (and CommonLib) license. Forks are also not guaranteed to follow these guidelines.
    1. You can provide your own license associated with your module, but it must be compliant with the project's GPL3 license. The common ones that are compliant are: `MIT`, `BSD`, `Apache-2.0`, `MPL-2.0`, or `GPL-3.0`.
1. "I give OpenFixes full permission to port my mod, `[MOD NAME]`, and any future versions of it, to OpenFixes and maintain it there. I will not be maintaining this port in OpenFixes"
    1. Additions to your module would always be welcome of course, but this route is more if you just want to give permissions but then not worry about doing any of the implementation.
    1. The license used will be your license from your original code for this module, assuming it is compatible with `GPL-3.0`.
1. "I do not permit the OpenFixes project to port my mod, `[MOD NAME]`."
    1. Note that different implementations of the same bug fix would still be allowed within the project so long as that implementation was made independently from your mod.

## License Grant Template

The following is a template for a license grant, to be used in a module subfolder with filename `PERMISSION.md`

```
# License grant

- Code covered: <repo URL> at commit <sha> (or: <mod> <version>, source archive from <Nexus URL>)
- License before the grant: <none | license name and SPDX>
- License granted: <license name> (<SPDX>), full text in <LICENSE [and EXCEPTIONS.md]>
- Not covered: <parts by other people and their licenses, or "nothing">

| Copyright holder | Account | Date | Where | Their words |
| --- | --- | --- | --- | --- |
| <name> | <Nexus/GitHub account> | <date> | <link to issue/comment, or "Nexus private message, copy kept by the maintainer"> | "<the grant, quoted exactly>" |
```

Header of `.cpp` file within the module:

```
// Original code: <license before the grant, or "no license">; licensed under <license granted> by <holder(s)>
// on <date>, see PERMISSION.md. Copyright (c) <year> <holder(s)>.
// Full texts in LICENSE and PERMISSION.md in this folder.
// SPDX-License-Identifier: <SPDX of the license granted>
```

## Fix Source Header

```
// <fix description>
//
// Ported from "<mod>" by <mod author> (<mod author username>), <source url>
// (commit <commit short hash>)
// Original code: <license>, Copyright (c) <year> <copyright holder>. Full text in LICENSE at the repository root.
// SPDX-License-Identifier: <license identifier>
```

## AI Policy

This project allows AI usage and uses AI for the following use cases (alongside Humans):

* Initial port of a mod
* Code Review (Each PR requires at least 1 human reviewer as well)
* Searching for potentially in-scope mods on Nexus via the Nexus API
* Coding assistance

Ultimately, the guideline is that you are responsible for any code AI generates. This means you should understand it fully before opening a pull request to contribute to this project.
