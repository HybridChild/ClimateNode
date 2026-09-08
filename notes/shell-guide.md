# Understanding the Zephyr shell

*A from-first-principles guide to what the `uart:~$` prompt actually is, how a command like `net iface` or `sensor get` comes to exist, and why device names tab-complete.*

A teaching document. It builds up from "the board has no screen and no debugger attached" to the handful of commands you type over [`./scripts/console.sh`](../scripts/console.sh) on this bench, using this repo's `net` and `sensor` shells as the running examples. Every code reference is real: this repo's code is cited by function name (it moves), and Zephyr's by `file:line` against v4.4.1 in `~/zephyr-workspace` (pinned, so those hold).

This guide has **no reference half** in `../docs/` — there is no project *decision* to record about the shell the way there is for the sensor or the MQTT client; the shell is a stock subsystem we simply switch on. What *this repo* does with it is two `prj.conf` lines (`CONFIG_SHELL`, `CONFIG_NET_SHELL`) and one more that pulls a shell in as a side effect (`CONFIG_SENSOR_SHELL`), all covered below. The sensor shell's own quirks — its command table and the RTIO detour it drags in — stay in [`sensor-api-guide.md` §7](sensor-api-guide.md), because those are sensor concerns; this guide is the general machinery underneath them.

**Prerequisites:** what a Kconfig option is ([`zephyr-build-system-guide.md`](zephyr-build-system-guide.md) §5). Everything else — the backend, the command tree, the linker section commands are registered through, dynamic completion — is built from nothing.

**The shape of this document:**

- **§1** — why a shell exists at all: the board has no keyboard, and you still need to poke it while it runs.
- **§2** — the two things people call "the console": the raw byte pipe, and the command interpreter that sits on top of it.
- **§3** — how a command comes to exist. The registration model, the linker trick behind it, and the handler signature every command shares.
- **§4** — subcommands and the command tree; where `sensor get` splits into `sensor` and `get`.
- **§5** — tab completion, and the *dynamic* subcommands that make `scd40@62` complete even though no one typed it into a table.
- **§6** — the built-in shells you already have on this bench: `kernel`, `device`, `net`, `sensor`.
- **§7** — what it costs and the knobs worth knowing.
- **§8** — cheat sheet.
- **§9** — lab: exercises on the real board.
- **§10–§11** — the whole model in a paragraph, and where to go next.

---

## 1. The problem being solved

The Nucleo has no screen, no keyboard, and — unless you set one up — no debugger halting it at breakpoints. It boots, runs your two threads forever, and the only window into it is a single serial line carried back to your PC over the ST-LINK's USB. `printf` can send text *out* through that line (that is what `CONFIG_LOG` and `printk` do). But logging is a monologue: the firmware decides what to say and when. You cannot ask it a question.

Plenty of debugging needs a question. *Is the Ethernet link even up? What IP did the board settle on? What does the sensor read right now, independent of my application code?* You could add a log line for each and reflash — a two-minute round trip to answer a yes/no — or you could give the firmware a **command interpreter**: a component that reads text arriving on that same serial line, matches it against a table of commands, and runs the matching one.

That interpreter is the Zephyr **shell**. It is not a debugger and not a separate program; it is a subsystem compiled *into* your firmware, running as one more thread, turning lines of text you type into function calls inside the running image. `net iface` is not a Zephyr built-in in the way an `ls` binary exists on Linux — it is a C function in your image that a line of text happened to name.

That reframing is the whole guide: **a shell command is a registered function, and typing its name is how you call it.** Everything else is plumbing around that sentence.

---

## 2. Two things called "the console"

It pays to separate two layers that both get called "the console," because the shell is only the upper one.

**The lower layer — the byte pipe.** The STM32's UART peripheral is wired to the ST-LINK chip on the Nucleo, which bridges it to USB and presents a virtual COM port on your PC. `./scripts/console.sh` opens that port at 115200 baud. This layer moves raw bytes in both directions and understands nothing about them. `CONFIG_STDOUT_CONSOLE=y` in [`prj.conf`](../gateway/prj.conf) is what points C's `printf` at this pipe. With *only* this layer, you get log output and nothing to type at.

