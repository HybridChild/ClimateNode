# Understanding the Zephyr Build System

*A from-first-principles guide to how a Zephyr application actually gets built.*

This is a teaching document, not project documentation. It explains the concepts, components, and flow of the Zephyr build system in the order that makes them easiest to learn. It uses this repository — a CO₂ sensor node on an ST Nucleo-H753ZI — only as a running example to keep the ideas concrete. For the project-specific details (exact file paths, verified line numbers, the artifact-by-artifact table), see its companion, [`build-system-overview.md`](../docs/build-system-overview.md).

**The shape of this document:**

- **§1–§3** — why the build feels strange, the two questions it answers, and the three tools that answer them.
- **§4–§6** — devicetree (the hardware facts), Kconfig (the software policy), and the bridge that lets the first decide the second. §6 is the heart of the system.
- **§7–§8** — the build start to finish, and how your own generators hook into it.
- **§9** — what changes when one repo holds more than one application, and how two of them share code.
- **§10** — a worked example, following one sensor and one schema all the way through.
- **§11–§12** — daily habits, and exercises that make the invisible pipeline visible.
- **§13–§14** — the whole model in a paragraph, and where to go next.

---

## 1. Why the Zephyr build feels strange at first

If you come from application programming, "building" means: take my source files, compile them, link them, done. A Zephyr build is not that. Or rather, *that* is only the last and least interesting step.

The reason is that Zephyr targets *thousands* of different circuit boards and microcontrollers from one shared source tree, and it lets you compile in or leave out hundreds of independent features. Before a single line of your `main.cpp` can be compiled, the build system has to answer two questions that an application programmer never thinks about:

1. **What hardware am I running on, and how is it wired?** (Which UART is the console? Where is the I²C controller? What clock feeds it?)
2. **Which of Zephyr's features do I want compiled into this particular image?** (Networking? The sensor subsystem? Which drivers?)

Neither answer lives in your C code. Both are *resolved by the build system* and then handed to your code as generated headers. So the mental model you need is not "compiler" — it is **"a configuration system that generates code, which then gets compiled."** Once you internalise that, everything else falls into place.

---

## 2. The one big idea: two questions, two systems, resolved before boot

Zephyr answers those two questions with two entirely separate machineries, and the single most useful thing you can hold in your head is which is which:

| The question | The system that answers it | Its nature |
| --- | --- | --- |
| *What hardware exists, and how is it wired?* | **Devicetree** | **Facts** — dictated by the physical board. Ideally data, not code. |
| *Which software features do I compile in?* | **Kconfig** | **Policy** — your choice, and it can differ per application. |

Keep these apart in your mind and you have already understood most of the system. Almost every file you will touch, and every generated artifact you will see, belongs to one side or the other. The build's central job is to **reconcile** the two — to take "the hardware has an I²C-attached CO₂ sensor" (a devicetree fact) and "yes, compile the sensor subsystem" (a Kconfig policy) and turn them into a working driver bound to a real bus.

There is a second idea, just as important: **Zephyr resolves everything it possibly can at compile time.** On a big operating system, a driver might scan a bus at runtime and discover devices. Zephyr does the opposite — by the time `main()` runs, your sensor is *already* a fully-formed `struct device` in memory, with its bus and address baked in by macros that were expanded during the build. "Describe the hardware as data; resolve before boot" is the philosophy behind the whole design. Nearly every arrow in this document happens on your workstation during the build, not on the microcontroller.

---

## 3. The three tools: who does what

Three programs cooperate to produce firmware, and they sit in a strict hierarchy. Confusing their roles is the most common source of "wait, what actually does the building?"

```
west   — orchestrates: manages the source tree and launches the build   (Python)
cmake  — configures:   answers the two questions, plans the build
ninja  — executes:     runs the planned compile and link commands
```

**West** is a workspace manager, not a build tool. Zephyr is not one repository — it is the core `zephyr` tree plus dozens of separate modules (hardware abstraction layers, crypto libraries, `nanopb`, and so on). West's original job is to clone and version-pin all of them from a manifest (`west init`, `west update`). It *also* offers `west build` as a convenience, but that command is a thin wrapper: it works out the board and paths, then shells out to CMake and Ninja. West itself compiles nothing.

**CMake** is where the real intelligence lives. When you build a normal C project, CMake's job is modest — find some libraries, generate a Makefile. In Zephyr, CMake runs the entire configuration process described in this document: it resolves the board, parses the devicetree, runs Kconfig, and only then plans the compilation. Its *output* is not object files; it is a complete, static list of every command needed to build the firmware.

**Ninja** is deliberately dumb and fast. It reads the command list CMake produced (`build.ninja`) and executes it, rebuilding only what changed. It makes no decisions.

