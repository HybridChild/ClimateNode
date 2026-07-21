# Writing C++ on Zephyr

*A from-first-principles guide to what actually changes when your application is C++
instead of C.*

This is a teaching document, not project documentation. It explains the concepts, the
machinery, and the sharp edges of writing a Zephyr application in C++, in the order that
makes them easiest to learn. It uses this repository — a CO₂ sensor node built from two
C++ translation units, `sensor.cpp` and `main.cpp` — only as a running example to keep the
ideas concrete. For how the build system underneath works at all, see its companion,
[`zephyr-build-system-guide.md`](zephyr-build-system-guide.md).

---

## 1. The mental model: a C++ guest in a C house

Zephyr is a C project. The kernel, every driver, every subsystem — the sensor API, zbus,
the MQTT library, the net stack — is written in C and compiled by a C compiler. C++ is not
woven through Zephyr; it is an *application* language bolted onto the top, and everything
strange about using it follows from two facts:

1. **You live on a C/C++ boundary.** Every Zephyr API you call was compiled as C. Your code
   has to speak to it across the line where C++'s extra machinery (name mangling, member
   functions, lambdas) meets C's flat world of plain functions and structs.
2. **You get *freestanding* C++, not hosted C++.** The C++ you know on a desktop assumes an
   operating system underneath it — a heap, `<vector>`, `<string>`, `<iostream>`,
   exceptions that unwind the stack. On a microcontroller none of that is free, and Zephyr
   gives you almost none of it by default.

Hold those two ideas and the rest of this document is just their consequences. Nearly every
"why doesn't this compile?" and "why is this the pattern?" traces back to *boundary* or
*freestanding*.

---

## 2. Turning it on

The single most reassuring fact: **the compiler was already a C++ compiler.** The Zephyr SDK
ships `arm-zephyr-eabi-g++` alongside the C driver; you are not installing anything. Enabling
C++ is just telling the build to use it.

Two Kconfig symbols do the whole job:

```conf
CONFIG_CPP=y          # compile C++ translation units at all
CONFIG_STD_CPP17=y    # pick the language standard (-std=c++17)
```

A trap worth knowing up front: **the default standard is C++11**, not something modern. If
you care about `constexpr` breadth, `if constexpr`, structured bindings, or guaranteed copy
elision, set `CONFIG_STD_CPP17` (or `..._CPP20`) explicitly — leaving it unset silently
gives you 2011.

On the CMake side there is nothing special. Give your sources a `.cpp` extension, list them
the same way you would a `.c` file, and Zephyr compiles them with `g++`:

```cmake
target_sources(app PRIVATE src/main.cpp src/sensor.cpp)
```

In the build log you will see the payoff — the app's translation units built as CXX, and the
final image linked as a C++ executable, everything else still C:

```
[39/185] Building CXX object CMakeFiles/app.dir/src/main.cpp.obj
[40/185] Building CXX object CMakeFiles/app.dir/src/sensor.cpp.obj
[146/185] Linking CXX static library app/libapp.a
[185/185] Linking CXX executable zephyr/zephyr.elf
```

Because `CONFIG_CPP` is a Kconfig change, it ripples through generated configuration — do a
pristine build (`-p`) the first time you add it.

---

## 3. Freestanding vs hosted: the runtime ladder

This is the one big idea, the C++ analogue of "devicetree vs Kconfig." A C++ program leans on
a **runtime**: the code and data that make `new`, virtual calls, static objects, exceptions,
and the standard library work. On a desktop that runtime is enormous and always present. On
Zephyr you choose *how much of it* to pull in, and the default is deliberately tiny.

Zephyr ships a **minimal C++ runtime** (`zephyr/lib/cpp/minimal`). It provides the
*language-support* runtime — the parts the compiler *requires* to emit working code — and
almost nothing else:

- `operator new` / `operator delete` (we will see they map to `malloc`/`free`).
- Virtual-table and pure-virtual-call support, so classes with `virtual` methods link.
- Static-initialisation guards and the machinery that **runs global constructors at boot**.
- Exactly three standard headers: **`<new>`, `<cstddef>`, `<cstdint>`**. That is the whole
  list.