**The upper layer — the shell.** `CONFIG_SHELL=y` adds a command interpreter that *claims* a backend to read from and write to. On this bench the backend is the serial one (`SHELL_DEFINE(shell_uart, …)`, `subsys/shell/backends/shell_uart.c:527`), so the shell and the raw console share the same UART. The shell runs in **its own thread** (`shell_thread`, `subsys/shell/shell.c:1355`, started by the `k_thread_create` at `shell.c:1419`): that thread blocks waiting for bytes, echoes what you type, handles backspace and history and Tab, and when you press Enter, parses the line and dispatches it. The prompt string it prints — `uart:~$ ` — is just a Kconfig default (`subsys/shell/backends/Kconfig.backends:37`).

So the mental split is: the **backend** is the wire, the **shell** is the interpreter, and a **command** is an entry in the interpreter's table. The rest of the guide is about that table.

> A subtlety worth naming once: because the shell and `printf` logging share one UART, log lines can appear mid-prompt while you are typing. That is cosmetic — the shell redraws the line — but it is why bench output sometimes interleaves a `[00:00:12.345] <inf>` line with your half-typed command.

---

## 3. How a command comes to exist

Here is the surprising part, and the one that makes the rest click: **commands are not registered by any startup code you can point to.** There is no `shell_add_command()` call in `main()`. Instead each command is placed into a dedicated region of the binary at *link* time, and at boot the shell walks that region.

Look at what `SHELL_CMD_REGISTER` actually expands to (`include/zephyr/shell/shell.h:447`, via `SHELL_CMD_ARG_REGISTER` at `:390`):

```c
static const struct shell_static_entry _shell_net = SHELL_CMD_ARG(net, …);
static const TYPE_SECTION_ITERABLE(union shell_cmd_entry,
        shell_cmd_net, shell_root_cmds, shell_cmd_net) = {
    .entry = &_shell_net,
};
```

`TYPE_SECTION_ITERABLE` is the key. It tells the linker: *put this object in the section named `shell_root_cmds`, alongside every other object anyone else put there.* Every `SHELL_CMD_REGISTER` in the entire firmware — yours, the networking subsystem's, the sensor driver's — drops one entry into that same section. The linker gathers them into one contiguous array. At runtime the shell iterates that array to know what root commands exist.

This is why a driver you never call into can still add a `sensor` command just by being compiled in: registration is a **link-time** act, not a runtime one. It is the same iterable-section pattern Zephyr uses for devices, init hooks, and zbus observers — once you recognise it here, you will see it everywhere in the tree.

**The handler.** Every command points at a function with one fixed signature (`shell_cmd_handler`, `include/zephyr/shell/shell.h:251`):

```c
int handler(const struct shell *sh, size_t argc, char **argv);
```

If that looks like `main(argc, argv)`, that is exactly the intuition. `argv[0]` is the command's own name; `argv[1]` onward are the whitespace-separated tokens the user typed. The shell has already split the line for you. `sh` is the handle you print *through* — use `shell_print(sh, …)`, `shell_error(sh, …)`, `shell_info(sh, …)` rather than bare `printf`, so the output goes to the backend that invoked the command and is coloured by severity. The return value matters: `0` means success, `-EINVAL` means "bad arguments" (the shell can print usage), `-ENOEXEC` means "I did not run." A minimal command is nothing more than:

```c
static int cmd_hello(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "hello, argc=%zu", argc);
    return 0;
}
SHELL_CMD_REGISTER(hello, NULL, "Say hello", cmd_hello);
```

Compile that in and `hello` works at the prompt. No wiring, no header, no call site — the `SHELL_CMD_REGISTER` line *is* the wiring. (This repo does not register any custom commands; every command you see on the bench comes from a subsystem. But the mechanism is this small, which is the point of showing it.)

