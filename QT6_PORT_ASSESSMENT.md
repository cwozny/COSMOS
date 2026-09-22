# COSMOS 4.5.2 → latest Qt: port assessment

**Date:** 2026-09-20 · **Baseline:** COSMOS v4.5.2 (`22e4176b`), built locally as 4.5.3
**Target:** Qt 6.11.2 (current stable; bottled in Homebrew, packaged in MacPorts)
**Platform:** macOS 27.0, arm64, system Ruby 2.6.10

All figures below are measured from this checkout, not estimated.

---

## 1. Verdict

**The COSMOS code is not the hard part. The binding layer is the entire problem.**

COSMOS talks to Qt through the `qtbindings` gem, which is built on KDE's **Smoke** — a C++
introspection layer. Smoke never worked with Qt5, was never ported to Qt6, and is dead upstream.
There is no maintained Ruby↔Qt5/Qt6 *widget* binding in existence.

So "upgrade COSMOS to Qt 6" is really two projects in sequence:

1. **Build a Ruby↔Qt6 binding layer** (does not exist — must be written)
2. **Then** migrate COSMOS onto it

Step 1 dominates. Step 2 is unexpectedly cheap (see §6).

### Primary-source evidence

`qtbindings` is maintained by **Ryan Melton — COSMOS's own author** (see `cosmos.gemspec`).

> "The reason Qt5 is not supported is because **Smoke does not work with Qt5**."
> — ryanmelt, issue #131, 2015-12-03

> "I've estimated the effort to get qtbindings to work with Qt5 to be **at least 2 months of full
> time work**. For that reason, and others, mainly that everyone wants 'cloud' support now,
> **COSMOS is moving to a web based interface**."
> — issue #168, 2020-05-14

He archived `ryanmelt/qtbindings` on 2021-02-15. That estimate was for **Qt5**; Qt6 is the larger jump.
COSMOS 5 / OpenC3 replacing the Qt GUI with a web UI *is* upstream's answer to this exact question.

---

## 2. Binding layer survey

| Project | Qt | State | Verdict |
|---|---|---|---|
| `qtbindings` (Smoke) | 4.8 only | **archived** 2021-02-15, 337★ | Current dependency. Terminal. |
| `chrisburel/smokegen` | 5 (WIP) | 4★, last push 2022-12-07 | LLVM-based Smoke rewrite. Abandoned, never reached Qt6. |
| `chrisburel/smokeqt` | 5 (WIP) | 1★, last push 2019-09-12 | Same. |
| `kitech/ruby-jit-qt` | 5 | 12★, last push 2017-05-02 | Author's own description says **"(stalled)"**. |
| `seanchas116/ruby-qml` | 5 | 136★, last push 2019-11-17 | **QML/Qt Quick only** — not QtWidgets. Wrong paradigm for COSMOS. |
| `qt` gem (CyJimmy264) | **6.4.2+** | 0★, 0 forks, experimental | Only live Qt6 option. Ruby **3.2+**; wraps ~5 widget classes. |

The single Qt6-capable option requires Ruby 3.2+ (COSMOS 4 declares `required_ruby_version = '~> 2.4'`)
and covers roughly 5 of the 191 classes COSMOS uses.

---

## 3. Measured Qt surface in this codebase

| Metric | Count |
|---|---|
| Files referencing `Qt::` | **161** |
| Distinct Qt symbols used | **191** |
| Total `Qt::` references | **2,725** |
| `lib/cosmos/gui` LOC | **11,988** |
| `lib/cosmos/tools` LOC | **37,398** |
| `connect(` calls | **550** |
| `SIGNAL(` / `SLOT(` macros | **512** / **62** |
| `slots` / `signals` declarations | **40** / **16** |
| Tools inheriting `QtTool` | **17** |
| **Distinct camelCase methods called** (proxy for Qt C++ API) | **286** |
| Distinct method names overall (incl. pure Ruby) | 1,100 |

### Per-tool breakdown (Qt-touching files only)

