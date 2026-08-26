# Writing C++ on Zephyr

*A from-first-principles guide to what actually changes when your application is C++ instead of C.*

This is a teaching document, not project documentation. It explains the concepts, the machinery, and the sharp edges of writing a Zephyr application in C++, in the order that makes them easiest to learn. It uses this repository — a CO₂ sensor node built from five C++ translation units — only as a running example to keep the ideas concrete. For how the build system underneath works at all, see its companion, [`zephyr-build-system-guide.md`](zephyr-build-system-guide.md).

**The shape of this document:**

- **§1–§2** — the two facts everything else follows from, and how to switch C++ on.
- **§3–§5** — *freestanding*: the runtime ladder, what exceptions and RTTI cost, and the header breakage that is usually the first thing you hit.
- **§6–§8** — *the boundary*: name mangling and `extern "C"`, static constructors at boot, and where Zephyr's macros stop being C++-safe.
- **§9–§11** — what C++ is actually worth on an MCU, mixing the two languages in one image, and a field guide to the daily quirks.
- **§12** — exercises: four things to break on purpose, all on the Mac, no board needed.
- **§13–§14** — the whole model in a paragraph, and where to go next.

---

## 1. The mental model: a C++ guest in a C house

Zephyr is a C project. The kernel, every driver, every subsystem — the sensor API, zbus, the MQTT library, the net stack — is written in C and compiled by a C compiler. C++ is not woven through Zephyr; it is an *application* language bolted onto the top, and everything strange about using it follows from two facts:

1. **You get *freestanding* C++, not hosted C++.** The C++ you know on a desktop assumes an operating system underneath it — a heap, `<vector>`, `<string>`, `<iostream>`, exceptions that unwind the stack. On a microcontroller none of that is free, and Zephyr gives you almost none of it by default.
2. **You live on a C/C++ boundary.** Every Zephyr API you call was compiled as C. Your code has to speak to it across the line where C++'s extra machinery (name mangling, member functions, lambdas) meets C's flat world of plain functions and structs.

Hold those two ideas and the rest of this document is just their consequences. Nearly every "why doesn't this compile?" and "why is this the pattern?" traces back to *freestanding* or *boundary* — and the document takes them in that order, because the freestanding consequences are the ones you hit within the first hour (§3–§5), while the boundary only bites once you start handing functions to C subsystems (§6–§8).

---

## 2. Turning it on

The single most reassuring fact: **the compiler was already a C++ compiler.** The Zephyr SDK ships `arm-zephyr-eabi-g++` alongside the C driver; you are not installing anything. Enabling C++ is just telling the build to use it.

Two Kconfig symbols do the whole job:

```conf
CONFIG_CPP=y          # compile C++ translation units at all
CONFIG_STD_CPP17=y    # pick the language standard (-std=c++17)
```

A trap worth knowing up front: **the default standard is C++11**, not something modern. If you care about `constexpr` breadth, `if constexpr`, structured bindings, or guaranteed copy elision, set `CONFIG_STD_CPP17` (or `..._CPP20`) explicitly — leaving it unset silently gives you 2011.

On the CMake side there is nothing special. Give your sources a `.cpp` extension, list them the same way you would a `.c` file, and Zephyr compiles them with `g++`:

```cmake
target_sources(app PRIVATE src/main.cpp src/sensor.cpp src/relay.cpp)
```

In the build log you will see the payoff. These are the C++-relevant lines out of a 343-step pristine build of this app:

```
[18/343] Building C   object CMakeFiles/app.dir/node.pb.c.obj
[19/343] Building CXX object zephyr/CMakeFiles/zephyr.dir/lib/cpp/minimal/cpp_vtable.cpp.obj
[20/343] Building CXX object zephyr/CMakeFiles/zephyr.dir/lib/cpp/minimal/cpp_new.cpp.obj
[25/343] Building CXX object CMakeFiles/app.dir/.../shared/commands.cpp.obj
[26/343] Building CXX object CMakeFiles/app.dir/.../shared/protocol.cpp.obj
[27/343] Building CXX object CMakeFiles/app.dir/src/sensor.cpp.obj
[35/343] Building CXX object CMakeFiles/app.dir/src/relay.cpp.obj
[46/343] Building CXX object CMakeFiles/app.dir/src/main.cpp.obj
[290/343] Linking CXX static library app/libapp.a
[343/343] Linking CXX executable zephyr/zephyr.elf
```