---

## 4. Subcommands: the command tree

In §3 a command was a single word — `hello` — bound to a single function. Real shells rarely stay that flat. `net iface`, `net ping`, `sensor get`, `sensor attr_set`: two words, and the first one groups the second. This is the same idea as `git commit` and `git push` — `git` is not really a command you run on its own, it is a **namespace**, and `commit` and `push` are the actual actions grouped under it. Zephyr's shell works exactly this way. The first word is a **root** command; the second is a **subcommand** of it. Together they form a small tree, and this section is about how you build one.

### 4.1 A tree from scratch

Forget the real Zephyr code for a moment and grow §3's example one level. Say we want `light on` and `light off`. We write the two leaf functions exactly as before, then a *group* to hang them under, then a root that points at the group:

```c
static int cmd_light_on(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "light on");
    return 0;
}
static int cmd_light_off(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "light off");
    return 0;
}

/* 1. the group: the set of children that live under `light` */
SHELL_STATIC_SUBCMD_SET_CREATE(sub_light,
    SHELL_CMD(on,  NULL, "Turn the light on",  cmd_light_on),
    SHELL_CMD(off, NULL, "Turn the light off", cmd_light_off),
    SHELL_SUBCMD_SET_END
);

/* 2. the root, wired to that group */
SHELL_CMD_REGISTER(light, &sub_light, "Light control", NULL);
```

Compile that in and `light on` and `light off` work at the prompt. Four pieces are doing the work, and each is worth one sentence:

- **`SHELL_STATIC_SUBCMD_SET_CREATE(sub_light, …)`** (`include/zephyr/shell/shell.h:484`) declares the group of children and gives it a name — `sub_light` — that is just a handle you will hand to the root on the next line. The word **static** means the list of children is fixed at compile time; §5 introduces the *dynamic* alternative, where the children are computed while the board runs.
- **`SHELL_CMD(word, subcmds, help, handler)`** is one child. `on` is the word the user types after `light`; `cmd_light_on` is the function it calls; the `"…"` string is the help text shown by `light -h`. The second argument — `NULL` here — is *that child's own* group of children; `NULL` means it has none, so `on` is a leaf, the end of a branch.
- **`SHELL_SUBCMD_SET_END`** (`:573`) closes the list. It is not optional: the set stores no count anywhere, so the shell reads children one after another until it reaches this sentinel. Omit it and the shell reads off the end of the array.
- Back on the root line, **`SHELL_CMD_REGISTER(light, &sub_light, …)`** is the same macro from §3 — but where `hello` passed `NULL` for its children, `light` passes `&sub_light`. That one argument is the whole difference between a flat command and a tree. And notice `light`'s *handler* (the last argument) is `NULL`: running `light` by itself does nothing, because a namespace is not an action — so there is no function to give it, and the shell responds to a bare `light` by listing its children instead. (If you *did* want `light` alone to do something, you would give it a real handler here.)

That is the entire mechanism. A root points at a set; a set is children plus a terminator; a child is a word, an optional sub-set, and a function. Everything else is this shape repeated.

### 4.2 The same shape in the wild

Zephyr's own `device` shell is precisely the pattern above (`subsys/shell/modules/device_service.c:250`):

```c
SHELL_STATIC_SUBCMD_SET_CREATE(sub_device,
    SHELL_CMD_ARG(list, &dsub_device_name_lookup, LIST_CMD_HELP, cmd_device_list, 1, 1),
    /* …conditional pm/init subcommands… */
    SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(device, &sub_device, "Device commands", NULL);
```

Line for line it is our `light` example: a set named `sub_device`, one child (`list`), the terminator, and a root wired to the set with a `NULL` handler. Two differences are worth naming:

- **`SHELL_CMD_ARG`** (`:601`) instead of `SHELL_CMD`. It is the same macro plus two trailing numbers — here `1, 1` — the count of **mandatory** and **optional** arguments the child accepts. The shell checks the count *before* calling your handler and rejects a wrong-count line itself, which is why handlers can trust `argc` without re-validating it. Use `SHELL_CMD_ARG` when a subcommand takes arguments, `SHELL_CMD` when it does not.
- **`&dsub_device_name_lookup`** where our `on` had `NULL`. The child `list` names its *own* sub-set, so the tree keeps going — that is how you get a third level, and it is also the hook that feeds tab-completion. Both are the subject of §5.

The sensor shell is the same shape again, one level richer: [`sensor-api-guide.md` §7.1](sensor-api-guide.md) lists its full subcommand table, and its `sub_sensor` set is at `drivers/sensor/sensor_shell.c:1130`, registered as a root at `:1147`. You are now equipped to read `sensor get` for what it is: the root `sensor` (one entry in the linker section from §3) plus the child `get` (one `SHELL_CMD_ARG` inside `sub_sensor`), whose handler is `cmd_get_sensor`.

---

## 5. Tab completion and dynamic subcommands

Every subcommand set from §4 quietly answers one question: *given what has been typed so far, which words may come next?* That is the question Tab asks out loud. Press Tab after `net ` and the shell answers `iface`, `ping`, and the rest — it just reads them off the `net` set, the fixed array someone wrote by hand at compile time. Nothing new is needed: the answer was baked into the binary.

### 5.1 When the answer isn't known until runtime

Now press Tab after `sensor get ` and the shell offers `scd40@62`. That string is in no array anywhere. It is a **devicetree node name**, and *which* devices a board has depends on the board and its overlay — a fact that does not exist until the firmware is running on real hardware. You cannot precompute this list the way `net`'s children were precomputed. The set of valid next words has to be *generated on the spot*, each time Tab is pressed.

So Zephyr lets a subcommand set be backed by a **function** instead of an array. Spell out the children when you know them at compile time (the §4 way); point at a function when you don't (this way). Zephyr calls the second kind a **dynamic** subcommand set, and everything else about it — how a parent references it, how completion consumes it — is identical to the static kind. Only the source of the children differs.

### 5.2 The function is an iterator

`SHELL_DYNAMIC_CMD_CREATE(name, get)` (`include/zephyr/shell/shell.h:581`) is the dynamic counterpart to `SHELL_STATIC_SUBCMD_SET_CREATE`: it makes a named set you can hand to a parent, but where the static macro took a list of children, this one takes a single function, `get`. That function has the `shell_dynamic_get` signature (`:96`):

```c
void get(size_t idx, struct shell_static_entry *entry);
```

Notice the shell never asks for the whole list at once. It asks for **one candidate at a time**, by index: "give me candidate `0`," then "candidate `1`," then `2`, and so on. Each call fills in the passed-in `entry` — the candidate's word (`syntax`), its help, and its own children — for that one index. When `idx` runs past the last real candidate, the function writes `entry->syntax = NULL`, which is how it tells the shell *"that's the end, stop asking."* The function is, in other words, an **iterator the shell drives**: it turns "the list of devices right now" into answers to "what is item `idx`?"

Here is the sensor shell's actual generator, the one behind `dsub_device_name` (`drivers/sensor/sensor_shell.c:866`, one bookkeeping line elided):

```c
static void device_name_get(size_t idx, struct shell_static_entry *entry)
{
    const struct device *dev = shell_device_filter(idx, sensor_device_check);

    entry->syntax  = (dev != NULL) ? dev->name : NULL;  /* idx-th sensor, or NULL to stop */
    entry->handler = NULL;
    entry->help    = NULL;
    entry->subcmd  = &dsub_channel_name;                /* what may follow the device name */
}
SHELL_DYNAMIC_CMD_CREATE(dsub_device_name, device_name_get);
```