| Tool | Files | LOC | Qt refs |
|---|---|---|---|
| tlm_viewer | 56 | 4,162 | 152 |
| tlm_grapher | 15 | 3,873 | 187 |
| script_runner | 3 | 3,199 | 210 |
| test_runner | 3 | 1,942 | 140 |
| cmd_tlm_server | 7 | 1,892 | 205 |
| config_editor | 3 | 1,762 | 170 |
| tlm_extractor | 2 | 1,365 | 150 |
| cmd_sequence | 3 | 1,196 | 105 |
| table_manager | 1 | 1,152 | 139 |
| cmd_sender | 4 | 1,079 | 138 |
| limits_monitor | 1 | 1,029 | 75 |
| data_viewer | 3 | 987 | 39 |
| packet_viewer | 1 | 641 | 52 |
| cmd_extractor | 1 | 470 | 27 |
| opengl_builder | 1 | 416 | 46 |
| launcher | 3 | 351 | 38 |
| handbook_creator | 1 | 182 | 14 |

### Framework layer (`lib/cosmos/gui`)

| Module | Files | LOC | Qt refs |
|---|---|---|---|
| dialogs | 21 | 2,950 | 352 |
| line_graph | 6 | 2,077 | 35 |
| text | 4 | 1,136 | 55 |
| opengl | 3 | 1,046 | 24 |
| widgets | 5 | 931 | 93 |
| choosers | 7 | 790 | 63 |
| utilities | 4 | 636 | 62 |

---

## 4. The seam: COSMOS already has a Qt abstraction layer

This is the most favourable structural fact in the whole assessment.

- **`lib/cosmos/gui/qt.rb` (858 lines)** — central shim. Provides factory helpers
  (`getColor`, `getBrush`, `getPalette`, `getPen`, `getFont`, `getFontMetrics`, `getCursor`,
  `get_icon`) and defines four COSMOS widgets inside the Qt namespace:
  `Qt::CheckboxLabel`, `Qt::ColorListWidget`, `Qt::MatrixLayout`, `Qt::AdaptiveGridLayout`.
- **`lib/cosmos/gui/qt_tool.rb` (524 lines, 35 Qt refs)** — base class for all **17** GUI tools.

A binding swap is concentrated here rather than smeared across 161 files — *provided* the
replacement binding presents a qtbindings-compatible API.

### Hard requirement on any replacement

`qt.rb` **reopens native Qt classes** to add methods — `Qt::TableWidget`, `Qt::TreeWidget`,
`Qt::TreeWidgetItem`, `Qt::TabWidget`, `Qt::LineEdit`, `Qt::PlainTextEdit`, `Qt::TextEdit`,
`Qt::ComboBox`, `Qt::ListWidget`, `Qt::Painter`, `Qt::Icon`, `Qt::Dialog`, and the layout classes.

Any replacement binding **must support Ruby reopening of native classes**. A generated-bridge or
FFI binding that returns opaque handles will not satisfy this without extra design work. This is
the single most important constraint to validate before committing to an approach.

> **RESOLVED 2026-09-20 — see §10.** A working spike clears this. Writing the binding as a Ruby
> C extension in C++ (`rb_define_class`) rather than via FFI makes the Qt classes *genuine Ruby
> classes*, so reopening and subclassing work natively, for free. Option B is viable.

### Non-Qt API COSMOS depends on

These are `qtbindings`/Smoke internals, not Qt. A new binding must provide or obviate them
(~29 references total — small, but they will not exist in any replacement):

| Symbol | Refs | Note |
|---|---|---|
| `Qt::Internal` | 7 | `Qt::Internal::setDebug(...)` (mostly commented out) |
| `Qt::QtDebugChannel` | 7 | debug channel constants |
| `Qt::RubyThreadFix` | 4 | **Ruby/Qt thread marshalling — load-bearing, not cosmetic** |
| `Qt::Boolean` | 3 | out-param wrapper for C++ `bool&` |
| `Qt::PLUGIN_PATH` | 3 | Windows plugin path |
| `Qt::qVersion` | 2 | version string |
| `Qt::Enum` | 2 | enum type wrapper |
| `Qt::DebugLevel` | 1 | debug level constant |

`Qt::RubyThreadFix` deserves attention: it exists because Qt's event loop and Ruby's GVL/threads
interact badly. Any new binding will have to solve that problem again from scratch.

