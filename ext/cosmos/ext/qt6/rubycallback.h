#ifndef RUBYCALLBACK_H
#define RUBYCALLBACK_H

#include <QObject>
#include <functional>
#include <QString>
#include <QModelIndex>

// moc cannot parse ruby.h, so hide it and give moc a stand-in for VALUE.
#ifndef Q_MOC_RUN
#  include "ruby_compat.h"
#else
   typedef unsigned long VALUE;
#endif

// Runs a blocking Qt call (a modal exec, a static file dialog) with the GVL
// released, so Ruby's background threads keep running while it is up.
// Whether a callback needs to re-acquire the GVL is no longer tracked in our
// own state -- ruby_thread_has_gvl_p() answers it directly.
// On the GUI thread, a Ruby interrupt (a signal, Thread#raise) is serviced
// inside the loop; an exit-class exception it raises (SystemExit, Interrupt,
// SignalException) ends every event loop and waits for
// ruby_raise_pending_exit.
void ruby_without_gvl(const std::function<void()> &fn);

// Raises the exit-class exception that ended the event loops, if any. Call it
// with the GVL held, after the loop's C++ objects are out of scope: the raise
// longjmps past the caller's frame, skipping destructors (a parented stack
// QMessageBox left behind would be deleted again by its parent).
void ruby_raise_pending_exit(void);

// After rb_protect caught something (state != 0): if it is exit-class
// (SystemExit, Interrupt, SignalException), clears it, ends every event loop
// and leaves it for ruby_raise_pending_exit; returns true. Anything else is
// left in rb_errinfo for the caller. Exit-class exceptions must end the app,
// not be reported like a slot error and swallowed.
bool ruby_defer_exit_exception(int state);

// Reports a contained Ruby exception (class, message, first frames) on stderr
// and clears it; returns true if there was one. `context` names the call site,
// e.g. "paintEvent", and appears in the message.
bool ruby_contain_error(int state, const char *context);

// GC anchors for Ruby blocks C++ holds as a bare VALUE: a connected slot's
// block, a posted one-shot. Counted, since one block can be held more than
// once. Neither call touches Ruby, so both are safe without the GVL -- a
// slot's destructor runs whenever Qt deletes the sender, often with the GVL
// released. Defined in cosmos_qt6.cpp, which marks the anchored blocks.
void ruby_anchor_proc(VALUE proc);
void ruby_release_proc(VALUE proc);

// Converts one signal argument (QMetaType id + raw pointer) to a Ruby VALUE.
// Defined in cosmos_qt6.cpp, where the bound class objects live.
VALUE ruby_value_from_meta(int typeId, void *data);

// Calls a Ruby proc, acquiring the GVL first when a Qt event loop holds it.
void ruby_invoke_proc(VALUE proc);
// Runs fn with the GVL held, acquiring it only if needed on this thread.
void ruby_run_with_gvl(void *(*fn)(void *), void *arg);

// Same, for a C++ callable. Qt fires widget callbacks from its event loop with
// the GVL released, and EVERY VALUE a callback touches -- including the
// arguments it builds -- must be created under the GVL. Wrap the whole body,
// argument construction included, rather than acquiring it further in.
// If the calling thread is not known to Ruby, fn does not run at all.
void ruby_with_gvl(const std::function<void()> &fn);

// Bridges any Qt signal into a Ruby proc, with ALL of its arguments.
//
// The previous approach declared one moc slot per parameter type, which meant
// only int/bool/QString/QModelIndex first-arguments were forwarded and every
// argument after the first was dropped -- so e.g. customContextMenuRequested
// (QPoint) delivered nil and cellEntered(int,int) lost the column.
//
// Deliberately NO Q_OBJECT here: we override qt_metacall by hand to receive
// arbitrary signals, and moc would generate a conflicting definition. Without
// Q_OBJECT the class uses QObject's static metaobject, so the first free slot
// index is QObject's own methodCount().
// The slot is parented to the sender and holds its block's anchor for as long
// as it lives, so the block goes when the sender does. Anchoring every block
// for the life of the process kept whatever each one captured -- a closed
// TlmViewer screen's whole widget tree -- reachable for good.
class RubyGenericSlot : public QObject {
public:
  RubyGenericSlot(VALUE proc, QObject *parent, const QList<int> &paramTypes)
    : QObject(parent), m_proc(proc), m_types(paramTypes) { ruby_anchor_proc(proc); }
  ~RubyGenericSlot() override { ruby_release_proc(m_proc); }

  int qt_metacall(QMetaObject::Call c, int id, void **a) override;

  static int slotIndex() { return QObject::staticMetaObject.methodCount(); }

private:
  VALUE m_proc;
  QList<int> m_types;
};

// Bridges a Qt signal back into a Ruby proc. Needs Q_OBJECT/moc so QMetaObject
// can resolve invoke() as a slot at runtime -- that is what lets us connect by
// signal *name*, the way COSMOS's SIGNAL('clicked()') idiom requires.
class RubyCallback : public QObject {
  Q_OBJECT
public:
  explicit RubyCallback(VALUE proc, QObject *parent = nullptr)
    : QObject(parent), m_proc(proc) {}
  VALUE m_proc;
  void dispatch(int argc, VALUE *argv);
public slots:
  void invoke();
  // Signals carry arguments (currentChanged(int), activated(QModelIndex), ...).
  // One slot per common parameter type; connect() picks the match from the
  // signal's own metadata.
  void invoke_int(int v);
  void invoke_str(const QString &v);
  void invoke_bool(bool v);
  void invoke_idx(const QModelIndex &v);
};

#endif