Read it against the paragraph above. `shell_device_filter(idx, …)` returns the `idx`-th device that passes a sensor check, or `NULL` once they run out — so `entry->syntax` is either that device's name or the `NULL` stop signal, exactly the protocol described. The list is built from the board's *live* device table at the instant you press Tab, which is why it reflects reality and not a table someone maintained by hand.

### 5.3 Where it plugs in

That last line — `entry->subcmd = &dsub_channel_name` — is the payoff of §4 meeting §5. Each device-name candidate is handed its *own* subcommand set (the channel names), so the tree keeps branching: complete `scd40@62`, and the next Tab offers `co2`, `ambient_temp`, `humidity`, because those are `dsub_channel_name`'s children. And `dsub_channel_name` is *itself* dynamic, generated the same way one level down.

Step back and the whole thing is one idea: **a subcommand set is a subcommand set, whether its children come from an array or from a function.** A parent references it the same way (`&dsub_device_name`, just like `&sub_light` in §4); the shell walks it the same way for completion. Static and dynamic are interchangeable wherever §4 expected a set — that interchangeability is the entire trick, and it is why `sensor get`'s `get` child could name `&dsub_device_name` as its children without caring that the list is computed at runtime.

The practical payoff at the prompt: you rarely type a device or channel name in full. Type `sensor get s`, Tab, and the node name completes itself — and because the candidates come from the live device table, a name that *fails* to complete is telling you the device is not actually there, before you run a single command against it.

---

## 6. The built-in shells on this bench

Nothing above required writing a command; the value on this bench comes from the shells other subsystems already register once you enable them. Five are worth knowing:

| Root | Enabled by | What it answers | Registered at |
| --- | --- | --- | --- |
| `kernel` | comes with `CONFIG_SHELL` | threads, uptime, stacks, reboot | `subsys/shell/modules/kernel_service/kernel_shell.c:10` |
| `device` | comes with `CONFIG_SHELL` | what devices exist and their init status | `subsys/shell/modules/device_service.c:269` |
| `net` | `CONFIG_NET_SHELL=y` | interface state, IP/MAC, `net ping` | `subsys/net/lib/shell/net_shell.c:235` |
| `sensor` | `CONFIG_SENSOR_SHELL=y` | read channels/attributes straight from a driver | `drivers/sensor/sensor_shell.c:1147` |
| `can` | `CONFIG_CAN_SHELL=y` | controller state and error counters, send a frame, add a receive filter | `drivers/can/can_shell.c:1242` |

Three of these earn their place in this project specifically:

- **`net`** is how the bench proves the link. `net iface` shows whether the Ethernet PHY has linked and which static IPv4 address `net_config` applied; `net ping 192.168.10.1` sends ICMP *from the board* to the Pi, verifying the Nucleo→Pi direction that a ping from the Pi cannot. `prj.conf` turns it on with `CONFIG_NET_SHELL=y`, and the comment there records exactly this use. See [`communication-guide.md`](communication-guide.md) for the networking side.
- **`sensor`** is a ground-truth read of the SCD-40 that bypasses your application entirely — no zbus channel, no protobuf, no MQTT. That is what makes it a pipeline bisector, and it is the reason `CONFIG_SENSOR_SHELL=y` is in the build at all. The details, and the RTIO machinery that flag quietly pulls in, are [`sensor-api-guide.md` §7](sensor-api-guide.md).
- **`can`** does for the CAN link what `net` does for Ethernet, and rather more, because a CAN controller can be driven end to end from the prompt with no application code: set a bitrate, enter loopback mode, send a frame, install a receive filter, watch what arrives. `can show` also reports the error counters and the controller state, which is the difference between knowing *that* a link is unhappy and knowing *which end*. [`can-guide.md` §10](can-guide.md) is built on it, and [`can-bringup.md`](../docs/can-bringup.md) uses it as the bisect between firmware and wiring. One trap comes with it: an application that installs its own receive filter first will shadow `can dump`, because real controllers deliver a frame to only the lowest matching filter — the mechanism is in [`can-guide.md` §7](can-guide.md).