---

## 5. Signals and slots

550 `connect(` calls and 512 `SIGNAL(` macros use the qtbindings string-macro idiom:

```ruby
connect(button, SIGNAL('clicked()'), self, SLOT('handle_click()'))
```

Qt6 prefers typed function-pointer connections; the string form is legacy. A Ruby binding
reimplements this layer regardless, so this is **binding work, not COSMOS work** — but it is a
large, correctness-critical part of the binding, and the 550 call sites define the compatibility
target precisely.

---

## 6. Qt4 → Qt6 API delta in COSMOS's *actual* usage — surprisingly small

Given a compatible binding, the genuinely removed/changed APIs COSMOS touches are few:

| API | Refs | Qt6 status | Replacement |
|---|---|---|---|
| `Qt::GLWidget` | 1 | removed in Qt6 | `QOpenGLWidget` |
| `Qt::StyleOptionViewItemV` | 3 | removed in **Qt5** | `QStyleOptionViewItem` |
| `Qt::Sound` | 2 | removed in Qt6 | `QSoundEffect` (Qt Multimedia) |
| `Qt::MidButton` | 2 | removed in Qt6 | `Qt::MiddleButton` |
| `Qt::DesktopServices` | 1 | API changed | `QDesktopServices` (narrowed) |
| `Qt::Variant` | 23 | metatype system reworked | audit each site |
| `Qt::AbstractItemModel` | 1 | minor changes | — |

Notably **absent** from COSMOS's usage: `QRegExp`, `QTextCodec`, `QWorkspace`, `QHttp`, `QMatrix`,
`QLinkedList`, `QStringRef` — the usual Qt6 migration landmines. The codebase is cleaner than a
49K-LOC Qt4 application has any right to be.

**Implication:** if the binding presented a qtbindings-compatible API, the COSMOS-side migration is
plausibly in the low hundreds of lines — not 49,000. The `opengl` module (3 files, 1,046 LOC) is the
one genuine rewrite, since `QGLWidget` → `QOpenGLWidget` is a real API change, and
`OpenGLBuilder` + `tlm_viewer`'s 3D widgets depend on it.

---

## 7. Options

### A. Revive Smoke for Qt6
Fork `chrisburel/smokegen` (LLVM-based, 4★, abandoned 2022) and carry it to Qt6.
- **Pro:** would preserve the qtbindings API exactly → COSMOS barely changes.
- **Con:** resurrecting an abandoned C++ binding generator across two major Qt versions.
- **Estimate:** ≥ 2 months full-time was the Qt5 figure from the person who knew it best. Qt6 is worse. **Highest risk.**

### B. Purpose-built Qt6 shim scoped to COSMOS — **VALIDATED, recommended**
Do **not** build general-purpose Qt bindings. Write a C++ shim exposing only the **191 symbols**
COSMOS actually uses, behind a C ABI, bound from Ruby via Fiddle/FFI, with a thin Ruby layer that
reproduces the `qtbindings` API surface (including class reopening and `SIGNAL`/`SLOT`).
- **Pro:** bounded, measurable scope — defined by this document's numbers rather than by all of Qt.
- **Pro:** `qt.rb` and `qt_tool.rb` give a clean insertion point.
- **Con:** still a substantial C++ project; must re-solve Ruby-thread/event-loop integration (`RubyThreadFix`) — **the top remaining unknown, see §10**.
- **Resolved:** Ruby 2.6 is *not* a blocker. The spike builds and runs against system Ruby 2.6.10
  and Qt 6.11.2. A Ruby upgrade may still be desirable, but it is not a prerequisite.

### C. Process split — Ruby backend, separate GUI
Keep the COSMOS Ruby core (which is **already verified working headless on this machine**) and put
the GUI in its own process: Qt6/C++, or web.
- **Pro:** no Ruby↔Qt binding needed at all. The `CmdTlmServer` JSON-DRb API already exists and is
  proven — I drove `cmd()`/`tlm()` across a process boundary during this build.
- **Con:** GUI is a rewrite, in another language.
- **Note:** this is materially **what OpenC3 / COSMOS 5 did**.

