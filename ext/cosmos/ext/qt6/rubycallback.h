#ifndef RUBYCALLBACK_H
#define RUBYCALLBACK_H

#include <QObject>
#include <functional>
#include <QString>
#include <QModelIndex>

// moc cannot parse ruby.h, so hide it and give moc a stand-in for VALUE.
#ifndef Q_MOC_RUN
#  include <ruby.h>
#else
   typedef unsigned long VALUE;
#endif

// True while QApplication::exec() is running with the GVL released. Tells the
// callback whether it must re-acquire the GVL before touching Ruby.
// Nesting depth of blocking Qt event loops running with the GVL released
// (app.exec can contain a modal dialog.exec). Non-zero means released.
extern int g_qt_gvl_released;

// RAII for "this thread is about to release the GVL". Releasing it makes the
// thread-local "I already re-acquired the GVL" flag a lie: a callback fired
// from the nested Qt event loop would take the "I hold it" path and run Ruby
// with the GVL released. Clear the flag for the duration and restore it after.
// Also owns the g_qt_gvl_released depth counter so the two can't drift.
struct GvlReleaseScope {
  bool saved;
  GvlReleaseScope();
  ~GvlReleaseScope();
};

// Reports a contained Ruby exception (class, message, first frames) on stderr
// and clears it; returns true if there was one. `context` names the call site,
// e.g. "paintEvent", and appears in the message.
bool ruby_contain_error(int state, const char *context);

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
class RubyGenericSlot : public QObject {
public:
  RubyGenericSlot(VALUE proc, QObject *parent, const QList<int> &paramTypes)
    : QObject(parent), m_proc(proc), m_types(paramTypes) {}

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