`kernel` and `device` come *free* with `CONFIG_SHELL` — you did not ask for them, but `kernel threads` (which threads exist, their stacks and states) and `device list` (did every device initialise?) are both useful when something hangs at boot.

---

## 7. What it costs, and the knobs

The shell is not free. It brings its own thread (a stack you are paying for), a line buffer, command history, and the iterable command sections. On a 2 MB-flash STM32H753 that is noise; on a tighter part it is a real line item, and it is common to gate `CONFIG_SHELL` behind a debug build.

This repo has both parts in one place. The gateway simply turns the shell on. The peer node — 16 KB of RAM — cannot: a **stock** shell with default settings takes it from 65 % to **95 %** of RAM before a single command set is added, and `CONFIG_SENSOR_SHELL` does not link at all, overflowing by 5856 B. So the peer's shell is a build variant rather than a default, and every knob below appears in [`peer-node/debug.conf`](../peer-node/debug.conf) with the bytes it saves written next to it, bringing the same shell plus the `can` commands back down to 83 %. Read that file alongside this section; it is this section with the numbers filled in.

A few knobs you may meet:

- **`CONFIG_SHELL_STACK_SIZE`** — the shell thread's stack. If a command handler does something heavy (deep formatting, large locals) and the board faults *only* when run from the shell, this is the first suspect.
- **`CONFIG_SHELL_CMD_BUFF_SIZE`** — the maximum line length the shell will accept. Long `attr_set` lines can bump it.
- **`CONFIG_SHELL_HISTORY`** / **`CONFIG_SHELL_HISTORY_BUFFER`** — the up-arrow history and its size.
- **`CONFIG_KERNEL_SHELL`, `CONFIG_DEVICE_SHELL`** — these default on with `CONFIG_SHELL`; turn them off to reclaim the flash if you never use `kernel`/`device`.

One cost is indirect and worth repeating from the sensor guide: `CONFIG_SENSOR_SHELL` `select`s `SENSOR_ASYNC_API` and `CBPRINTF_FP_SUPPORT`, so enabling the sensor shell pulls in RTIO and floating-point `printf` formatting whether or not the rest of your app wants them. That is a sensor-shell property, not a general shell one, and it is explained in full at [`sensor-api-guide.md` §7.2–§7.3](sensor-api-guide.md).

---

## 8. Cheat sheet

```text
# --- what the pieces are ---
backend                 the wire (UART over ST-LINK) the shell reads/writes
shell thread            reads bytes, echoes, handles Tab/history, dispatches on Enter
root command            top-level word; one entry in the shell_root_cmds linker section
subcommand set          array of children, terminated by SHELL_SUBCMD_SET_END
dynamic subcommand      children generated by a function at runtime (device names, …)

# --- registering a command (compile-time; no call site) ---
SHELL_CMD_REGISTER(name, &subcmds_or_NULL, "help", handler);
SHELL_STATIC_SUBCMD_SET_CREATE(sub_x, SHELL_CMD_ARG(child, …), …, SHELL_SUBCMD_SET_END);
int handler(const struct shell *sh, size_t argc, char **argv);   // 0 ok, -EINVAL, -ENOEXEC
shell_print(sh, …) / shell_error(sh, …) / shell_info(sh, …)      // not bare printf

# --- at the prompt (uart:~$) ---
<Tab>                   complete the current word (static and dynamic)
<Up>/<Down>             command history
help                    list root commands;  <cmd> help  or  <cmd> -h for a command's help
kernel threads          list threads, stacks, states
kernel uptime           milliseconds since boot
device list             every device and whether it initialised
net iface               interface state, IPv4 address, MAC
net ping <ip>           ICMP from the board
sensor get <dev> <chan...>   read named channels straight from the driver
```

---

## 9. Lab — exercising the shell on the board

These run over [`./scripts/console.sh`](../scripts/console.sh) (115200 baud; quit with Ctrl-A then K). They need the board flashed with the current `prj.conf`. Each says what it **proves**.