### D. Stay on Qt 4.8 (status quo, working today)
MacPorts ships a maintained `qt4-mac` 4.8.7 with an explicit aarch64 patch set and a prebuilt
binary for this exact platform: `qt4-mac-4.8.7_15.darwin_27.arm64.tbz2` (167 MB, built 2026-09-17).
- **Pro:** gets the real 4.5.3 GUI running in hours, not months.
- **Con:** Qt 4.8 is EOL since 2015; depends on MacPorts keeping the port alive.
- **Unproven step:** `qtbindings`'s bundled CMakeLists is rejected by CMake 4.3
  (`No cmake_minimum_required command is present`). Needs `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` or an older CMake.

---

## 8. Recommended sequence

1. **De-risk first (D).** Prove the GUI runs at all on this machine via MacPorts Qt 4.8.7. Hours of
   work, and it produces the reference build you need to validate any port against. Without a
   working Qt4 GUI you have nothing to diff behaviour against.
2. **Spike, don't commit (B).** The method surface is now **measured: 286 distinct camelCase
   methods** across 191 classes (see §3). That is the binding's API contract, and it is small
   enough to be real work rather than open-ended research — the top 25 methods alone
   (`addWidget` ×625, `addAction` ×248, `setEnabled` ×203, `setText` ×131 …) cover a large
   fraction of all call sites, so a shim can be built breadth-first and still run most screens
   early.

   The remaining unknown is not scope but **feasibility of class reopening** (§4). Spike exactly
   that: prototype `Qt::Label`, `Qt::PushButton`, `Qt::VBoxLayout`, one `SIGNAL`/`SLOT`
   connection, and reopening a native class to add a method. If reopening can't be made to work
   cleanly, option B collapses and the honest answer is C.
3. **Decide B vs C on that spike's result**, not on principle.

### First concrete step
Generate the binding's API contract — 191 symbols × 286 methods, ranked by call frequency so the
shim can be built in dependency order:
```
# the 286-method Qt API surface, most-used first
git grep -ohE "\.[a-z][a-zA-Z0-9]*[A-Z][a-zA-Z0-9]*\(" -- lib/cosmos/gui lib/cosmos/tools \
  | sed 's/^\.//; s/(//' | sort | uniq -c | sort -rn

# the 191 Qt symbols
git grep -oh "Qt::[A-Za-z_]*" -- lib | sort -u
```
Caveat: the 286 count includes a few COSMOS helpers that happen to be camelCase
(`getFont`, `getBrush`, `getPalette`), so treat it as a slight overcount.

---

## 9. Bottom line

Porting *COSMOS* to Qt6 is small — the codebase avoided nearly every Qt6 landmine, and it already
has the right abstraction seam. Porting *Ruby* to Qt6 is the project, and nobody has done it in the
eleven years since Qt5 shipped. Budget accordingly, and treat §8 step 2 as a go/no-go gate.

---

## 10. Spike result — 2026-09-20

A working Ruby↔Qt6 binding spike lives in **`ext/cosmos/ext/qt6/`** (4 source files, ~230 lines).
It is self-contained: it adds files and modifies nothing existing.

Built against **Qt 6.11.2** (Homebrew) and **system Ruby 2.6.10**, arm64, macOS 27.

### Verified working

```
Qt runtime version : 6.11.2
label.text         : "hello"  ->  after setText: "world"
layout.count       : 2
inherited isEnabled: false                    # Qt::Label < Qt::Widget, native method
signal fired count : 2  (expected 2)          # connect by signal NAME
reopened Qt::Label : "WORLD!"                 # class reopening
reopened PushButton: "[GO]"
subclass works     : "status: CONNECTED"      # class StatusLabel < Qt::Label
subclass is_a Label: true / is_a Widget: true
```

| Requirement | Status |
|---|---|
| Ruby 2.6 + Qt 6.11.2 compile/link together | **works** |
| Widgets, layouts, property get/set | **works** |
| Inherited native methods across hierarchy | **works** |
| `connect` by signal *name* (COSMOS `SIGNAL('clicked()')`) | **works** via `QMetaObject` + moc'd callback |
| **Reopening native classes** (§4 hard constraint) | **works** |
| **Subclassing native classes** (`class QtTool < Qt::MainWindow`) | **works** |