What it deliberately leaves out is the **standard *library***: no `<vector>`, `<string>`,
`<map>`, `<algorithm>`, `<iostream>`, and — the one that surprises everyone — none of the
`<cXXX>` C-library wrappers like `<cstdio>` or `<cstring>`. It compiles with `-nostdinc++`,
so those headers are not merely empty; they are *not on the include path at all*.

If you genuinely need the full standard library, there is a rung up the ladder:

| Runtime | Kconfig | What you get | Cost |
| --- | --- | --- | --- |
| **Minimal** (default) | `MINIMAL_LIBCPP` | Language support + `<new>/<cstddef>/<cstdint>` | Tiny; matches a no-malloc target |
| **Full libstdc++** | `CONFIG_REQUIRES_FULL_LIBCPP=y` | Real `<vector>`, `<string>`, `<c*>` headers (GCC's `GLIBCXX_LIBCPP`) | Code size, heap use, hidden allocation |

The guidance that falls out of this: **stay minimal unless something forces you up.** A
constrained node wants the same discipline nanopb already imposes — fixed-size buffers, no
hidden allocation — and the minimal runtime keeps you honest by making the tempting hosted
conveniences simply unavailable. Reach for `REQUIRES_FULL_LIBCPP` only when a concrete
dependency needs it, and know that you have opted into a heap.

---

## 4. Exceptions and RTTI: off, and why it shapes your code

Two more pieces of hosted C++ are disabled by default, and Zephyr passes the flags to prove
it: every C++ TU is compiled `-fno-exceptions -fno-rtti`.

- **Exceptions** need stack-unwinding tables baked into flash and turn any `throw` into a
  non-deterministic, non-local jump — both things you avoid on a real-time MCU. With them
  off, `throw`/`try`/`catch` won't compile.
- **RTTI** (run-time type information) carries per-type metadata for `dynamic_cast` and
  `typeid`. Off, those won't compile either.

This is not a limitation to work *around*; it is a style to write *within*. The C APIs you
are calling already return error codes rather than throwing, so match them: **propagate
failures by return value, not exceptions**, and resolve types at compile time instead of with
`dynamic_cast`. If a third-party library truly requires them, `CONFIG_CPP_EXCEPTIONS` and
`CONFIG_CPP_RTTI` opt back in — but treat that as a deliberate weight gain, not a default.

---

## 5. The header consequence — the first thing that bites

Sections 3 and 4 have an immediate, concrete casualty. Because the minimal runtime ships no
`<cXXX>` wrappers and compiles `-nostdinc++`, the idiomatic C++ spelling fails outright:

```cpp
#include <cstdio>   // fatal error: cstdio: No such file or directory
```

The fix is to include the **C header** instead — it is on the path and works perfectly from
C++:

```cpp
#include <stdio.h>  // printf(), as used in this project's main.cpp
```

Same for `<cstring>` → `<string.h>`, `<cstdlib>` → `<stdlib.h>`, and so on. The only
C++-spelled headers you may assume exist under the minimal runtime are the three it ships:
`<new>`, `<cstddef>`, `<cstdint>`. The `<cXXX>` family reappears only once
`REQUIRES_FULL_LIBCPP` pulls in real libstdc++.

Remember the *cause*, not just the rule: it is the freestanding runtime, not a compiler bug.

---

## 6. The C/C++ boundary: linkage and name mangling

Now the other founding fact. To support overloading, a C++ compiler **mangles** names —
`read(int)` and `read(float)` become distinct decorated symbols. A C compiler does not; `read`
is just `read`. So a C++ TU and a C-compiled kernel disagree about what symbols are called,
and something has to reconcile them. That something is **language linkage**, spelled
`extern "C"`: "give this declaration C linkage — don't mangle it, it matches a C symbol."

Three practical truths follow.

**Calling Zephyr from C++ just works — and here's why.** You never sprinkle `extern "C"` over
Zephyr calls, because every public Zephyr header already wraps its declarations in an
`extern "C"` guard. Including `<zephyr/drivers/sensor.h>` from a `.cpp` therefore yields
C-linkage symbols that line up with the C-compiled driver. The same guard is why nanopb's
generated `*.pb.h` includes cleanly from C++ — it, too, is wrapped.

**`main` needs nothing.** The C++ standard reserves `main` and forbids mangling it, so a plain
`int main(void)` in a `.cpp` matches Zephyr's C startup as-is. No `extern "C"`, no ceremony.

**Callbacks are the sharp edge.** C subsystems work by storing a *function pointer* and calling
it later from C — a zbus observer, the MQTT event handler, a sensor trigger handler. Handing
them a C++ callable has rules:

- A **captureless lambda** converts to a plain function pointer. Fine for a callback.
- A **capturing lambda** or a **non-static member function** does **not** convert — there is
  no slot in a C function pointer to smuggle the captured variables or the `this` pointer
  through. The compiler will reject it.
- The way you actually pass context, then, is not a capture but the API's own mechanism:
  Zephyr callbacks almost always hand you a `user_data` pointer or a message struct. Put your
  state there and recover it inside a free function.

So the idiomatic shape is a **free function** (a captureless lambda, or a function you can mark
`extern "C"` to be strictly correct about linkage) that pulls its context out of `user_data`,
not a method and not a capture.

This project hands three such functions to C subsystems, and they are worth looking at
together because each crosses the boundary differently:

| Callback | Registered with | Called from |
| --- | --- | --- |
| `mqtt_evt_handler` | `client.evt_cb = …` — a struct field holding a function pointer | inside `mqtt_input()`, on your own thread |
| `on_telemetry` | `ZBUS_LISTENER_DEFINE(telemetry_listener, on_telemetry)` — a macro building a static record | inside `zbus_chan_pub()`, on the *publisher's* thread |
| `sensor_cmd_valid` | an argument to `ZBUS_CHAN_DEFINE(...)` | inside `zbus_chan_pub()`, before the message is stored |

All three are plain file-scope functions taking their context from the arguments the C API
supplies — an `mqtt_evt *`, a `zbus_channel *`, a `const void *msg`. None of them could
have been a capturing lambda or a member function.

Note the second and third also illustrate why the C++ side has to care about **linkage**,
not just calling convention: `ZBUS_LISTENER_DEFINE` and `ZBUS_CHAN_DEFINE` emit named
symbols that the *other* translation unit refers to by name through `ZBUS_CHAN_DECLARE`.
Put them inside an anonymous namespace — the §9 idiom for internal linkage — and the
definition and the declaration no longer name the same thing. In `sensor.cpp` and
`main.cpp` those definitions therefore sit deliberately at global scope, outside the
anonymous namespace that holds everything else in the file.

---

## 7. Constructors, destructors, and the heap at boot

C++ objects with static storage duration — file-scope and function-`static` objects — have
constructors that must run *sometime*. On a hosted system the C runtime does it before `main`.
Zephyr reproduces that: **global constructors run during kernel initialisation, before
`main()`**. So a file-scope C++ object is fully constructed by the time your application code
executes, and you can rely on it in `main`.

Two caveats complete the picture:

- **Destructors of globals effectively never run.** An embedded image doesn't "exit," so the
  `atexit`/destructor machinery exists for completeness but doesn't fire in practice. Don't
  put meaningful cleanup in a global's destructor expecting it to happen.
- **`operator new` is real but is `malloc` in disguise.** The minimal runtime implements
  `operator new` as a call to the C library's `malloc` (and `delete` as `free`). That means
  `new` "works" *only if a libc heap is configured*, and even then it is heap allocation — the
  very thing a constrained node avoids. Prefer static and stack allocation, Zephyr kernel
  objects, and fixed-size buffers. This is the same static-allocation discipline nanopb asks
  for, applied to the language itself.

---

## 8. Zephyr's macros in C++ — mostly fine, one edge

Zephyr leans heavily on macros, and it is fair to worry whether they survive in C++. Mostly
they do — with one category to watch.

The **query and handle macros are pure constant expressions** and behave identically in C and
C++. This project's `sensor.cpp` calls, unchanged:

```cpp
const struct device *const scd40 = DEVICE_DT_GET(DT_NODELABEL(scd40));
```

`DT_NODELABEL`, `DEVICE_DT_GET`, the `DT_DRV_*` family — all fine.

The edge is a family of **initialiser macros** that expand to *C-only syntax*. Two constructs
in particular have no standard C++ equivalent: **compound literals** (`(struct foo){ ... }`)
and **out-of-order designated initialisers**, both of which some `*_DT_SPEC_GET`-style macros
emit. GCC accepts many of them as extensions (this project isn't built with `-pedantic`, so
they slip through), but you are relying on compiler grace, and a macro that leans on a
form GCC won't extend will fail to compile in a `.cpp`.

When one does bite, the fix is the boundary technique from §6 in reverse: **keep that macro
usage in a small C translation unit** and expose the result to C++ through an `extern "C"`
accessor. Let C do the C-shaped thing; call across the line for the answer.

---

## 9. Idioms that belong on an MCU

C++ earns its place here through a handful of patterns that fit a constrained, real-time
target — not through the hosted conveniences you've had to give up.

- **RAII around C handles** is the highest-value pattern. Acquire a resource in a constructor,
  release it in the destructor, and the compiler guarantees the release: a scoped mutex lock,
  a bus transaction, the MQTT client's connected state. This is the one thing C genuinely
  cannot express, and it directly targets the lifecycle bugs (forgotten unlocks, leaked
  connections) that the project is about.
- **Classes as modules.** A publisher object that *owns* its sequence counter, buffers, and
  client handle beats a scatter of file-static globals — clearer ownership, easier to reason
  about.
- **`constexpr` for compile-time constants**, e.g. this project's `constexpr uint16_t
  kKeepaliveSec = 60;` and `constexpr size_t kSensorStackSize = 2048;` — typed constants
  with no storage and no macro, where C would reach for `#define`.
- **Anonymous namespaces** for internal linkage, in place of file-`static`.
- **Static allocation, fixed-size types, no standard containers on the hot path** — hold the
  same determinism contract C gave you: no hidden allocation, no `throw`, nothing that can
  block unexpectedly.

Use C++ for *structure and safety*, not to import a desktop programming style onto a 512 KB
part.

---

## 10. Mixing C and C++ in one image

A Zephyr firmware image is overwhelmingly C with a few C++ translation units grafted on, and
the linker joins them without fuss — you saw `libapp.a` (CXX) linked into `zephyr.elf`
alongside dozens of C archives. Two rules keep the mix clean:

- **Generated C stays C.** nanopb's `*.pb.c` / `*.pb.h` are C; compile them with the C
  compiler and include the header from C++ across its `extern "C"` guard. Never rename a
  generated file to `.cpp` or hand-edit it — regenerate from the `.proto`, which is the single
  source of truth for the wire format.
- **Pick the language per file, deliberately.** A file is C++ because it *benefits* from C++
  (RAII, a class); everything else — glue, generated code, anything that only pushes bytes
  around — is fine as C.

---

## 11. Day-to-day quirks

A short field guide to things that will otherwise cost you ten confused minutes:

- **`<cstdio>` and friends don't exist** under the minimal runtime — use the C header
  (`<stdio.h>`). See §5.
- **Set `CONFIG_STD_CPP17` explicitly** — the default is C++11 (§2).
- **Your IDE will show false errors.** clangd reads the GCC ARM compile flags
  (`-mfp16-format=ieee`, `-fno-reorder-functions`) and Zephyr macros (`K_SECONDS`) it can't
  parse, and flags them red — while the GCC build is clean. **The GCC build is the source of
  truth.** A `.clangd` file that removes the offending flags silences the noise.
- **Pristine-build after `CONFIG_CPP` or standard changes** — they're Kconfig changes and
  ripple through generated files.

---

## 12. The model in one paragraph

Zephyr is a C system with C++ available as an application language, and everything about using
it follows from two facts. First, you sit on a **C/C++ boundary**: names mangle on your side
and not the kernel's, so `extern "C"` reconciles them — automatically for the Zephyr headers
that wrap themselves in it, and by hand at the callback edge, where only free functions and
captureless lambdas cross and context travels through `user_data`. Second, you get
**freestanding** C++: a minimal runtime that provides `new`, vtables, and boot-time
constructors but no standard library, no exceptions, and no RTTI — so you include C headers,
return error codes instead of throwing, and allocate statically. Turn it on with `CONFIG_CPP`
plus an explicit standard, use C++ for RAII and structure rather than for a hosted
programming style, keep generated C as C, and the language buys you real safety on the target
without dragging a desktop runtime onto it.