### Exercise 1 — the prompt is a running thread, not a menu

At the prompt, ask the kernel to describe itself:

```console
uart:~$ kernel threads
```

You should see a list that includes a `shell_uart` thread alongside `main` and the sensor thread. Then:

```console
uart:~$ kernel uptime
Uptime: 42134 ms
```

**Proves:** the shell is not a static menu baked into a boot ROM — it is a thread in *your* image, listed among your own threads, answering live. The uptime climbs every time you ask.

### Exercise 2 — completion is generated, not typed

Type the following but press **Tab** where marked instead of typing the rest:

```console
uart:~$ sensor get s<Tab>
```

The node name completes to `scd40@62`. Now break it — type a label that does not exist and press Tab:

```console
uart:~$ sensor get xyz<Tab>
```

Nothing completes.

**Proves:** the candidate list is computed from the board's actual devicetree at the moment you press Tab (the dynamic subcommand of §5), not read from a fixed table — a name completes only if the device is really present, so completion doubles as a presence check.

### Exercise 3 — a command answers the network question logging cannot

```console
uart:~$ net iface
```

Read off the interface state (`oper state: UP` when the cable is in) and the IPv4 address — it should be `192.168.10.2`, the static address from `prj.conf`. Then ping the Pi *from the board*:

```console
uart:~$ net ping 192.168.10.1
```

**Proves:** the shell lets you interrogate state the firmware never volunteered — the link came up and `net_config` applied the address — and `net ping` verifies the board→Pi direction, which a ping issued on the Pi cannot. This is the question a monologue of log lines could not answer (§1).

### Exercise 4 — one root command, two sources

```console
uart:~$ help
```

The list includes `kernel` and `device` (free with `CONFIG_SHELL`), `net` (from `CONFIG_NET_SHELL`), `sensor` (from `CONFIG_SENSOR_SHELL`) and `can` (from `CONFIG_CAN_SHELL`) — five roots, registered by five different pieces of code you never call directly.

**Proves:** registration is a link-time act (§3). Each subsystem dropped its root into the same `shell_root_cmds` section just by being compiled in; the `help` list is that section read back.

---

## 10. The model in one paragraph

A Zephyr shell is a command interpreter compiled into your firmware and run as its own thread, reading and writing over a backend — here the same UART your logs use, reached by `console.sh`. A command is a plain function with an `argc`/`argv` signature, bound to a name by `SHELL_CMD_REGISTER`, which places one entry into an iterable linker section that the shell walks at boot — so registration is a link-time act with no call site, and any subsystem you compile in can add commands. Commands form a tree: a root points at a subcommand set, and a set can be static (an array known at compile time) or dynamic (candidates generated by a function at runtime, which is how device names complete). On this bench that machinery gives you `kernel`, `device`, `net`, and `sensor` for the price of a few Kconfig lines, and each is a way to *ask the running board a question* that logging, being one-directional, cannot.

---

## 11. Where to go next

- **[`sensor-api-guide.md` §7](sensor-api-guide.md)** — the `sensor` shell in full: its command table, its output format, and the RTIO detour `CONFIG_SENSOR_SHELL` pulls in. Read it now that the general shell machinery is clear; §7 stops re-explaining the parts covered here.
- **[`communication-guide.md`](communication-guide.md)** — the networking the `net` shell interrogates: what "interface up" and that static address actually mean.
- **[`zephyr-build-system-guide.md`](zephyr-build-system-guide.md)** — the Kconfig `select` mechanism (why one `CONFIG_` forces others on) and the devicetree that `device list` and dynamic completion read from.
- **Zephyr shell subsystem** — `subsys/shell/` in `~/zephyr-workspace/zephyr`. The macros in `include/zephyr/shell/shell.h` are readable; start at `SHELL_CMD_REGISTER` (`:447`) and follow it down to the linker-section expansion.