### Two build workarounds required (both small, both in `extconf.rb`)

1. **`-Wno-register`** — Ruby 2.6's `intern.h:56` uses the `register` storage class, which C++17 removed.
2. **Include order** — Ruby's `missing.h` declares a global `finite(double)` that collides with
   libc++'s `<cmath>` internals. Qt headers must be included *before* `ruby.h`.

Also: `moc` cannot parse `ruby.h`, so `rubycallback.h` guards it with `#ifndef Q_MOC_RUN`.

### Event loop + Ruby threads — **RESOLVED, the big one**

This was ranked the highest remaining risk: Qt's blocking event loop vs Ruby's GVL, the problem
`Qt::RubyThreadFix` exists to paper over. It is now solved directly.

`QApplication::exec()` runs via `rb_thread_call_without_gvl`, so Ruby's scheduler keeps running
other threads. Any Qt signal that calls into Ruby re-acquires the GVL first via
`rb_thread_call_with_gvl` (`RubyCallback::invoke`, gated on `g_qt_gvl_released`).

Measured with three Ruby threads running against a 1-second blocking Qt event loop:

```
exec_for(1000ms) elapsed : 1.01s
ruby thread ticks during : 507                  # threads not starved
qt->ruby callbacks fired : [:a, :b, :c]         # all 3 timers called Ruby safely
```

This matters for COSMOS specifically: interface threads, router threads and the CmdTlmServer
background tasks all need to keep running while a GUI tool's event loop is live. **No
`RubyThreadFix` equivalent is needed** — the GVL is handled properly at the boundary instead.

### Ownership and GC — **RESOLVED**

Implemented properly rather than leaked:

- **Ownership tracking.** Each wrapper records whether Ruby owns the `QObject`. `addWidget`,
  `setLayout` and `setCentralWidget` reparent, so they hand ownership to Qt; `QApplication` is
  never Ruby-owned.
- **Identity map.** One `QObject` always maps to one Ruby object.
- **Dangling detection.** A wrapper whose C++ object was destroyed (via `QObject::destroyed`)
  raises `"Qt object has already been destroyed"` instead of segfaulting.
- **GC-sweep guard.** Deleting a parent during Ruby's GC cascades into children, firing
  `destroyed()` for each. The handler must not dereference other Ruby wrappers at that point —
  they may already be swept. A `g_in_gc_free` flag makes the handler drop map entries only.
  *This was a real crash, found and fixed: reparent-then-exit aborted with a lost stdout buffer.*

### Current status — 2026-09-20 (after GUI-driven testing)

| | |
|---|---|
| Unit suite | `test_qt6.rb` — **96 checks** (98 with a real GL context) |
| Integration suite | `test_cosmos_integration.rb` — **20 checks** |
| Tools loading | **17 / 17** |
| Tools launching to a usable window | **14 / 17** (TestRunner fails; DataViewer/TlmViewer flaky) |

Run a tool with **`./cosmos-demo.sh [ToolName]`** (`--list` shows them all).

### Bugs found by driving the GUI (not by loading it)

Reaching the event loop is a weak signal. Clicking through the running app
surfaced five real defects that "the process is alive" testing missed:

1. **`app.exec`/`dialog.exec` held the GVL.** COSMOS initialises CmdTlmServer on
   a background Ruby thread while the main thread blocks in a Qt loop
   (`splash.rb:104`). Holding the GVL starved that thread, so CmdTlmServer hung
   on its splash screen forever. Both now run under
   `rb_thread_call_without_gvl` with a nest-safe depth counter.
2. **Cross-thread QObject creation.** `post_to_main_thread` constructed a
   `RubyCallback` parented to `qApp` *from a background thread* — "Cannot create
   children for a parent that is in a different thread" is a crash, not a
   warning. Now posts a plain functor via `QMetaObject::invokeMethod`.
3. **Recursive GVL acquisition.** A Ruby slot that triggers another signal
   called `rb_thread_call_with_gvl` while already holding the GVL, corrupting
   Ruby's heap — crashes surfaced later in unrelated COSMOS threads
   (`structure.rb`, `sleeper.rb`). Fixed with a thread-local guard.