Ten lines out of 343 — the other 333 are C. Four details in there are the whole of this document in miniature:

- **`lib/cpp/minimal/…`** is Zephyr's C++ runtime compiling itself into your image. Two files: vtable support and `operator new`. That is very nearly the entire runtime you get, and §3 is about what it does and does not contain.
- **`node.pb.c` is built as a C object inside the app target.** Generated nanopb code is C and stays C, sitting in the same target as five C++ files. §10 returns to this.
- **Two of those C++ objects have a long path.** `commands.cpp` and `protocol.cpp` live in `shared/`, outside the application, and are compiled into this app's target from there — the peer node compiles the same two files for a Cortex-M0. See [`zephyr-build-system-guide.md`](zephyr-build-system-guide.md) §9.
- **Only your own sources are CXX.** C++ here is an application language grafted onto a C system, which is §1's founding claim, visible in the build log.

Because `CONFIG_CPP` is a Kconfig change, it ripples through generated configuration — do a pristine build (`-p`) the first time you add it.

---

## 3. Freestanding vs hosted: the runtime ladder

This is the one big idea, the C++ analogue of "devicetree vs Kconfig." A C++ program leans on a **runtime**: the code and data that make `new`, virtual calls, static objects, exceptions, and the standard library work. On a desktop that runtime is enormous and always present. On Zephyr you choose *how much of it* to pull in, and the default is deliberately tiny.

Zephyr ships a **minimal C++ runtime** (`zephyr/lib/cpp/minimal`). It provides the *language-support* runtime — the parts the compiler *requires* to emit working code — and almost nothing else:

- `operator new` / `operator delete` (we will see they map to `malloc`/`free`).
- Virtual-table and pure-virtual-call support, so classes with `virtual` methods link.
- Static-initialisation guards and the machinery that **runs global constructors at boot**.
- Exactly three standard headers: **`<new>`, `<cstddef>`, `<cstdint>`**. That is the whole list.

What it deliberately leaves out is the **standard *library***: no `<vector>`, `<string>`, `<map>`, `<algorithm>`, `<iostream>`, and — the one that surprises everyone — none of the `<cXXX>` C-library wrappers like `<cstdio>` or `<cstring>`. It compiles with `-nostdinc++`, so those headers are not merely empty; they are *not on the include path at all*.

If you genuinely need the full standard library, there is a rung up the ladder:

| Runtime | Kconfig | What you get | Cost |
| --- | --- | --- | --- |
| **Minimal** (default) | `MINIMAL_LIBCPP` | Language support + `<new>/<cstddef>/<cstdint>` | Tiny; matches a no-malloc target |
| **Full libstdc++** | `CONFIG_REQUIRES_FULL_LIBCPP=y` | Real `<vector>`, `<string>`, `<c*>` headers (GCC's `GLIBCXX_LIBCPP`) | Code size, heap use, hidden allocation |

The guidance that falls out of this: **stay minimal unless something forces you up.** A constrained node wants the same discipline nanopb already imposes — fixed-size buffers, no hidden allocation — and the minimal runtime keeps you honest by making the tempting hosted conveniences simply unavailable. Reach for `REQUIRES_FULL_LIBCPP` only when a concrete dependency needs it, and know that you have opted into a heap.

---

## 4. Exceptions and RTTI: off, and why it shapes your code

Two more pieces of hosted C++ are disabled by default, and Zephyr passes the flags to prove it: every C++ TU is compiled `-fno-exceptions -fno-rtti`.

- **Exceptions** need stack-unwinding tables baked into flash and turn any `throw` into a non-deterministic, non-local jump — both things you avoid on a real-time MCU. With them off, `throw`/`try`/`catch` won't compile.
- **RTTI** (run-time type information) carries per-type metadata for `dynamic_cast` and `typeid`. Off, those won't compile either.

This is not a limitation to work *around*; it is a style to write *within*. The C APIs you are calling already return error codes rather than throwing, so match them: **propagate failures by return value, not exceptions**, and resolve types at compile time instead of with `dynamic_cast`. If a third-party library truly requires them, `CONFIG_CPP_EXCEPTIONS` and `CONFIG_CPP_RTTI` opt back in — but treat that as a deliberate weight gain, not a default.

---

## 5. The header consequence — the first thing that bites

Sections 3 and 4 have an immediate, concrete casualty. Because the minimal runtime ships no `<cXXX>` wrappers and compiles `-nostdinc++`, the idiomatic C++ spelling fails outright:

```cpp
#include <cstdio>   // fatal error: cstdio: No such file or directory
```

The fix is to include the **C header** instead — it is on the path and works perfectly from C++:

```cpp
#include <stdio.h>  // printf(), as used in this project's main.cpp
```

Same for `<cstring>` → `<string.h>`, `<cstdlib>` → `<stdlib.h>`, and so on. The only C++-spelled headers you may assume exist under the minimal runtime are the three it ships: `<new>`, `<cstddef>`, `<cstdint>`. The `<cXXX>` family reappears only once `REQUIRES_FULL_LIBCPP` pulls in real libstdc++.

Remember the *cause*, not just the rule: it is the freestanding runtime, not a compiler bug.

---

## 6. The C/C++ boundary: linkage and name mangling

Now the other founding fact. To support overloading, a C++ compiler **mangles** names — `read(int)` and `read(float)` become distinct decorated symbols. A C compiler does not; `read` is just `read`. So a C++ TU and a C-compiled kernel disagree about what symbols are called, and something has to reconcile them. That something is **language linkage**, spelled `extern "C"`: "give this declaration C linkage — don't mangle it, it matches a C symbol."

Three practical truths follow.

**Calling Zephyr from C++ just works — and here's why.** You never sprinkle `extern "C"` over Zephyr calls, because every public Zephyr header already wraps its declarations in an `extern "C"` guard. Including `<zephyr/drivers/sensor.h>` from a `.cpp` therefore yields C-linkage symbols that line up with the C-compiled driver. The same guard is why nanopb's generated `*.pb.h` includes cleanly from C++ — it, too, is wrapped.

**`main` needs nothing.** The C++ standard reserves `main` and forbids mangling it, so a plain `int main(void)` in a `.cpp` matches Zephyr's C startup as-is. No `extern "C"`, no ceremony.

**Callbacks are the sharp edge.** C subsystems work by storing a *function pointer* and calling it later from C — a zbus observer, the MQTT event handler, a sensor trigger handler. Handing them a C++ callable has rules:

- A **captureless lambda** converts to a plain function pointer. Fine for a callback.
- A **capturing lambda** or a **non-static member function** does **not** convert — there is no slot in a C function pointer to smuggle the captured variables or the `this` pointer through. The compiler will reject it.
- The way you actually pass context, then, is not a capture but the API's own mechanism: Zephyr callbacks almost always hand you a `user_data` pointer or a message struct. Put your state there and recover it inside a free function.

So the idiomatic shape is a **free function** (a captureless lambda, or a function you can mark `extern "C"` to be strictly correct about linkage) that pulls its context out of `user_data`, not a method and not a capture.

This project hands three such functions to C subsystems, and they are worth looking at together because each crosses the boundary differently:

| Callback | Registered with | Called from |
| --- | --- | --- |
| `mqtt_evt_handler` | `client.evt_cb = …` — a struct field holding a function pointer | inside `mqtt_input()`, on your own thread |
| `on_telemetry` | `ZBUS_LISTENER_DEFINE(telemetry_listener, on_telemetry)` — a macro building a static record | inside `zbus_chan_pub()`, on the *publisher's* thread |
| `sensor_cmd_valid` | an argument to `ZBUS_CHAN_DEFINE(...)` | inside `zbus_chan_pub()`, before the message is stored |

All three are plain file-scope functions taking their context from the arguments the C API supplies — an `mqtt_evt *`, a `zbus_channel *`, a `const void *msg`. None of them could have been a capturing lambda or a member function.

Note the second and third also illustrate why the C++ side has to care about **linkage**, not just calling convention: `ZBUS_LISTENER_DEFINE` and `ZBUS_CHAN_DEFINE` emit named symbols that the *other* translation unit refers to by name through `ZBUS_CHAN_DECLARE`. Put them inside an anonymous namespace — the §9 idiom for internal linkage — and the definition and the declaration no longer name the same thing. In `sensor.cpp` and `main.cpp` those definitions therefore sit deliberately at global scope, outside the anonymous namespace that holds everything else in the file.

---

## 7. Constructors, destructors, and the heap at boot

C++ objects with static storage duration — file-scope and function-`static` objects — have constructors that must run *sometime*. On a hosted system the C runtime does it before `main`. Zephyr reproduces that: **global constructors run during kernel initialisation, before `main()`**. So a file-scope C++ object is fully constructed by the time your application code executes, and you can rely on it in `main`.

Two caveats complete the picture:

- **Destructors of globals effectively never run.** An embedded image doesn't "exit," so the `atexit`/destructor machinery exists for completeness but doesn't fire in practice. Don't put meaningful cleanup in a global's destructor expecting it to happen.
- **`operator new` is real but is `malloc` in disguise.** The minimal runtime implements `operator new` as a call to the C library's `malloc` (and `delete` as `free`). That means `new` "works" *only if a libc heap is configured*, and even then it is heap allocation — the very thing a constrained node avoids. Prefer static and stack allocation, Zephyr kernel objects, and fixed-size buffers. This is the same static-allocation discipline nanopb asks for, applied to the language itself.

---

## 8. Zephyr's macros in C++ — mostly fine, one edge

Zephyr leans heavily on macros, and it is fair to worry whether they survive in C++. Mostly they do — with one category to watch.

The **query and handle macros are pure constant expressions** and behave identically in C and C++. This project's `sensor.cpp` calls, unchanged:

```cpp
const struct device *const scd40 = DEVICE_DT_GET(DT_NODELABEL(scd40));
```

`DT_NODELABEL`, `DEVICE_DT_GET`, the `DT_DRV_*` family — all fine.

The edge is a family of **initialiser macros** that expand to *C-only syntax*. Two constructs in particular have no standard C++ equivalent: **compound literals** (`(struct foo){ ... }`) and **out-of-order designated initialisers**, both of which some `*_DT_SPEC_GET`-style macros emit. GCC accepts many of them as extensions (this project isn't built with `-pedantic`, so they slip through), but you are relying on compiler grace, and a macro that leans on a form GCC won't extend will fail to compile in a `.cpp`.

When one does bite, the fix is the boundary technique from §6 in reverse: **keep that macro usage in a small C translation unit** and expose the result to C++ through an `extern "C"` accessor. Let C do the C-shaped thing; call across the line for the answer.

---

## 9. Idioms that belong on an MCU

C++ earns its place here through a handful of patterns that fit a constrained, real-time target — not through the hosted conveniences you've had to give up.

- **RAII around C handles** is the highest-value pattern. Acquire a resource in a constructor, release it in the destructor, and the compiler guarantees the release: a scoped mutex lock, a bus transaction, the MQTT client's connected state. This is the one thing C genuinely cannot express, and it directly targets the lifecycle bugs (forgotten unlocks, leaked connections) that the project is about.
- **Classes as modules.** A publisher object that *owns* its sequence counter, buffers, and client handle beats a scatter of file-static globals — clearer ownership, easier to reason about.
- **`constexpr` for compile-time constants**, e.g. this project's `constexpr uint16_t kKeepaliveSec = 60;` and `constexpr size_t kSensorStackSize = 2048;` — typed constants with no storage and no macro, where C would reach for `#define`.
- **Anonymous namespaces** for internal linkage, in place of file-`static` — with the one exception §6 ends on: symbols another translation unit names, such as the `ZBUS_*` definitions, must stay at global scope.
- **Static allocation over the heap**, for the reasons §7 gives — and note that the minimal runtime helps you here by making the alternative inconvenient rather than by forbidding it.

Use C++ for *structure and safety*, not to import a desktop programming style onto a 512 KB part.

---

## 10. Mixing C and C++ in one image

A Zephyr firmware image is overwhelmingly C with a few C++ translation units grafted on, and the linker joins them without fuss — you saw `libapp.a` (CXX) linked into `zephyr.elf` alongside dozens of C archives. Two rules keep the mix clean:

- **Generated C stays C.** nanopb's `*.pb.c` / `*.pb.h` are C; compile them with the C compiler and include the header from C++ across its `extern "C"` guard. Never rename a generated file to `.cpp` or hand-edit it — regenerate from the `.proto`, which is the single source of truth for the wire format.
- **Pick the language per file, deliberately.** A file is C++ because it *benefits* from C++ (RAII, a class); everything else — glue, generated code, anything that only pushes bytes around — is fine as C.

---

## 11. Day-to-day quirks

A short field guide, in the order you are likely to meet them. The first two are the sections above compressed to one line each; the rest appear nowhere else in this document:

- **`<cstdio>` and friends don't exist** — use the C header (§5). **Set `CONFIG_STD_CPP17` explicitly** — the default is C++11, and pristine-build after changing it (§2).
- **Your IDE will show false errors.** clangd reads the GCC ARM compile flags (`-mfp16-format=ieee`, `-fno-reorder-functions`) and Zephyr macros (`K_SECONDS`) it can't parse, and flags them red — while the GCC build is clean. **The GCC build is the source of truth.** A `.clangd` file that removes the offending flags silences the noise.
- **`-Wsection` on every `ZBUS_*_DEFINE` is a C++-only artefact.** zbus's `_ZBUS_CPP_EXTERN` expands to `extern` under `__cplusplus` and to nothing in C, so in C++ the definition is a *re*declaration of the `ZBUS_CHAN_DECLARE`/`ZBUS_OBS_DECLARE` extern — and it is the one carrying the section attribute. clang warns; GCC doesn't. The split is zbus's intended API (§6), nothing reads the section from the declaration side, so `.clangd` suppresses it.
- **A link error naming a symbol you can see defined** is almost always linkage, not a missing file: check whether the definition drifted into an anonymous namespace (§6).
- **Link errors are where mangling becomes visible.** An undefined symbol reported as `_Z11on_telemetryPK13zbus_channel` rather than `on_telemetry` is telling you which side of the boundary the reference came from — pipe it through `c++filt` to read it.
- **Compiler diagnostics point at the `.cpp`, not the macro** when a Zephyr initialiser macro expands to C-only syntax (§8). Build with `-save-temps` or read the expansion in `build/` before assuming your own code is wrong.

---

## 12. Exercising the C++ layer

Four exercises, all on the Mac with no board attached. Three of them work by **breaking something on purpose** — which is the fastest way to believe a rule you have only read. Each says what to revert; revert it before moving on.

```sh
./scripts/build.sh          # incremental; -p forces pristine
```

### Exercise 1 — Find the C++ in a C image

*Demonstrates §1: C++ is an application language grafted onto a C system.*

```sh
./scripts/build.sh -p 2>&1 | grep -E 'CXX|cpp/minimal|node\.pb\.c'
```

You get the ten lines quoted in §2 and no more. Count them against the 343 total, then look at *which* files they are: two from `zephyr/lib/cpp/minimal`, five of your own, and one C object — `node.pb.c` — compiled as C inside the same `app` target as the C++ files.

**Proves:** the runtime you depend on is two translation units, your code is the only C++ in the application, and language is chosen per file rather than per project (§10).

### Exercise 2 — Watch the freestanding runtime refuse a header

*Demonstrates §3 and §5: no `<cXXX>` wrappers, and `-nostdinc++` means they are not merely empty but absent.*

Add the idiomatic C++ spelling to the top of `gateway/src/sensor.cpp`:

```cpp
#include <cstdio>
```

```
gateway/src/sensor.cpp:1:10: fatal error: cstdio: No such file or directory
```

Change it to `#include <stdio.h>` and the build succeeds. **Revert both** — `sensor.cpp` needs neither.

**Proves:** the failure is the *runtime*, not the compiler or a missing package. The same compiler compiles `<stdio.h>` from the same file a second later; what changed is only which header exists on the include path.

### Exercise 3 — Break the linkage rule and read the error

*Demonstrates §6: `ZBUS_*` definitions must stay at global scope.*

In `gateway/src/main.cpp`, move the `ZBUS_LISTENER_DEFINE(telemetry_listener, on_telemetry);` line from below `}  // namespace` to just above it, so it falls inside the anonymous namespace. Compilation still succeeds; the **link** does not:

```
ld.bfd: app/libapp.a(sensor.cpp.obj):(._zbus_channel_observation.static.chan_telemetry00_+0x4):
        undefined reference to `telemetry_listener'
```

**Revert it.**

Read that error closely, because it names the whole mechanism. The complaint comes from **`sensor.cpp`**, not from the file you edited: `ZBUS_CHAN_DEFINE(chan_telemetry, …, ZBUS_OBSERVERS(telemetry_listener), …)` emitted an observation record there that refers to the symbol *by name*. Internal linkage renamed the definition, so the two no longer meet.

**Proves:** a macro that emits a symbol another translation unit names is a **linkage** decision, not a style one — and that C++'s internal-linkage idiom (§9) has exactly one exception in this codebase, which the comment above that line records.

### Exercise 4 — See mangling, and see where it stops

*Demonstrates §6: what `extern "C"` and the reserved status of `main` are actually doing.*

```sh
NM=~/zephyr-sdk-1.0.1/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-nm
$NM gateway/build/zephyr/zephyr.elf | grep -E ' [A-Za-z] (chan_telemetry|telemetry_listener|main)$'
$NM gateway/build/zephyr/zephyr.elf | grep _GLOBAL__N_1 | head -4
```

The first command prints plain, undecorated names:

```
080292f8 R chan_telemetry
08029320 R telemetry_listener
0800295c T main
```

The second prints the same program's C++ functions, decorated:

```
080023d4 t _ZN12_GLOBAL__N_112on_telemetryEPK12zbus_channel
08002888 t _ZN12_GLOBAL__N_116mqtt_evt_handlerEP11mqtt_clientPK8mqtt_evt
```

Pipe those through `arm-zephyr-eabi-c++filt` to read them:

```
(anonymous namespace)::on_telemetry(zbus_channel const*)
(anonymous namespace)::mqtt_evt_handler(mqtt_client*, mqtt_evt const*)
```

**Proves:** mangling is real and visible, and the boundary runs exactly where §6 says. Note the case column — capital `R`/`T` for the global, undecorated symbols; lowercase `t` for the mangled, file-local ones. The *records* C code refers to by name (`chan_telemetry`, `telemetry_listener`) are global and unmangled; the **callbacks they point to** are mangled, file-local, and perfectly happy that way, because nothing ever names them across a translation unit — they are reached through a function pointer. That is Exercise 3's error explained from the other direction, and it is why §6 says the sharp edge is at the callback *registration*, not at the callback itself.

---

## 13. The model in one paragraph

Zephyr is a C system with C++ available as an application language, and everything about using it follows from two facts. First, you get **freestanding** C++: a minimal runtime that provides `new`, vtables, and boot-time constructors but no standard library, no exceptions, and no RTTI — so you include C headers, return error codes instead of throwing, and allocate statically. Second, you sit on a **C/C++ boundary**: names mangle on your side and not the kernel's, so `extern "C"` reconciles them — automatically for the Zephyr headers that wrap themselves in it, and by hand at the callback edge, where only free functions and captureless lambdas cross, context travels through `user_data`, and any symbol another translation unit names must stay at global scope. Turn it on with `CONFIG_CPP` plus an explicit standard, use C++ for RAII and structure rather than for a hosted programming style, keep generated C as C, and the language buys you real safety on the target without dragging a desktop runtime onto it.

## 14. Where to go next

This guide has no reference half — there is no `docs/` counterpart, because the decisions it would record are visible in the two source files themselves. So:

- **`gateway/src/sensor.cpp` and `gateway/src/main.cpp`** — read them for the idioms in §9 as they actually appear: the anonymous namespace, the `constexpr` constants, the three callbacks of §6, and the one comment explaining why the `ZBUS_*` definitions sit outside the namespace.
- **[`zephyr-build-system-guide.md`](zephyr-build-system-guide.md)** — the companion: what `CONFIG_CPP` and `target_sources` are doing inside the build, and why `node.pb.c` is compiled as C beside them.
- **[`zbus-guide.md`](zbus-guide.md) §9** — the other side of Exercise 3: what those channel and observer symbols *are*, and why the definitions live in `sensor.cpp`.
- **`zephyr/lib/cpp/minimal/`** in the workspace — the entire runtime, small enough to read in one sitting. `cpp_new.cpp` is where §7's claim that `operator new` is `malloc` in disguise is settled in about ten lines.
- **`CONFIG_REQUIRES_FULL_LIBCPP`** — try a pristine build with it on and compare the footprint against §3's table, if you ever need to justify staying minimal.