A useful slogan: **CMake thinks, Ninja executes.** This split has a practical consequence you will feel daily. Editing a `.c` file changes nothing about the *configuration*, so only Ninja needs to re-run — fast. But editing your devicetree overlay, your `prj.conf`, or a Kconfig option changes the configuration itself, so CMake must run again to regenerate everything downstream. Usually the build detects this automatically; when generated state gets into a confusing stale condition, a *pristine* build (`west build -p`) wipes the output directory and starts the configuration from scratch.

---

## 4. Devicetree: describing the hardware

### 4.1 What it is

Devicetree is a small, declarative language for describing hardware. Zephyr borrowed it from the Linux kernel, where it solved exactly the same problem: one kernel binary that runs on many boards, each described by a data file instead of by board-specific C code.

A devicetree is a hierarchy of **nodes**, each with **properties**. A node describes one piece of hardware — a UART, an I²C controller, a sensor — and its properties describe that hardware's parameters:

```dts
&i2c1 {                          /* an I²C controller that already exists in the SoC */
    status = "okay";             /* this peripheral is present and enabled */
    clock-frequency = <400000>;
};
```

Two properties carry special weight and deserve immediate attention:

- **`status`** is either `"okay"` (this hardware is present and should be used) or `"disabled"` (it exists on the chip but is not wired up on this board). A vast number of build problems come down to a node not being `"okay"`.
- **`compatible`** is a string naming *what kind of device this is*, e.g. `"sensirion,scd40"`. It is the single most important concept in devicetree, and we return to it below.

### 4.2 Layering: SoC, board, and your overlay

You never write a devicetree from scratch. It is assembled in layers, each adding to or overriding the one beneath:

1. **The SoC include** (`.dtsi`) ships with Zephyr and declares every peripheral the chip *has* — on the STM32H753, that is every I²C, SPI, UART, the Ethernet MAC, and so on — most of them `disabled` by default.
2. **The board file** (`.dts`) is written by whoever ported the board. It takes the SoC's peripherals and turns `"okay"` the ones this board actually wires out, assigning the physical pins. On the Nucleo-H753ZI, that includes enabling `&i2c1` on pins PB8/PB9 and the Ethernet MAC with its PHY.
3. **Your overlay** is where *you* customise, without touching the tree. This is a crucial point about how you are meant to work: **the SoC and board files live in the read-only Zephyr workspace and you do not edit them.** Instead you supply a small overlay that is merged on top.

In this project, the sensor is not part of the board — it is a breakout we wired on. So the overlay grafts a new child node onto the existing I²C controller:

```dts
&i2c1 {                          /* reference the board's existing controller… */
    scd40: scd40@62 {            /* …and add our sensor as a child of it */
        compatible = "sensirion,scd40";
        reg = <0x62>;            /* the sensor's address on the I²C bus */
        status = "okay";
    };
};
```

The `@62` in the node name and `reg = <0x62>` are the device's I²C address. Making the sensor a *child* of the I²C node is how devicetree expresses "this device sits on that bus" — a relationship the driver will read back later.

### 4.3 `compatible` and bindings: the linchpin

Return to `compatible`, because it does two independent jobs, and understanding both unlocks how hardware descriptions connect to actual code.

Every `compatible` string points to a **binding** — a YAML schema file in the Zephyr tree (here, `dts/bindings/sensor/sensirion,scd40.yaml`). The binding declares which properties a node of that kind may have and what they mean. This is the first job: **validation.** When the tree is parsed, each node is checked against its binding; a typo'd property or a missing required one is an error, caught on your workstation, not in the field.

The second job is **driver binding**. A Zephyr driver announces which `compatible` it serves:

```c
#define DT_DRV_COMPAT sensirion_scd40      /* note: the comma becomes an underscore */
```

and then a macro walks the tree and instantiates that driver *once for every enabled node* with that compatible. One string, matched from two directions — schema on one side, driver on the other. Get it wrong and the build fails immediately; it never silently misbehaves at runtime.

### 4.4 How the devicetree becomes something code can use

Here is where people are often misled by the name. There is a classic tool called `dtc`, the "devicetree compiler," and you might assume Zephyr uses it to process the tree. **It does not** — not as the real processor. Zephyr parses the devicetree with its own Python library (`edtlib`) and runs `dtc`, if it is even installed, only as an optional linter to catch extra warnings.

The real pipeline is three Python scripts, and the artifact at its centre is a parsed model of the tree saved as `edt.pickle`:

```
board .dts ┐
SoC  .dtsi ├─► cpp ─► (merged source) ─► gen_edt.py ─► edt.pickle ─► gen_defines.py ─► devicetree_generated.h
overlay    ┘                                 ▲
bindings ────────────────────────────────────┘
```

Step by step:

- **`cpp`** — the C preprocessor — runs first, expanding the `#include`s and macros in the devicetree source into one flat text file. (Yes, the same preprocessor used for C; the `.dts` format deliberately reuses it.)
- **`gen_edt.py`** reads that flattened source *plus the bindings* and produces **`edt.pickle`**, the authoritative in-memory model of your hardware. It also writes a human-readable `zephyr.dts` — the fully-merged tree, invaluable for answering "did my overlay actually take effect?"
- **`gen_defines.py`** turns `edt.pickle` into **`devicetree_generated.h`**, a very large header full of `#define`s describing every node.

Your code never reads that header directly. Instead it uses the friendly `DT_*` macros from `<zephyr/devicetree.h>`, which expand down to those generated defines. `DT_NODELABEL(scd40)` finds the node you labelled; `DEVICE_DT_GET(...)` turns it into a pointer to the driver instance; `I2C_DT_SPEC_INST_GET(...)` reads a node's parent bus and address. All of it resolves at compile time — the devicetree exists only during the build.

---

## 5. Kconfig: choosing the software

### 5.1 What it is

Kconfig is the other half, and it too comes straight from the Linux kernel. Where devicetree describes hardware, Kconfig selects **software features**. Every configurable piece of Zephyr — a subsystem, a driver, a library, a buffer size — is a Kconfig **symbol**, conventionally referred to in C as `CONFIG_SOMETHING`.

Symbols have types (boolean, integer, string), **defaults**, and — importantly — **dependencies** on one another. A driver can declare that it `depends on` a bus being enabled, or that enabling it should `select` (force on) a library it needs. This dependency graph is what lets you say "I want the sensor subsystem" and have the I²C driver and the CRC library pulled in automatically.

### 5.2 Where the answers come from

The final configuration is layered, much like the devicetree, with later layers overriding earlier ones:

- The **Kconfig files in the tree** define every symbol, its default, and its dependencies.
- The **board's `defconfig`** sets baseline choices appropriate to the board.
- Your **`prj.conf`** — in your application — sets the choices for *this* build. This is the file you edit to turn features on:

  ```conf
  CONFIG_SENSOR=y     # compile the sensor subsystem
  CONFIG_I2C=y        # compile the I²C drivers
  ```

Kconfig reconciles all of these into one resolved answer set: **`.config`**, a flat list of every symbol's final value. From `.config` it mechanically generates **`autoconf.h`**, the same information as C macros (`CONFIG_SENSOR=y` becomes `#define CONFIG_SENSOR 1`).

### 5.3 How the configuration reaches your code — two routes

The configuration influences the build in two distinct ways, and it is worth separating them:

1. **It selects which source files are compiled at all.** CMake reads `.config`, and Zephyr's build glue adds a driver's `.c` files to the build *only if* its `CONFIG_` symbol is set. The SCD4x driver is compiled because `CONFIG_SCD4X` is `y`; on a build where it is not, the file is simply never handed to the compiler.
2. **It parametrises the code that is compiled.** `autoconf.h` is *force-included* into every single translation unit (via the compiler's `-imacros` flag). That is why any `.c` file can write `#ifdef CONFIG_SOMETHING` without an `#include` — the definitions are always already there. This is how one source file compiles differently for different configurations.

---

## 6. The bridge: how enabling hardware turns on software

We now have the two halves. The most elegant part of the whole system is how they connect — how the mere presence of a sensor in your devicetree causes its driver to be compiled, without you ever setting the driver's Kconfig option by hand.

The coupling is a clean division of labour between *declaring* a configuration symbol and *deciding its value*:

- **Declaration comes from the bindings, and is board-independent.** A script (`gen_driver_kconfig_dts.py`) scans *every binding in the tree* and emits, for each possible `compatible`, a Kconfig symbol named `DT_HAS_<COMPATIBLE>_ENABLED`. This produces a generated Kconfig file that declares thousands of such symbols — for every kind of hardware Zephyr knows about, whether or not you have it.
- **The value comes from your actual tree.** Each of those symbols is defined as:

  ```
  config DT_HAS_SENSIRION_SCD40_ENABLED
      def_bool $(dt_compat_enabled,sensirion,scd40)
  ```

  where `dt_compat_enabled` is a Kconfig helper function that, during configuration, reads **`edt.pickle`** and returns true only if some `"okay"` node in *your* tree has that compatible.

So `edt.pickle` — the parsed devicetree — is consulted twice: once by `gen_defines.py` to make the C macros, and once here, by Kconfig, to decide these `DT_HAS_*` values. The bindings say which symbols *can* exist; your hardware says which are *true*.

The payoff is the driver's own Kconfig entry:

```
config SCD4X
    default y
    depends on DT_HAS_SENSIRION_SCD40_ENABLED || DT_HAS_SENSIRION_SCD41_ENABLED
    select I2C
    select CRC
```

Because your overlay put an `"okay"` `sensirion,scd40` node in the tree, `DT_HAS_SENSIRION_SCD40_ENABLED` became true, so `CONFIG_SCD4X` defaulted to `y` — and it `select`ed the I²C and CRC libraries it needs. You enabled a driver, and pulled in its dependencies, purely by describing the hardware. That is the devicetree→Kconfig bridge, and it is the heart of Zephyr's configuration model.

---

## 7. The whole build, start to finish

With the concepts in hand, here is the actual sequence when you run `west build`. Notice that configuration (everything CMake does) comes first, and compilation is the very last act.

1. **West** works out the board and paths and invokes **CMake**.
2. Your application's `CMakeLists.txt` calls `find_package(Zephyr)`. This is the seam where your project hands control to Zephyr's build system. It must come *before* the CMake `project()` call, because Zephyr needs to install its cross-compiler before CMake probes the compiler — otherwise CMake would test your host `gcc` instead of the ARM one.
3. `find_package(Zephyr)` pulls in Zephyr's ordered list of build modules. The order is a dependency chain, and one ordering fact matters most: **the devicetree stage runs before the Kconfig stage**, precisely because Kconfig needs `edt.pickle` (via the bridge of §6). Roughly: resolve the board → **devicetree** → **Kconfig** → select the CPU architecture and SoC support → finally the kernel module, which defines the `app` build target that your source attaches to.
4. Along the way, all the generated files we have discussed are written into the build directory: `edt.pickle`, `devicetree_generated.h`, the generated Kconfig, `.config`, `autoconf.h`.
5. Only now does CMake plan the compilation — using `.config` to decide which sources are in — and emit `build.ninja`.
6. **Ninja** runs the compile and link commands, and you get a firmware image.

A detail from step 3 that surprises newcomers: your `CMakeLists.txt` says `target_sources(app PRIVATE src/main.cpp)`, yet you never created an `app` target. Zephyr's kernel module created it for you. You are not defining an executable; you are contributing your source into Zephyr's pre-existing application target, which is then linked against the kernel.

---

## 8. Generating your own sources

Step 5 of §7 said CMake decides which sources are in the build. It can also decide to *create* some first — and this is the one place where the machinery described so far stops being purely Zephyr's and becomes something your project extends. Everything generated up to now (`devicetree_generated.h`, `autoconf.h`, the driver Kconfig) came from Zephyr's own scripts. This is your own generator, hooked into the same pipeline.

This repo needs C structs for its wire format, generated from a `.proto` schema, so `gateway/CMakeLists.txt` adds one line before its `target_sources`:

```cmake
list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)

zephyr_nanopb_sources(app ${CMAKE_CURRENT_SOURCE_DIR}/../proto/node.proto)

target_sources(app PRIVATE src/main.cpp src/sensor.cpp src/relay.cpp)
```

`zephyr_nanopb_sources()` registers a *build rule*: run the generator on `node.proto`, put `node.pb.c`/`node.pb.h` in the build directory, add the `.c` to the `app` target, and add the directory to the include path. Ninja then treats the generated `.c` like any other source, and re-runs the generator whenever the `.proto` changes. **Note which target all of that attaches to** — `app`, not "the project." §9 is about why that one word decides how two applications can share code.

Two things generalise from this:

- **Generated code belongs in `build/`, never in the repo.** It is an artifact, subject to the same rule as `devicetree_generated.h` and `autoconf.h` — change the input and rebuild, never edit the output. Here that is what makes "the `.proto` is the single source of truth" a mechanical guarantee: there is no state in which the firmware builds against a stale schema, because the schema is compiled on every build.
- **`nanopb` is a Zephyr module**, so its CMake lives in the workspace (`${ZEPHYR_BASE}/modules/nanopb`) rather than in this repo. That is the standard shape: west fetches modules, and each contributes CMake and Kconfig the app can opt into.

---

## 9. One repo, two applications

Everything so far has assumed one application. Plenty of real projects are not: an application plus a bootloader, a main MCU plus a companion, a shipped image plus a bench variant. This repo is the two-MCU shape — a Cortex-M7 gateway and a Cortex-M0 peer node, in `gateway/` and `peer-node/`, sharing four headers and two translation units out of a third directory, `shared/`.

Zephyr offers four different mechanisms for that, and the interesting part is that the plainest one wins here for a reason you can point at.

### 9.1 What the `app` target actually is

Step 5 of §7 said `include(kernel)` is what creates the `app` target. It is worth being precise about what it creates: **an ordinary CMake library target.** Not a Zephyr abstraction, not a directory, not a manifest — a target, of the same kind you would make yourself with `add_library()`.

That is why `target_sources(app PRIVATE …)` is plain CMake with no Zephyr magic in it, and why it will accept **any path**, including one that leaves the application directory entirely:

```cmake
set(SHARED ${CMAKE_CURRENT_SOURCE_DIR}/../shared)

target_sources(app PRIVATE src/main.cpp src/sensor.cpp src/relay.cpp
               ${SHARED}/protocol.cpp ${SHARED}/commands.cpp)
target_include_directories(app PRIVATE ${SHARED})
```

The object files are the giveaway that nothing clever is happening. CMake names an object after its source path, and for a source from outside the project it mirrors the whole absolute path underneath the target's directory:

```
gateway/build/CMakeFiles/app.dir/src/main.cpp.obj
gateway/build/CMakeFiles/app.dir/Users/.../shared/protocol.cpp.obj
```

Read that second line as what it is: `protocol.cpp` was compiled **into this application's own target**, not linked in from a library built somewhere else. Build the other app and it happens again, separately, in that app's build tree. One source file, two object files, and no shared binary anywhere — which is the only arrangement that *could* work here, since the two objects are for different instruction sets.

### 9.2 Four mechanisms, and what each one buys

| Mechanism | What it is | Buys you | Costs |
|---|---|---|---|
| **`target_sources` with an outside path** | plain CMake, as above | nothing to set up; the file is simply part of each app | no isolation — same flags, no Kconfig of its own |
| **`zephyr_library()`** | a separate CMake library, linked into the image | its own compile options, and a Kconfig symbol that can switch it per app | a *different target*, so include paths and generated headers must be plumbed to it by hand |
| **A Zephyr module** | a directory with `zephyr/module.yml`, fetched by west | shares across *repos*, versioned on its own, contributes its own `Kconfig` and DT bindings | a second repo and a manifest entry to maintain |
| **sysbuild** | builds *several images* in one invocation, with `SB_CONFIG_*` above them | app + MCUboot, or two cores, configured together | a whole extra configuration layer |

The last one is a different axis and worth separating out, because the name suggests otherwise: **sysbuild is not about sharing source, it is about building more than one image at once.** This repo builds one app at a time on purpose — the two boards are flashed independently, and `scripts/build.sh -a <app>` is the whole of its multi-app story.

Note also that you have already met mechanism three. **nanopb is a Zephyr module** (§8) — that is exactly what `${ZEPHYR_BASE}/modules/nanopb` is. So the module system is not exotic; it is what the thing you are calling into is built as.

### 9.3 Why the plainest mechanism wins here

A `zephyr_library` looks like the tidier choice, and it is the wrong one, for a concrete reason: **a generated header is a property of a target, not of a directory.**

`zephyr_nanopb_sources(app …)` attaches both the generated `node.pb.c` *and* its include directory to `app`. `shared/protocol.h` opens with `#include <node.pb.h>`. Move `protocol.cpp` into a `zephyr_library` and that library is a different target — it does not inherit `app`'s include directories, so the generated header is simply not found. You can fix it, by generating per-library or exporting the include directory across targets, but you would be buying plumbing with no benefit attached: both apps want the same flags for these files, and neither needs a Kconfig switch to turn them off.

The corollary is that **each app runs the generator for itself.** Two build trees, two `node.pb.c`, one `.proto`. That is not duplication worth removing — it is §8's discipline applied twice, and it is what makes it impossible for one app to be built against a stale copy of the schema.

### 9.4 The one thing the build system will not check for you

CMake is perfectly happy to let application A reach into application B's directory for a file. It compiles, and nothing warns you. What it costs is not mechanical but architectural: the dependency graph now says *A depends on B*, when the truth is that both depend on a contract neither one owns.

The rule that avoids it is simple enough to apply without thinking: **a file's home is decided by how many applications link it.** Two means `shared/`. One means that app's own `src/`. And nothing in `shared/` may include anything from an application.

That last clause is the only one with any teeth, and it has more than you would expect. Look at the include paths an app actually compiles with — `shared/` is on them, and **the app's own `src/` is not on them at all.** Its own sources reach their neighbours only through the C preprocessor's rule that a quoted `#include` is searched for first in the directory of the file doing the including. Which means a header in `shared/` cannot reach `gateway/src/relay.h` even when the *gateway* is what is being built: the search starts in `shared/`, where the including file lives, and `gateway/src` is on no `-I` flag anywhere. §12's last exercise does this on purpose and reads the error.

So the layout is not merely a convention that a reviewer has to defend. In the direction that matters, it is enforced by include paths — and the third consumer, `tests/`, is the payoff: a suite that compiles `shared/protocol.cpp` is testing the same file both boards run, not a copy of it.

---

## 10. Worked example: from a node in the tree to bytes on the wire

Let us follow this project's sensor all the way through, because it exercises every concept above. The theme to watch for is the **compile-time / runtime boundary** — almost everything happens during the build.

1. **You describe the hardware.** Your overlay adds the `scd40@62` node under `&i2c1` (§4.2). That is the only hardware fact you supply.
2. **The tree is parsed.** `cpp` flattens the sources, `gen_edt.py` merges your overlay in and records the node in `edt.pickle`, validating it against the `sensirion,scd40` binding.
3. **C macros are generated.** `gen_defines.py` writes defines describing the node — crucially, that its parent bus is the I²C controller and its address is `0x62`.
4. **The bridge fires.** The generated Kconfig declares `DT_HAS_SENSIRION_SCD40_ENABLED`, and `dt_compat_enabled` reads `edt.pickle` and makes it true.
5. **Kconfig resolves.** `CONFIG_SCD4X` defaults `y`; it selects `CONFIG_I2C` and `CONFIG_CRC`. These land in `.config` and `autoconf.h`.
6. **The driver is compiled.** Because `CONFIG_SCD4X` is set, CMake includes the driver's `scd4x.c` in the build.
7. **The driver instantiates itself for your node.** Its `DT_DRV_COMPAT` is `sensirion_scd40`, and a for-each macro creates exactly one device instance — the one you declared. Its configuration captures the bus and address by reading the generated macros from step 3 (`I2C_DT_SPEC_INST_GET`). A `struct device` now exists, with a table of function pointers for "fetch a sample" and "read a channel."
8. **Your application gets a handle.** In `sensor.cpp`, `DEVICE_DT_GET(DT_NODELABEL(scd40))` resolves — at compile time — to a pointer to that exact device. If the node did not exist, this would not compile.

Everything to this point happened during the build. Only the final step is runtime:

9. **You read the sensor.** `sensor_sample_fetch()` calls through the device's function-pointer table into the driver, which finally performs a real I²C transaction on the bus — the only part of the whole story that touches hardware.

The arrow from "`&i2c1`" to "the SCD40 driver talking to address `0x62`" was drawn entirely by macros during compilation. By the time the firmware runs, there is nothing to discover.

### The same trip for a source you generate

The sensor path is Zephyr's own machinery. Run the §8 generator alongside it and the two are the same shape — which is the point of the module system:

1. **You describe the contract.** `proto/node.proto` is the input, and it lives outside `gateway/` because the host tooling generates from it too.
2. **CMake registers a rule**, not an output. `zephyr_nanopb_sources(app …)` tells the build *how* to produce `node.pb.c`/`node.pb.h` and that the `.c` belongs to `app`. Nothing has been generated yet — this is still the "CMake thinks" half of §3.
3. **Ninja runs the generator** when it notices `node.proto` is newer than its outputs, exactly as it would re-run a compiler. The outputs land in `gateway/build/`.
4. **The generated `.c` compiles like any other source** — as **C**, even though the two app sources beside it are C++, because that is the language it was written in:

   ```
   [16/335] Building C object CMakeFiles/app.dir/node.pb.c.obj
   [26/335] Building CXX object CMakeFiles/app.dir/src/protocol.cpp.obj
   [30/335] Building CXX object CMakeFiles/app.dir/src/sensor.cpp.obj
   ```

5. **Your code includes the generated header** — `#include <node.pb.h>` resolves because the rule added the build directory to the include path.

Same discipline as steps 2–3 above: the input is versioned, the output is not, and the only way to change the output is to change the input and rebuild. §11 has you prove that by touching the file and watching what ninja does.

---

## 11. Working with the system day to day

A few practical habits follow directly from the model:

- **Customise with overlays and `prj.conf`, never by editing the tree.** The board and SoC files are shared, read-only infrastructure. Your hardware additions go in an overlay; your feature choices go in `prj.conf`. This is why the system is built the way it is.
- **When a device isn't working, read the generated `zephyr.dts` first.** It is the fully merged tree. If your node isn't there, or isn't `"okay"`, your overlay didn't take — a build/config problem, before any driver code is even involved.
- **When a feature isn't compiled, read `.config`.** If `CONFIG_YOURTHING` isn't `y`, the code was never built. Check its `depends on` — often an unmet devicetree or Kconfig dependency is silently keeping it off.
- **Reach for a pristine build (`-p`) after devicetree or Kconfig changes** if results look stale — those changes ripple through generated files, and a clean regenerate removes doubt.
- **Never edit anything under the build directory.** It is all generated and will be overwritten. Change the *inputs* and rebuild.

---

## 12. Exercising the build system

Everything above is claims about a pipeline you cannot see. All six exercises below make one part of it visible, and none of them needs the board — only the Mac and a build:

```sh
./scripts/build.sh          # incremental; -p forces pristine
```

### Exercise 1 — Prove your overlay actually applied

*Demonstrates §4.4: `zephyr.dts` is the fully-merged tree.*

```sh
grep -A6 'scd40@62' gateway/build/zephyr/zephyr.dts
```

```
/* node '/soc/i2c@40005400/scd40@62' defined in .../gateway/boards/nucleo_h753zi.overlay:4 */
scd40: scd40@62 {
        compatible = "sensirion,scd40"; /* in .../nucleo_h753zi.overlay:5 */
        reg = < 0x62 >;                 /* in .../nucleo_h753zi.overlay:6 */
        status = "okay";                /* in .../nucleo_h753zi.overlay:7 */
        zephyr,deferred-init;           /* in .../nucleo_h753zi.overlay:16 */
};
```

Read the path in the first line: your node was grafted under `/soc/i2c@40005400`, the board's own I²C1 controller, which you never declared. Every property is annotated with **the file and line it came from** — so when three layers disagree, this file tells you which one won.

**Proves:** the overlay merged into the board and SoC layers of §4.2, and the parent/child relationship that will hand the driver its bus is real. This is the first thing to check when a device does not come up, *before* suspecting any driver code.

### Exercise 2 — Watch the devicetree→Kconfig bridge fire

*Demonstrates §6, the most elegant part of the system and the least visible.*

You never wrote `CONFIG_SCD4X` anywhere — confirm that first, then look at what the build decided:

```sh
grep -rn 'CONFIG_SCD4X' gateway/prj.conf           # no matches: you never asked for it
grep -n 'SENSIRION_SCD40\|CONFIG_SCD4X\|CONFIG_CRC=\|CONFIG_I2C=' gateway/build/zephyr/.config
```

```
CONFIG_DT_HAS_SENSIRION_SCD40_ENABLED=y
CONFIG_I2C=y
CONFIG_SCD4X=y
CONFIG_CRC=y
```

Four symbols, none of them requested. The first is `dt_compat_enabled` having read `edt.pickle` and found an `"okay"` node with that compatible; the second and fourth are the `select`s the driver's Kconfig pulled in behind it.

**Proves:** describing hardware turned on software. Delete the node from the overlay, rebuild pristine, and all four go away — which is also the answer to "why is my driver not being compiled?"

### Exercise 3 — Ninja rebuilds only what changed

*Demonstrates §3 (CMake thinks, Ninja executes) and §8 (the generator is just another build rule).*

Touch the schema — change nothing in it — and rebuild:

```sh
touch proto/node.proto
./scripts/build.sh
```

```
[1/12] Running C++ protocol buffer compiler using nanopb plugin on .../proto/node.proto
[2/12] Building C object CMakeFiles/app.dir/node.pb.c.obj
[3/12] Building CXX object CMakeFiles/app.dir/.../shared/protocol.cpp.obj
[4/12] Building CXX object CMakeFiles/app.dir/.../shared/commands.cpp.obj
[5/12] Building CXX object CMakeFiles/app.dir/src/relay.cpp.obj
[6/12] Building CXX object CMakeFiles/app.dir/src/main.cpp.obj
[7/12] Linking CXX static library app/libapp.a
...
[12/12] Linking CXX executable zephyr/zephyr.elf
```

Twelve steps out of the 343 a pristine build runs. Note carefully what is **absent**: CMake never re-ran (no configuration changed), and `sensor.cpp` was not rebuilt — it is the only application source that never sees `node.pb.h`, because acquisition has no business knowing the wire format. Steps 3 and 4 are §9's out-of-tree sources, recognisable by the mirrored absolute path.

**Proves:** the generator is an ordinary build rule with ordinary dependency tracking, and the schema genuinely cannot go stale — the only way to get `node.pb.c` is to run the generator on the current `.proto`. It also shows the acquisition/transport boundary from a completely different angle: `sensor.cpp` does not rebuild because it has never heard of the wire format.

### Exercise 4 — Make CMake re-run, and feel the difference

*Demonstrates §3's practical consequence: configuration changes are not source changes.*

Append a Kconfig line to `gateway/prj.conf` and rebuild:

```sh
echo 'CONFIG_ASSERT=y' >> gateway/prj.conf
./scripts/build.sh
```

The build re-runs CMake and Kconfig, regenerates `.config` and `autoconf.h`, and then rebuilds essentially the entire tree — **hundreds of steps**, against twelve in Exercise 3. Remove the line and rebuild to restore.

The reason is §5.3's second route: `autoconf.h` is force-included into *every* translation unit via `-imacros`, so changing one symbol invalidates all of them. Pick your symbol deliberately when trying this — many are already at the value you would set. Adding `CONFIG_THREAD_NAME=y`, for instance, changes nothing at all, because the board's defconfig already turned it on; the build correctly does almost nothing, which proves the same point from the other side.

**Proves:** editing a `.cpp` is a Ninja-only event while editing `prj.conf` is a configuration event that re-runs the whole front half of §7. That asymmetry is the daily, felt consequence of "CMake thinks, Ninja executes", and it is why the pristine-build advice attaches to devicetree and Kconfig changes specifically.

### Exercise 5 — One source, two instruction sets

*Demonstrates §9.1: `shared/` is source shared between targets, not a library shared between images.*

Build both applications, then ask each build tree how it compiled the *same* file:

```sh
./scripts/build.sh && ./scripts/build.sh -a peer-node

for app in gateway peer-node; do
  python3 -c "
import json
d = json.load(open('$app/build/compile_commands.json'))
e = [x for x in d if x['file'].endswith('shared/protocol.cpp')][0]
print('$app:', *[t for t in e['command'].split() if t.startswith('-mcpu')])
"
done
```

```
gateway: -mcpu=cortex-m7
peer-node: -mcpu=cortex-m0
```

One file, two entirely different processors. Now look at where each object landed:

```sh
find gateway/build/CMakeFiles/app.dir -name 'protocol.cpp.obj'
```

```
gateway/build/CMakeFiles/app.dir/Users/.../shared/protocol.cpp.obj
```

**Proves:** nothing is shared at the binary level, and nothing could be — a Cortex-M0 cannot run Cortex-M7 code. `shared/` is a directory of *source*, compiled from scratch into each application's own `app` target, which is exactly what §9.1 claims and exactly why the plain `target_sources` mechanism is sufficient. It also shows why the two apps each run the nanopb generator for themselves: the generated `.c` beside these objects has to be compiled for its own part too.

### Exercise 6 — Break the dependency direction on purpose

*Demonstrates §9.4: the layout is enforced by include paths, not by good intentions.*

Reach from shared code into an application's private header. Add one line to `shared/protocol.h`, just below its `#include <node.pb.h>`:

```c
#include "relay.h"   /* the gateway's own header — a deliberate violation */
```

Now build the app that *owns* `relay.h`:

```sh
./scripts/build.sh
```

```
In file included from .../shared/protocol.cpp:3:
.../shared/protocol.h:29:10: fatal error: relay.h: No such file or directory
```

The **gateway** failed — the app the header belongs to. That is the interesting part, and the reason is worth slowing down for. Ask what `shared/protocol.cpp` actually compiles with:

```sh
python3 -c "
import json
d = json.load(open('gateway/build/compile_commands.json'))
e = [x for x in d if x['file'].endswith('shared/protocol.cpp')][0]
print(*[t for t in e['command'].split() if t.startswith('-I') and 'Ethernet' in t], sep='\n')
"
```

```
-I.../gateway/build
-I.../gateway/../shared
-I.../gateway/build/zephyr/include/generated/zephyr
-I.../gateway/build/zephyr/include/generated
```

`shared/` is there. **`gateway/src/` is not** — it is on no `-I` flag at all. The application's own sources reach their neighbours only through the C preprocessor's rule that a quoted `#include` is searched for *first in the directory of the file doing the including*: `relay.cpp` finds `relay.h` because they sit side by side. A header in `shared/` gets no such help, because the search starts where *it* lives.

Revert with `git checkout shared/protocol.h`.

**Proves:** "nothing in `shared/` may depend on an application" is not a convention a reviewer has to police — it is a rule the preprocessor applies, and it holds even for the app that owns the header. The inverse also holds and is what makes the arrangement useful: both apps *can* reach into `shared/`, because that is the one directory deliberately placed on both include paths. The dependency direction of §9.4 is built out of exactly those two facts.

---

## 13. The model in one paragraph

A Zephyr build is a configuration system that generates code and then compiles it. Two independent systems answer two independent questions: **devicetree** describes the hardware (facts, from the board), and **Kconfig** selects the software (policy, your choice). The devicetree is parsed into `edt.pickle`, which becomes both the C macros your code reads and the values that decide which drivers Kconfig turns on — that shared parsed tree is the bridge between the two halves. CMake runs this entire reconciliation up front and hands a static build plan to Ninja, which does the actual compiling. Your own generators plug into the same seam — a build rule producing sources into `build/`, tracked like any other dependency. And because it all resolves before the target boots, your hardware arrives in `main()` already described, already configured, and already bound to its driver.

## 14. Where to go next

- **[`build-system-overview.md`](../docs/build-system-overview.md)** — the reference half of this guide: the artifact-by-artifact table for *this* build, with real paths and sizes, including the nanopb row §8 describes.
- **`gateway/build/zephyr/zephyr.dts` and `.config`** — the two files Exercises 1 and 2 read. Skimming them once, in full, is worth more than another page of prose about what they contain.
- **[`language-cpp.md`](language-cpp.md)** — the other half of what `target_sources` does here, and why exactly one of the gateway's six compiled sources is C while the other five are C++.
- **Zephyr's own build documentation** — the `west build` reference and the *Application Development* chapter. Worth reading on the two things §9 only sketches: **sysbuild**, if you ever need several images configured together, and `zephyr_library*()`, if shared code ever grows a Kconfig of its own. `EXTRA_CONF_FILE` you have already used — it is what `build.sh --debug` passes.