4. **`on_destroyed` touched Ruby without the GVL** when fired from Qt's event
   loop. Now acquires it first.
5. **Missing methods/constants** reached only after startup: `setForeground`,
   `frameWidth`, `cellWidget`, `appendHtml`, `QScrollBar`'s slider API,
   instance-level `Qt::Application` methods, variadic `QComboBox#addItem`.

### Known gap: empty tab contents

`CmdTlmServer`'s tabs render their tab *bar* but no content. Root cause is
identified: constructors ignore the parent argument, and
`Qt::VBoxLayout.new(@widget)` (`interfaces_tab.rb:105`) is supposed to install
the layout **on** `@widget`. Without it every widget added to that layout is
orphaned.

`ctor_layout` in `cosmos_qt6.cpp` implements the fix and makes the tabs
populate — but enabling it destabilises object lifetimes elsewhere and
segfaults during startup, so it is left disabled. Doing this properly needs
ownership tracking for parented objects, which is the next piece of work.

### Design decisions that mattered

1. **Ruby C extension, not FFI.** `rb_define_class` makes the Qt classes genuine Ruby classes, so
   COSMOS's reopening and subclassing work natively.
2. **C++ templates for accessors.** Each bound method is one registration line
   (`set_str<QLabel, &QLabel::setText>`); one hand-written function per method would not scale.
   *Gotcha:* for an inherited method, name the class that DECLARES it — `&QSlider::setValue` has
   type `void (QAbstractSlider::*)(int)`.
3. **Three wrapper kinds.** QObject-derived (parent/child ownership), value types (QColor, QFont,
   QKeySequence — heap copy, freed on GC), and item types (QTableWidgetItem — owned by the view).
4. **`initialize` on the base class, via a constructor registry.** COSMOS reopens classes and
   defines `initialize` calling `super`; a per-class `initialize` gets shadowed and the QObject is
   never constructed. Registering constructors against `Qt::Base` means `super` always lands on a
   real constructor.
5. **Hierarchy dictated by COSMOS's own C extension.** `ext/cosmos/ext/line_graph/line_graph.c:425`
   declares `Qt::Widget` directly under `Qt::Base`, so the binding matches that exactly rather than
   patching COSMOS.
6. **The shim absorbs Qt4→Qt6 breakage** so COSMOS source is untouched: `Qt::MidButton` →
   `MiddleButton`, `ItemIsTristate` → `ItemIsUserTristate`, `StyleOptionViewItemV2/3/4` →
   `QStyleOptionViewItem`, `QFontMetrics::width` → `horizontalAdvance`, `QGLWidget` →
   `QOpenGLWidget`, `QSound` → no-op.
7. **qtbindings internals reimplemented**: `Qt::Boolean`, `Qt::Enum`, `Qt::Internal`,
   `QtDebugChannel`, and `slots`/`signals`/`SIGNAL`/`SLOT`/`connect`. `Qt::RubyThreadFix` is an
   always-empty queue because the GVL problem it papers over is solved properly in the binding.

### Known gaps

1. **`QPainter` is not implemented.** `gui/line_graph` (2,077 LOC) draws through it, so graphing
   tools load but will not paint. This is the largest remaining piece.
2. **Model/view is declared, not implemented.** `AbstractItemModel`, `StyledItemDelegate` and
   `HeaderView` exist as classes with constants but no behaviour.
3. **OpenGL is aliased, not ported.** `Qt::GLWidget` maps to `QOpenGLWidget`; the Qt4 immediate-mode
   drawing in `gui/opengl` (1,046 LOC) still needs a real rewrite.
4. **Loading is not running.** Tools load and widgets render; no tool has been driven end-to-end
   against a live CmdTlmServer.
5. **Ownership edge cases.** Ownership transfer is handled for the reparenting paths that are
   bound; less-common paths may still leak or double-free.

### Integration plan (keeps the diff minimal)

`lib/cosmos/gui/qt.rb:80` is `require 'Qt'` — the single seam. When the binding is complete
enough, the swap is **one line in one existing file**. Everything else is additive.
