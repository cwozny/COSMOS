// Spike: minimal Ruby <-> Qt6 binding for COSMOS.
// Goal is to answer the go/no-go questions from QT6_PORT_ASSESSMENT.md section 4/8:
//   1. can we drive Qt6 widgets from Ruby 2.6?
//   2. does connect-by-signal-name work (COSMOS's SIGNAL('clicked()') idiom)?
//   3. can Ruby REOPEN these native classes to add methods? (the hard constraint)

// Include order matters: Ruby 2.6's missing.h declares a global finite(double)
// that collides with libc++'s <cmath> internals. Qt pulls in <cmath> via
// qglobal.h, so Qt must be seen before ruby.h.
#include <QApplication>
#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QMainWindow>
#include <QDialog>
#include <QFrame>
#include <QGroupBox>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QAction>          // QtGui in Qt6, not QtWidgets
#include <QKeySequence>
#include <QVariant>
#include <QFont>
#include <QColor>
#include <QSize>
#include <QPoint>
#include <QSizePolicy>
#include <QMessageBox>
#include <QFileDialog>
#include <QCoreApplication>
#include <QTableWidget>
#include <QTreeWidget>
#include <QListWidget>
#include <QTabWidget>
#include <QSplitter>
#include <QScrollArea>
#include <QScrollBar>
#include <QFormLayout>
#include <QMenu>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QProgressBar>
#include <QRadioButton>
#include <QSlider>
#include <QSpinBox>
#include <QTextEdit>
#include <QHeaderView>
#include <QPalette>
#include <QCursor>
#include <QTextCursor>
#include <QAbstractItemView>
#include <QInputDialog>
#include <QShortcut>
#include <QPen>
#include <QBrush>
#include <QLinearGradient>
#include <QFontMetrics>
#include <QPixmap>
#include <QIcon>
#include <QImage>
#include <QSettings>
#include <QScreen>
#include <QGuiApplication>
#include <QStackedLayout>
#include <QLayoutItem>
#include <QIntValidator>
#include <QDoubleValidator>
#include <QCompleter>
#include <QPainter>
#include <QPaintDevice>
#include <QPolygon>
#include <QRect>
#include <QPointF>
#include <QDialogButtonBox>
#include <QStringListModel>
#include <QFileSystemModel>
#include <QListView>
#include <QTreeView>
#include <QStyledItemDelegate>
#include <QTextDocument>
#include <QTextCharFormat>
#include <QTextFormat>
#include <QTextOption>
#include <QSyntaxHighlighter>
#include <QActionGroup>
#include <QSpacerItem>
#include <QCalendarWidget>
#include <QEvent>
#include <QEventLoop>
#include <QMovie>
#include <QMimeData>
#include <QUrl>
#include <QDesktopServices>
#include <QDate>
#include <QToolTip>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStyleOptionViewItem>
#include <QMovie>
#include <QTextBlock>
#include <QThread>
#include <QRadialGradient>
#include <QOpenGLWidget>
#include <QAbstractItemModel>
#include <QModelIndex>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QMetaObject>
#include <QStyleHints>
#include <QMetaMethod>
#include <QString>
#include <QByteArray>

#include <map>
#include <mutex>

#include <ruby.h>
#include <ruby/thread.h>
#include "rubycallback.h"
#include "rubywidget.h"

static VALUE mQt;
static VALUE cQtBase, cQtObject, cApplication, cWidget, cFrame, cLabel;
static VALUE cAbstractButton, cPushButton, cCheckBox;
static VALUE cLineEdit, cComboBox, cPlainTextEdit, cGroupBox;
static VALUE cDialog, cMainWindow, cAbstractScrollArea, cAbstractItemView;
static VALUE cAbstractItemModel;   // needed by the model read-back accessors
static VALUE cLayout, cBoxLayout, cVBoxLayout, cHBoxLayout, cGridLayout, cAction;
static VALUE cIcon;   // declared early: ctor_action below takes an icon overload
static VALUE g_procs;   // GC anchor for connected Ruby blocks
void ruby_invoke_proc_once(VALUE proc);   // runs a one-shot block, then unanchors it

// ---------------------------------------------------------------------------
// Ownership model
//
// Qt owns objects through parent/child links: deleting a parent deletes its
// children, and addWidget()/setLayout() reparent. Ruby's GC owns the wrapper.
// Both can try to free the same QObject, so we track who is responsible:
//
//   owned == true   Ruby created it and it has no Qt parent -> Ruby deletes it
//   owned == false  Qt owns it (it has a parent, or it is the QApplication)
//
// We also keep an identity map so one QObject always maps to one Ruby object,
// and we listen to QObject::destroyed so that a wrapper whose C++ object died
// becomes "dangling" and raises a clean Ruby error instead of segfaulting.
// ---------------------------------------------------------------------------

struct QtWrap {
  QObject *ptr;
  bool owned;
};

// The QtWrap* is carried alongside the wrapper VALUE so that a destroyed()
// arriving during a GC sweep can mark the wrapper dangling without touching
// a Ruby object. qtwrap_free erases the map entry before it xfrees the
// struct, so a live entry always means a live struct.
struct ObjRef { VALUE v; QtWrap *w; };
static std::map<QObject *, ObjRef> g_objmap;   // weak: entries removed on free

// app.exec() releases the GVL, so Qt's event loop and a Ruby background thread
// run genuinely in parallel -- one mutating g_objmap from a destroyed() signal,
// the other from wrap_obj/GC. Every access below is under this lock.
// Recursive because qtwrap_free/obj_destroy hold it and then `delete o` fires
// on_destroyed on the same thread.
// LOCK ORDER: never hold this while calling into Ruby. A Qt thread that blocks
// on the GVL while holding it deadlocks any Ruby thread waiting on it.
static std::recursive_mutex g_objmap_mutex;
typedef std::lock_guard<std::recursive_mutex> ObjMapLock;

// True while we are deleting a QObject from inside Ruby's GC sweep. Deleting a
// parent cascades into its children, firing destroyed() for each one, and the
// handler must NOT dereference other Ruby wrappers at that point -- they may
// already have been swept.
static thread_local bool g_in_gc_free = false;

static void qtwrap_free(void *p) {
  QtWrap *w = (QtWrap *)p;
  if (w->ptr) {
    QObject *o = w->ptr;
    { ObjMapLock lk(g_objmap_mutex); g_objmap.erase(o); }
    w->ptr = NULL;
    if (w->owned) {
      bool prev = g_in_gc_free;
      g_in_gc_free = true;
      delete o;            // cascades to children; see on_destroyed()
      g_in_gc_free = prev;
    }
  }
  xfree(p);
}
static size_t qtwrap_size(const void *) { return sizeof(QtWrap); }

static const rb_data_type_t qtwrap_type = {
  "Qt::Object",
  { NULL, qtwrap_free, qtwrap_size, { NULL, NULL } },
  NULL, NULL, RUBY_TYPED_FREE_IMMEDIATELY
};

// Raises rather than returning a dangling pointer.
static QObject *get_obj(VALUE self) {
  QtWrap *w;
  TypedData_Get_Struct(self, QtWrap, &qtwrap_type, w);
  if (!w->ptr)
    rb_raise(rb_eRuntimeError, "Qt object has already been destroyed");
  return w->ptr;
}

// A failed qobject_cast previously dereferenced NULL and segfaulted (e.g.
// Qt::BoxLayout.new falling through to Qt::Object's constructor). Raise a
// Ruby TypeError instead so the mistake is diagnosable.
template <typename T> static T *qcast(VALUE v) {
  T *p = qobject_cast<T *>(get_obj(v));
  if (!p)
    rb_raise(rb_eTypeError, "expected a %s but got %s",
             T::staticMetaObject.className(), rb_obj_classname(v));
  return p;
}

static QtWrap *get_wrap(VALUE self) {
  QtWrap *w;
  TypedData_Get_Struct(self, QtWrap, &qtwrap_type, w);
  return w;
}

// Qt deleted the object out from under us -> mark the wrapper dangling.

struct DanglingArg { VALUE wrapper; };
static void *mark_dangling(void *p) {
  QtWrap *w;
  TypedData_Get_Struct(((DanglingArg *)p)->wrapper, QtWrap, &qtwrap_type, w);
  w->ptr = NULL;
  w->owned = false;
  return NULL;
}

static void on_destroyed(QObject *o) {
  VALUE v;
  {
  ObjMapLock lk(g_objmap_mutex);
  std::map<QObject *, ObjRef>::iterator it = g_objmap.find(o);
  if (it == g_objmap.end()) return;      // already reaped by qtwrap_free
  if (g_in_gc_free) {
    // THIS thread is inside a GC sweep, so the wrapper VALUE may itself have
    // been swept and must not be touched. The QtWrap behind it is still
    // allocated, and clearing that needs no Ruby API and no GVL.
    // Dropping only the map entry (as this used to) left a child wrapper that
    // Ruby still referenced pointing at freed memory while disposed? answered
    // false -- so every COSMOS `unless widget.disposed?` guard walked straight
    // into a use-after-free.
    if (it->second.w) { it->second.w->ptr = NULL; it->second.w->owned = false; }
    g_objmap.erase(it);
    return;
  }
  v = it->second.v;
  g_objmap.erase(it);
  }   // lock released before re-entering Ruby (see LOCK ORDER above)
  // Touching a Ruby object requires the GVL; this can run from Qt's event
  // loop while the GVL is released, and doing it unguarded corrupts Ruby's
  // heap (crashes surface later in unrelated Ruby threads).
  DanglingArg arg;
  arg.wrapper = v;
  ruby_run_with_gvl(mark_dangling, &arg);
}

// Maps a Qt class name to the Ruby class we bound for it, so a QObject found
// by introspection (findChildren) comes back as the most specific Ruby class
// we know about rather than a bare Qt::Widget.
static std::map<std::string, VALUE> g_class_by_qtname;

static void register_qt_class(const char *qtName, VALUE rbClass) {
  g_class_by_qtname[qtName] = rbClass;
}

static VALUE best_ruby_class(QObject *o, VALUE fallback) {
  for (const QMetaObject *mo = o->metaObject(); mo; mo = mo->superClass()) {
    std::map<std::string, VALUE>::iterator it = g_class_by_qtname.find(mo->className());
    if (it != g_class_by_qtname.end()) return it->second;
  }
  return fallback;
}

static VALUE wrap_obj(VALUE klass, QObject *p, bool owned = true) {
  if (!p) return Qnil;
  {
    ObjMapLock lk(g_objmap_mutex);
    std::map<QObject *, ObjRef>::iterator it = g_objmap.find(p);
    if (it != g_objmap.end()) return it->second.v;   // identity: one wrapper per object
  }
  QtWrap *w;
  VALUE o = TypedData_Make_Struct(klass, QtWrap, &qtwrap_type, w);
  w->ptr = p;
  w->owned = owned;
  {
    ObjMapLock lk(g_objmap_mutex);
    std::map<QObject *, ObjRef>::iterator it = g_objmap.find(p);
    if (it != g_objmap.end()) {
      // Lost the race. Disarm our loser before dropping it: left armed, its
      // qtwrap_free would erase the WINNER's map entry (so the surviving
      // wrapper could never be marked dangling) and, when owned, delete a
      // QObject the winner still points at.
      w->ptr = NULL;
      w->owned = false;
      return it->second.v;
    }
    g_objmap[p] = ObjRef{o, w};
  }
  QObject::connect(p, &QObject::destroyed, p, &on_destroyed);
  return o;
}

static void attach(VALUE self, QObject *p, bool owned);   // defined below
static VALUE qtobj_alloc(VALUE klass);

// Qt took ownership via reparenting -- Ruby must not delete it any more.
static void release_ownership(VALUE self) {
  QtWrap *w = get_wrap(self);
  if (w) w->owned = false;
}
// COSMOS telemetry is binary (ascii-8bit) and packet items routinely carry
// embedded NUL bytes. StringValueCStr REJECTS those outright
// ("string contains null byte"), which crashed PacketViewer on any binary
// item (packet_viewer.rb:536). Convert by pointer+length instead, which
// preserves the data and cannot raise. This is on the path of every string
// the binding converts, so it affected far more than one tool.
static QString rb_to_qs(VALUE v) {
  if (NIL_P(v)) return QString();
  VALUE s = v;
  StringValue(s);
  return QString::fromUtf8(RSTRING_PTR(s), (qsizetype)RSTRING_LEN(s));
}

// ---------- Qt::Application ----------
static int    s_argc = 1;
static char   s_arg0[] = "cosmos";
static char  *s_argv[] = { s_arg0, NULL };


static VALUE app_process_events(VALUE self) {
  QApplication::processEvents();
  return Qnil;
}
// Blocking Qt event loops MUST release the GVL. COSMOS runs real work on
// background Ruby threads while the main thread sits in exec():
// Splash.execute initialises CmdTlmServer on a Thread while the splash dialog
// is modal (lib/cosmos/gui/dialogs/splash.rb:104). Holding the GVL here
// starves that thread and the app hangs on the splash forever.
static void *exec_app_thunk(void *) { QApplication::exec(); return NULL; }

static VALUE app_exec(VALUE self) {
  rb_thread_call_without_gvl(exec_app_thunk, NULL, RUBY_UBF_IO, NULL);
  return INT2NUM(0);
}

// Runs the Qt event loop for `ms` with the GVL RELEASED, so Ruby's other
// threads keep being scheduled. Any Ruby callback fired from inside the loop
// re-acquires the GVL first (see RubyCallback::invoke).
static void *run_exec(void *) { QApplication::exec(); return NULL; }

static VALUE app_exec_for(VALUE self, VALUE ms) {
  QTimer::singleShot(NUM2INT(ms), qApp, &QCoreApplication::quit);
  rb_thread_call_without_gvl(run_exec, NULL, RUBY_UBF_IO, NULL);
  return Qnil;
}

// Fire a Ruby block from the Qt event loop after `ms`.
static VALUE qt_single_shot(int argc, VALUE *argv, VALUE self) {
  VALUE ms, blk;
  rb_scan_args(argc, argv, "10&", &ms, &blk);
  if (NIL_P(blk)) rb_raise(rb_eArgError, "single_shot requires a block");
  rb_ary_push(g_procs, blk);
  VALUE proc = blk;
  int delay = NUM2INT(ms);
  // QTimer must be created on the thread that will run it.
  QMetaObject::invokeMethod(qApp, [proc, delay]() {
    QTimer::singleShot(delay, qApp, [proc]() { ruby_invoke_proc_once(proc); });
  }, Qt::QueuedConnection);
  return Qtrue;
}

// ---------------------------------------------------------------------------
// Generic accessors.
//
// One C function per Qt method does not scale to the 286-method surface this
// port needs. These templates turn each binding into a single registration
// line, e.g.
//     QDEF(cLabel, "setText",
//                      RUBY_METHOD_FUNC((set_str<QLabel, &QLabel::setText>)), 1);
// ---------------------------------------------------------------------------

// Construction uses Ruby's alloc + initialize protocol, with `initialize`
// defined ONCE on Qt::Object and a registry mapping Ruby class -> C++ ctor.
//
// Why not define initialize per class: COSMOS reopens classes and defines its
// own initialize calling super (lib/cosmos/gui/qt.rb, Qt::BoxLayout). That
// reopen SHADOWS a per-class initialize, and super would then walk past it to
// Object#initialize, leaving the QObject never constructed. Registering on the
// base class means super always reaches a real constructor.
static void attach(VALUE self, QObject *p, bool owned) {
  QtWrap *w = get_wrap(self);
  w->ptr = p;
  w->owned = owned;
  { ObjMapLock lk(g_objmap_mutex); g_objmap[p] = ObjRef{self, w}; }
  QObject::connect(p, &QObject::destroyed, p, &on_destroyed);
}

static VALUE qtobj_alloc(VALUE klass) {
  QtWrap *w;
  VALUE o = TypedData_Make_Struct(klass, QtWrap, &qtwrap_type, w);
  w->ptr = NULL;
  w->owned = false;
  return o;
}

typedef QObject *(*ctor_fn)(int argc, VALUE *argv);
static std::map<VALUE, ctor_fn> g_ctors;      // Ruby class -> C++ constructor
static std::map<VALUE, bool>    g_ctor_owned; // does Ruby own what it builds?

// Qt constructors take a parent, and COSMOS relies on it:
//   layout = Qt::VBoxLayout.new(@widget)     (interfaces_tab.rb:105)
// installs the layout ON @widget. Dropping the parent silently orphans
// everything added to the layout -- the tab renders empty.
static bool g_ctor_took_parent = false;

static QWidget *parent_arg(int argc, VALUE *argv) {
  for (int i = 0; i < argc; i++) {
    if (NIL_P(argv[i]) || RB_TYPE_P(argv[i], T_STRING) || RB_TYPE_P(argv[i], T_FIXNUM))
      continue;
    if (rb_obj_is_kind_of(argv[i], cQtBase)) {
      QWidget *w = qobject_cast<QWidget *>(get_obj(argv[i]));
      if (w) return w;
    }
  }
  return NULL;
}
static VALUE string_arg(int argc, VALUE *argv) {
  for (int i = 0; i < argc; i++)
    if (RB_TYPE_P(argv[i], T_STRING)) return argv[i];
  return Qnil;
}

// Qt::X.new(parent) installs the object into Qt's parent chain, and
// g_ctor_took_parent then tells qt_initialize not to also make Ruby's GC an
// owner. Dropping the parent (as these used to) produced a parentless object
// that was marked Ruby-owned, so anything retained only through Qt's parent
// chain was deleted at the next GC and never rendered in its intended parent.
// Layouts have always come through ctor_layout below, which does the same
// thing -- Qt::VBoxLayout.new(@widget) must install the layout ON @widget or
// everything added to it is orphaned (interfaces_tab.rb:105).
template <typename T> static QObject *ctor_plain(int argc, VALUE *argv) {
  QWidget *p = parent_arg(argc, argv);
  if (p) { g_ctor_took_parent = true; return new T(p); }
  return new T();
}

template <typename T> static QObject *ctor_str(int argc, VALUE *argv) {
  VALUE t = string_arg(argc, argv);
  QWidget *p = parent_arg(argc, argv);
  if (p) {
    g_ctor_took_parent = true;
    return NIL_P(t) ? new T(p) : new T(rb_to_qs(t), p);
  }
  return NIL_P(t) ? new T() : new T(rb_to_qs(t));
}

template <typename T> static QObject *ctor_layout(int argc, VALUE *argv) {
  QWidget *p = parent_arg(argc, argv);
  if (p) { g_ctor_took_parent = true; return new T(p); }
  return new T();
}
// Orientation-aware constructors. The first Fixnum argument is the
// orientation; a widget argument is the parent.
template <typename T>
static QObject *ctor_oriented(int argc, VALUE *argv, Qt::Orientation dflt) {
  Qt::Orientation o = dflt;
  for (int i = 0; i < argc; i++) {
    if (RB_TYPE_P(argv[i], T_FIXNUM)) { o = (Qt::Orientation)NUM2INT(argv[i]); break; }
  }
  QWidget *p = parent_arg(argc, argv);
  if (p) { g_ctor_took_parent = true; return new T(o, p); }
  return new T(o);
}
static VALUE splitter_orientation(VALUE self) {
  return INT2NUM((int)qcast<QSplitter>(self)->orientation());
}
static VALUE slider_orientation(VALUE self) {
  return INT2NUM((int)qcast<QAbstractSlider>(self)->orientation());
}
static VALUE slider_set_orientation(VALUE self, VALUE o) {
  qcast<QAbstractSlider>(self)->setOrientation((Qt::Orientation)NUM2INT(o));
  return self;
}

static QObject *ctor_splitter(int argc, VALUE *argv) {
  return ctor_oriented<QSplitter>(argc, argv, Qt::Horizontal);   // QSplitter's default
}
static QObject *ctor_slider(int argc, VALUE *argv) {
  return ctor_oriented<QSlider>(argc, argv, Qt::Vertical);       // QSlider's default
}

// COSMOS sets background colours in stylesheets without setting a text
// colour -- 15 sites, e.g. legal_dialog.rb:53
// "QLabel { background-color : white; }". Qt 4 had no macOS dark-mode
// support, so that was always safe; Qt 6 follows the system appearance and
// paints the palette's white text onto those white backgrounds, which is why
// the Legal Agreement pane looks empty. Default to the light scheme COSMOS
// was written against. COSMOS_QT_COLOR_SCHEME=dark|system opts out.
static void apply_color_scheme() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
  const char *want = getenv("COSMOS_QT_COLOR_SCHEME");
  if (want && !strcasecmp(want, "system")) return;
  Qt::ColorScheme scheme = Qt::ColorScheme::Light;
  if (want && !strcasecmp(want, "dark")) scheme = Qt::ColorScheme::Dark;
  if (QStyleHints *h = QGuiApplication::styleHints()) h->setColorScheme(scheme);
#endif
}

static QObject *ctor_application(int, VALUE *) {
  QApplication *app = new QApplication(s_argc, s_argv);
  apply_color_scheme();
  return app;
}
// QActionGroup has no default constructor; it requires a parent QObject*.
static QObject *ctor_action_group(int, VALUE *) { return new QActionGroup(NULL); }

// COSMOS builds one directly: Qt::BoxLayout.new(Qt::Horizontal). Qt's
// orientation enum is not QBoxLayout::Direction, so map it.
static QObject *ctor_box_layout(int argc, VALUE *argv) {
  QBoxLayout::Direction dir = QBoxLayout::TopToBottom;
  if (argc > 0 && RB_TYPE_P(argv[0], T_FIXNUM)) {
    int v = NUM2INT(argv[0]);
    if (v == (int)Qt::Horizontal)     dir = QBoxLayout::LeftToRight;
    else if (v == (int)Qt::Vertical)  dir = QBoxLayout::TopToBottom;
    else dir = (QBoxLayout::Direction)v;
  }
  QWidget *p = parent_arg(argc, argv);
  if (p) { g_ctor_took_parent = true; return new QBoxLayout(dir, p); }
  return new QBoxLayout(dir);
}

template <typename T> static T *get_val(VALUE self);   // defined below

// Qt::Action.new(text, parent) -- the parent matters: passing a QActionGroup
// is how COSMOS builds exclusive action groups (config_editor.rb), and Qt only
// adds the action to the group via that parent link.
// COSMOS uses all four qtbindings overloads, so scan every argument by type
// rather than by position: Action.new(text), Action.new(text, parent),
// Action.new(icon, text, parent) and Action.new(parent).
static QObject *ctor_action(int argc, VALUE *argv) {
  QString text;
  QIcon icon;
  bool have_text = false, have_icon = false;
  QObject *parent = NULL;
  for (int i = 0; i < argc; i++) {
    if (NIL_P(argv[i])) continue;
    if (!have_text && RB_TYPE_P(argv[i], T_STRING)) {
      text = rb_to_qs(argv[i]); have_text = true;
    } else if (!have_icon && rb_obj_is_kind_of(argv[i], cIcon)) {
      icon = *get_val<QIcon>(argv[i]); have_icon = true;
    } else if (!parent && rb_obj_is_kind_of(argv[i], cQtBase)) {
      parent = get_obj(argv[i]);
    }
  }
  QAction *a = have_icon ? new QAction(icon, text, parent) : new QAction(text, parent);
  // A parented QAction belongs to Qt. Without this the GC also owns it and
  // the second delete is a use-after-free at interpreter shutdown.
  if (parent) g_ctor_took_parent = true;
  if (QActionGroup *g = qobject_cast<QActionGroup *>(parent)) g->addAction(a);
  return a;
}

static void register_ctor(VALUE klass, ctor_fn fn, bool owned = true) {
  rb_define_alloc_func(klass, qtobj_alloc);
  g_ctors[klass] = fn;
  g_ctor_owned[klass] = owned;
}

// Defined on Qt::Object only; walks the ancestor chain for a registered ctor.
// qtbindings let a constructor take a configuration block, and COSMOS uses
// both of its forms (8 sites). Ignoring the block meant top_level.rb:335
// produced an untitled, empty, button-less modal that then blocked the GUI
// thread forever in exec.
//   Qt::Dialog.new { |box| box.setWindowTitle(..) }   -> yield the object
//   Qt::VBoxLayout.new { addWidget(ok) }              -> instance_eval
// The no-argument form relies on instance_eval so bare method calls resolve
// against the object being constructed.
static void run_ctor_block(VALUE self) {
  if (!rb_block_given_p()) return;
  VALUE blk = rb_block_proc();
  if (rb_proc_arity(blk) == 0) {
    rb_funcall_with_block(self, rb_intern("instance_eval"), 0, NULL, blk);
  } else {
    rb_yield(self);
  }
}

static VALUE qt_initialize(int argc, VALUE *argv, VALUE self) {
  if (get_wrap(self)->ptr) return self;          // already constructed
  VALUE k = rb_obj_class(self);
  while (!NIL_P(k)) {
    std::map<VALUE, ctor_fn>::iterator it = g_ctors.find(k);
    if (it != g_ctors.end()) {
      g_ctor_took_parent = false;
      QObject *obj = it->second(argc, argv);
      // A parented object belongs to Qt, not to Ruby's GC.
      attach(self, obj, g_ctor_owned[k] && !g_ctor_took_parent);
      run_ctor_block(self);   // after attach: the block uses the live object
      return self;
    }
    k = rb_funcall(k, rb_intern("superclass"), 0);
  }
  rb_raise(rb_eRuntimeError, "no Qt constructor registered for %s",
           rb_obj_classname(self));
}

template <typename T, void (T::*M)(const QString &)>
static VALUE set_str(VALUE self, VALUE v) {
  (qcast<T>(self)->*M)(rb_to_qs(v));
  return self;
}
template <typename T, QString (T::*M)() const>
static VALUE get_str(VALUE self) {
  return rb_str_new2((qcast<T>(self)->*M)().toUtf8().constData());
}
template <typename T, void (T::*M)(bool)>
static VALUE set_bool(VALUE self, VALUE v) {
  (qcast<T>(self)->*M)(RTEST(v));
  return self;
}
template <typename T, bool (T::*M)() const>
static VALUE get_bool(VALUE self) {
  return (qcast<T>(self)->*M)() ? Qtrue : Qfalse;
}
template <typename T, void (T::*M)(int)>
static VALUE set_int(VALUE self, VALUE v) {
  (qcast<T>(self)->*M)(NUM2INT(v));
  return self;
}
template <typename T, int (T::*M)() const>
static VALUE get_int(VALUE self) {
  return INT2NUM((qcast<T>(self)->*M)());
}
template <typename T, void (T::*M)()>
static VALUE call_void(VALUE self) {
  (qcast<T>(self)->*M)();
  return self;
}

// ---------- hand-written where the signature needs ownership handling ------
static VALUE widget_resize(VALUE self, VALUE w, VALUE h) {
  qcast<QWidget>(self)->resize(NUM2INT(w), NUM2INT(h));
  return self;
}
static VALUE widget_set_layout(VALUE self, VALUE l) {
  qcast<QWidget>(self)
    ->setLayout(qobject_cast<QLayout *>(get_obj(l)));
  release_ownership(l);   // Qt reparented it
  return self;
}
// QLayout::addWidget takes only the widget; QBoxLayout adds (stretch, align).
static VALUE layout_add_widget(int argc, VALUE *argv, VALUE self) {
  VALUE w, stretch, align;
  rb_scan_args(argc, argv, "12", &w, &stretch, &align);
  QWidget *widget = qobject_cast<QWidget *>(get_obj(w));
  QLayout *lay = qcast<QLayout>(self);
  QBoxLayout *box = qobject_cast<QBoxLayout *>(lay);
  if (box && (!NIL_P(stretch) || !NIL_P(align)))
    box->addWidget(widget, NIL_P(stretch) ? 0 : NUM2INT(stretch),
                   NIL_P(align) ? Qt::Alignment() : (Qt::Alignment)NUM2INT(align));
  else
    lay->addWidget(widget);
  release_ownership(w);   // Qt reparented it
  return self;
}
static VALUE layout_add_layout(int argc, VALUE *argv, VALUE self) {
  VALUE l, stretch;
  rb_scan_args(argc, argv, "11", &l, &stretch);
  QBoxLayout *b = qcast<QBoxLayout>(self);
  if (!b) rb_raise(rb_eRuntimeError, "addLayout requires a box layout");
  b->addLayout(qobject_cast<QLayout *>(get_obj(l)), NIL_P(stretch) ? 0 : NUM2INT(stretch));
  release_ownership(l);
  return self;
}
static VALUE layout_count(VALUE self) {
  return INT2NUM(qcast<QLayout>(self)->count());
}
// addItem(text), addItem(text, userData) and addItem(icon, text) are all used.
static VALUE combo_add_item(int argc, VALUE *argv, VALUE self) {
  VALUE a, b; rb_scan_args(argc, argv, "11", &a, &b);
  QComboBox *c = qcast<QComboBox>(self);
  if (NIL_P(b)) { c->addItem(rb_to_qs(a)); return self; }
  if (RB_TYPE_P(a, T_STRING)) {
    // The user-data arg may be a Qt::Variant; detect it by duck-typing so this
    // does not depend on class VALUEs declared later in Init.
    QVariant data;
    if (RB_TYPE_P(b, T_FIXNUM))        data = QVariant(NUM2INT(b));
    else if (RB_TYPE_P(b, T_STRING))   data = QVariant(rb_to_qs(b));
    else if (rb_respond_to(b, rb_intern("toString")))
      data = QVariant(rb_to_qs(rb_funcall(b, rb_intern("toString"), 0)));
    else                               data = QVariant();
    c->addItem(rb_to_qs(a), data);
  } else {
    c->addItem(rb_to_qs(b));   // addItem(icon, text): keep the text
  }
  return self;
}
static VALUE combo_item_text(VALUE self, VALUE i) {
  return rb_str_new2(qcast<QComboBox>(self)
                       ->itemText(NUM2INT(i)).toUtf8().constData());
}
static VALUE mainwindow_set_central(VALUE self, VALUE w) {
  qcast<QMainWindow>(self)
    ->setCentralWidget(qobject_cast<QWidget *>(get_obj(w)));
  release_ownership(w);
  return self;
}
// QPushButton::click() is not virtual-dispatchable through QAbstractButton in
// a template, so keep it explicit.
static VALUE button_click(VALUE self) {
  qcast<QAbstractButton>(self)->click();
  return self;
}

// ---------------------------------------------------------------------------
// Value types.
//
// Much of Qt is NOT QObject-derived: QKeySequence, QVariant, QFont, QColor,
// QSize, QPoint, QSizePolicy ... These have value semantics, so they get their
// own wrapper that simply owns a heap copy and deletes it on GC. No parent/
// child ownership questions apply.
// ---------------------------------------------------------------------------

template <typename T> static void val_free(void *p) { delete static_cast<T *>(p); }
template <typename T> static size_t val_size(const void *) { return sizeof(T); }

template <typename T> static const rb_data_type_t &val_type() {
  // One static per instantiation, so TypedData type-checking stays sound.
  static const rb_data_type_t t = {
    "Qt::Value",
    { NULL, val_free<T>, val_size<T>, { NULL, NULL } },
    NULL, NULL, RUBY_TYPED_FREE_IMMEDIATELY
  };
  return t;
}
template <typename T> static VALUE wrap_val(VALUE klass, const T &v) {
  return TypedData_Wrap_Struct(klass, &val_type<T>(), new T(v));
}
template <typename T> static T *get_val(VALUE self) {
  T *p;
  TypedData_Get_Struct(self, T, &val_type<T>(), p);
  return p;
}

static VALUE cKeySequence, cVariant, cFont, cColor, cSize, cPoint, cSizePolicy;
static VALUE cMessageBox, cFileDialog;

// ---- Qt::KeySequence (110 uses in COSMOS: Qt::KeySequence.new('Ctrl+Q')) ---
static VALUE keyseq_new(int argc, VALUE *argv, VALUE klass) {
  VALUE v; rb_scan_args(argc, argv, "01", &v);
  if (NIL_P(v)) return wrap_val<QKeySequence>(klass, QKeySequence());
  if (RB_TYPE_P(v, T_FIXNUM))
    return wrap_val<QKeySequence>(klass, QKeySequence(NUM2INT(v)));
  return wrap_val<QKeySequence>(klass, QKeySequence(rb_to_qs(v)));
}
static VALUE keyseq_to_s(VALUE self) {
  return rb_str_new2(get_val<QKeySequence>(self)->toString().toUtf8().constData());
}
static VALUE keyseq_is_empty(VALUE self) {
  return get_val<QKeySequence>(self)->isEmpty() ? Qtrue : Qfalse;
}
static VALUE action_set_shortcut(VALUE self, VALUE ks) {
  qcast<QAction>(self)->setShortcut(*get_val<QKeySequence>(ks));
  return self;
}
static VALUE action_shortcut(VALUE self) {
  return wrap_val<QKeySequence>(cKeySequence,
                                qcast<QAction>(self)->shortcut());
}

// ---- Qt::Variant -----------------------------------------------------------
static VALUE variant_new(int argc, VALUE *argv, VALUE klass) {
  VALUE v; rb_scan_args(argc, argv, "01", &v);
  if (NIL_P(v))                   return wrap_val<QVariant>(klass, QVariant());
  if (RB_TYPE_P(v, T_FIXNUM))     return wrap_val<QVariant>(klass, QVariant(NUM2INT(v)));
  if (RB_TYPE_P(v, T_FLOAT))      return wrap_val<QVariant>(klass, QVariant(NUM2DBL(v)));
  if (v == Qtrue || v == Qfalse)  return wrap_val<QVariant>(klass, QVariant(RTEST(v)));
  if (RB_TYPE_P(v, T_ARRAY)) {
    QStringList l;
    for (long i = 0; i < RARRAY_LEN(v); i++) l << rb_to_qs(rb_ary_entry(v, i));
    return wrap_val<QVariant>(klass, QVariant(l));
  }
  // qt_tool.rb:255 stores window geometry as Variant.new(pos()) /
  // Variant.new(size()); without these the Point fell through to rb_to_qs and
  // raised "no implicit conversion of Qt::Point into String" on every close.
  if (rb_obj_is_kind_of(v, cPoint)) return wrap_val<QVariant>(klass, QVariant(*get_val<QPoint>(v)));
  if (rb_obj_is_kind_of(v, cSize))  return wrap_val<QVariant>(klass, QVariant(*get_val<QSize>(v)));
  if (rb_obj_is_kind_of(v, cColor)) return wrap_val<QVariant>(klass, QVariant(*get_val<QColor>(v)));
  if (rb_obj_is_kind_of(v, cFont))  return wrap_val<QVariant>(klass, QVariant(*get_val<QFont>(v)));
  return wrap_val<QVariant>(klass, QVariant(rb_to_qs(v)));
}
static VALUE variant_to_s(VALUE self)  { return rb_str_new2(get_val<QVariant>(self)->toString().toUtf8().constData()); }
static VALUE variant_to_i(VALUE self)  { return INT2NUM(get_val<QVariant>(self)->toInt()); }
static VALUE variant_to_f(VALUE self)  { return DBL2NUM(get_val<QVariant>(self)->toDouble()); }
static VALUE variant_to_b(VALUE self)  { return get_val<QVariant>(self)->toBool() ? Qtrue : Qfalse; }
static VALUE variant_valid(VALUE self) { return get_val<QVariant>(self)->isValid() ? Qtrue : Qfalse; }

// ---- Qt::Font / Qt::Color / Qt::Size / Qt::Point ---------------------------
// QFont(family, pointSize, weight, italic) -- COSMOS uses the 3- and 4-arg
// forms from Cosmos.getFont (lib/cosmos/gui/qt.rb:180).
static VALUE font_new(int argc, VALUE *argv, VALUE klass) {
  VALUE fam, pt, weight, italic;
  rb_scan_args(argc, argv, "04", &fam, &pt, &weight, &italic);
  if (NIL_P(fam)) return wrap_val<QFont>(klass, QFont());
  if (NIL_P(pt))  return wrap_val<QFont>(klass, QFont(rb_to_qs(fam)));
  if (NIL_P(weight))
    return wrap_val<QFont>(klass, QFont(rb_to_qs(fam), NUM2INT(pt)));
  return wrap_val<QFont>(klass, QFont(rb_to_qs(fam), NUM2INT(pt),
                                      NUM2INT(weight), RTEST(italic)));
}
static VALUE font_family(VALUE self)    { return rb_str_new2(get_val<QFont>(self)->family().toUtf8().constData()); }
static VALUE font_point_size(VALUE self){ return INT2NUM(get_val<QFont>(self)->pointSize()); }
static VALUE font_set_bold(VALUE self, VALUE b) { get_val<QFont>(self)->setBold(RTEST(b)); return self; }
static VALUE font_bold(VALUE self)      { return get_val<QFont>(self)->bold() ? Qtrue : Qfalse; }
static VALUE widget_set_font(VALUE self, VALUE f) {
  qcast<QWidget>(self)->setFont(*get_val<QFont>(f));
  return self;
}

static VALUE color_new(int argc, VALUE *argv, VALUE klass) {
  VALUE r, g, b;
  rb_scan_args(argc, argv, "12", &r, &g, &b);
  if (NIL_P(g)) {   // single arg: Qt::GlobalColor int, or a name string
    if (RB_TYPE_P(r, T_FIXNUM))
      return wrap_val<QColor>(klass, QColor((Qt::GlobalColor)NUM2INT(r)));
    return wrap_val<QColor>(klass, QColor(rb_to_qs(r)));
  }
  return wrap_val<QColor>(klass, QColor(NUM2INT(r), NUM2INT(g), NUM2INT(b)));
}
static VALUE color_red(VALUE self)   { return INT2NUM(get_val<QColor>(self)->red()); }
static VALUE color_green(VALUE self) { return INT2NUM(get_val<QColor>(self)->green()); }
static VALUE color_blue(VALUE self)  { return INT2NUM(get_val<QColor>(self)->blue()); }
static VALUE color_name(VALUE self)  { return rb_str_new2(get_val<QColor>(self)->name().toUtf8().constData()); }

static VALUE size_new(VALUE klass, VALUE w, VALUE h) { return wrap_val<QSize>(klass, QSize(NUM2INT(w), NUM2INT(h))); }
static VALUE size_width(VALUE self)  { return INT2NUM(get_val<QSize>(self)->width()); }
static VALUE size_height(VALUE self) { return INT2NUM(get_val<QSize>(self)->height()); }
static VALUE widget_size(VALUE self) { return wrap_val<QSize>(cSize, qcast<QWidget>(self)->size()); }

static VALUE point_new(VALUE klass, VALUE x, VALUE y) { return wrap_val<QPoint>(klass, QPoint(NUM2INT(x), NUM2INT(y))); }
static VALUE point_x(VALUE self) { return INT2NUM(get_val<QPoint>(self)->x()); }
static VALUE point_y(VALUE self) { return INT2NUM(get_val<QPoint>(self)->y()); }
static VALUE widget_pos(VALUE self) { return wrap_val<QPoint>(cPoint, qcast<QWidget>(self)->pos()); }

// ---- size policy (COSMOS: setSizePolicy(Qt::SizePolicy::Fixed, ...)) -------
static VALUE widget_set_size_policy(VALUE self, VALUE h, VALUE v) {
  qcast<QWidget>(self)
    ->setSizePolicy((QSizePolicy::Policy)NUM2INT(h), (QSizePolicy::Policy)NUM2INT(v));
  return self;
}

// ---- Qt::MessageBox (232 refs; static helpers) -----------------------------
static QWidget *opt_parent(VALUE v) {
  return NIL_P(v) ? NULL : qobject_cast<QWidget *>(get_obj(v));
}
template <QMessageBox::Icon ICON>
// COSMOS calls these with five arguments:
//   MessageBox.critical(parent, title, text, buttons, defaultButton)
// (script_module_gui.rb:28 and 8 other sites), so accept two optional trailing
// arguments, not one.
static VALUE msgbox_static(int argc, VALUE *argv, VALUE klass) {
  VALUE parent, title, text, buttons, defaultButton;
  rb_scan_args(argc, argv, "32", &parent, &title, &text, &buttons, &defaultButton);
  QMessageBox::StandardButtons b = NIL_P(buttons)
      ? QMessageBox::StandardButtons(QMessageBox::Ok)
      : QMessageBox::StandardButtons((int)NUM2INT(buttons));
  QMessageBox box(ICON, rb_to_qs(title), rb_to_qs(text), b, opt_parent(parent));
  if (!NIL_P(defaultButton) && RB_TYPE_P(defaultButton, T_FIXNUM)) {
    int d = NUM2INT(defaultButton);
    if (d != (int)QMessageBox::NoButton)
      box.setDefaultButton((QMessageBox::StandardButton)d);
  }
  int rc = 0;
  ruby_without_gvl([&] { rc = box.exec(); });
  return INT2NUM(rc);
}

// ---- Qt::FileDialog --------------------------------------------------------
static VALUE filedlg_open(int argc, VALUE *argv, VALUE klass) {
  VALUE parent, caption, dir, filter;
  rb_scan_args(argc, argv, "04", &parent, &caption, &dir, &filter);
  QWidget *pw = opt_parent(parent);
  const QString cap = rb_to_qs(caption), d = rb_to_qs(dir), fl = rb_to_qs(filter);
  QString f;
  ruby_without_gvl([&] { f = QFileDialog::getOpenFileName(pw, cap, d, fl); });
  return f.isEmpty() ? Qnil : rb_str_new2(f.toUtf8().constData());
}
static VALUE filedlg_save(int argc, VALUE *argv, VALUE klass) {
  VALUE parent, caption, dir, filter;
  rb_scan_args(argc, argv, "04", &parent, &caption, &dir, &filter);
  QWidget *pw = opt_parent(parent);
  const QString cap = rb_to_qs(caption), d = rb_to_qs(dir), fl = rb_to_qs(filter);
  QString f;
  ruby_without_gvl([&] { f = QFileDialog::getSaveFileName(pw, cap, d, fl); });
  return f.isEmpty() ? Qnil : rb_str_new2(f.toUtf8().constData());
}
static VALUE filedlg_open_many(int argc, VALUE *argv, VALUE klass) {
  VALUE parent, caption, dir, filter;
  rb_scan_args(argc, argv, "04", &parent, &caption, &dir, &filter);
  QWidget *pw = opt_parent(parent);
  const QString cap = rb_to_qs(caption), d = rb_to_qs(dir), fl = rb_to_qs(filter);
  QStringList fs;
  ruby_without_gvl([&] { fs = QFileDialog::getOpenFileNames(pw, cap, d, fl); });
  VALUE ary = rb_ary_new();
  for (int i = 0; i < fs.size(); i++)
    rb_ary_push(ary, rb_str_new2(fs.at(i).toUtf8().constData()));
  return ary;
}
static VALUE filedlg_dir(int argc, VALUE *argv, VALUE klass) {
  VALUE parent, caption, dir;
  rb_scan_args(argc, argv, "03", &parent, &caption, &dir);
  QWidget *pw = opt_parent(parent);
  const QString cap = rb_to_qs(caption), d = rb_to_qs(dir);
  QString f;
  ruby_without_gvl([&] { f = QFileDialog::getExistingDirectory(pw, cap, d); });
  return f.isEmpty() ? Qnil : rb_str_new2(f.toUtf8().constData());
}

// ---- Qt::CoreApplication ---------------------------------------------------
static VALUE core_app_name(VALUE self) {
  return rb_str_new2(QCoreApplication::applicationName().toUtf8().constData());
}
static VALUE core_set_app_name(VALUE self, VALUE v) {
  QCoreApplication::setApplicationName(rb_to_qs(v));
  return self;
}

// ---------------------------------------------------------------------------
// Item types (QTableWidgetItem, QTreeWidgetItem, QListWidgetItem).
//
// A third category: not QObject, not value-semantic. The owning view takes
// ownership once the item is inserted, so Ruby never frees these.
// ---------------------------------------------------------------------------
template <typename T> static const rb_data_type_t &ptr_type() {
  static const rb_data_type_t t = {
    "Qt::Item", { NULL, NULL, NULL, { NULL, NULL } }, NULL, NULL, 0
  };
  return t;   // dfree == NULL: the owning widget frees it, not Ruby
}
template <typename T> static VALUE wrap_ptr(VALUE klass, T *p) {
  return p ? TypedData_Wrap_Struct(klass, &ptr_type<T>(), p) : Qnil;
}
template <typename T> static T *get_ptr(VALUE self) {
  T *p; TypedData_Get_Struct(self, T, &ptr_type<T>(), p); return p;
}

static VALUE cTableWidget, cTableWidgetItem, cTreeWidget, cTreeWidgetItem;
static VALUE cListWidget, cListWidgetItem, cTabWidget, cSplitter, cScrollArea;
static VALUE cFormLayout, cMenu, cMenuBar, cToolBar, cStatusBar, cProgressBar;
static VALUE cRadioButton, cSlider, cSpinBox, cDoubleSpinBox, cTextEdit, cTimer;

// ---- table -----------------------------------------------------------------
// COSMOS reopens Qt::TableWidgetItem#initialize to call
// setFlags(Qt::ItemIsEnabled) -- "the default COSMOS setting which makes
// table cells read only" (qt.rb:317, table_manager.rb:26). A singleton `new`
// that wrapped the pointer directly never ran initialize, so every cell kept
// QTableWidgetItem's editable/checkable defaults instead. Construct, then
// call initialize. The C implementation lives in an included module so the
// reopened version's `super(string)` still reaches it.
static VALUE twitem_new(int argc, VALUE *argv, VALUE klass) {
  VALUE obj = wrap_ptr<QTableWidgetItem>(klass, new QTableWidgetItem());
  rb_obj_call_init(obj, argc, argv);
  return obj;
}
static VALUE twitem_initialize(int argc, VALUE *argv, VALUE self) {
  VALUE t; rb_scan_args(argc, argv, "01", &t);
  if (!NIL_P(t)) get_ptr<QTableWidgetItem>(self)->setText(rb_to_qs(t));
  return self;
}
static VALUE twitem_text(VALUE self) {
  return rb_str_new2(get_ptr<QTableWidgetItem>(self)->text().toUtf8().constData());
}
static VALUE twitem_set_text(VALUE self, VALUE t) {
  get_ptr<QTableWidgetItem>(self)->setText(rb_to_qs(t)); return self;
}
static VALUE tw_set_item(VALUE self, VALUE r, VALUE c, VALUE item) {
  qcast<QTableWidget>(self)
    ->setItem(NUM2INT(r), NUM2INT(c), get_ptr<QTableWidgetItem>(item));
  return self;
}
static VALUE tw_item(VALUE self, VALUE r, VALUE c) {
  return wrap_ptr<QTableWidgetItem>(cTableWidgetItem,
    qcast<QTableWidget>(self)->item(NUM2INT(r), NUM2INT(c)));
}
static VALUE tw_set_hheader(VALUE self, VALUE c, VALUE item) {
  qcast<QTableWidget>(self)
    ->setHorizontalHeaderItem(NUM2INT(c), get_ptr<QTableWidgetItem>(item));
  return self;
}

// ---- tree ------------------------------------------------------------------
static VALUE tritem_new(int argc, VALUE *argv, VALUE klass) {
  VALUE obj = wrap_ptr<QTreeWidgetItem>(klass, new QTreeWidgetItem());
  rb_obj_call_init(obj, argc, argv);
  return obj;
}
static VALUE tritem_initialize(int argc, VALUE *argv, VALUE self) {
  VALUE t; rb_scan_args(argc, argv, "01", &t);
  if (NIL_P(t)) return self;
  QTreeWidgetItem *i = get_ptr<QTreeWidgetItem>(self);
  if (RB_TYPE_P(t, T_ARRAY)) {
    // qtbindings' QStringList overload. Test Runner builds every suite node
    // as Qt::TreeWidgetItem.new([name]) (test_runner.rb:723), which used to
    // raise TypeError and leave the tree empty.
    for (long c = 0; c < RARRAY_LEN(t); c++)
      i->setText((int)c, rb_to_qs(RARRAY_AREF(t, c)));
  } else {
    i->setText(0, rb_to_qs(t));
  }
  return self;
}
// qt.rb:676 builds Qt::ListWidgetItem.new(icon, text); there was no ctor.
static VALUE lwitem_new(int argc, VALUE *argv, VALUE klass) {
  VALUE obj = wrap_ptr<QListWidgetItem>(klass, new QListWidgetItem());
  rb_obj_call_init(obj, argc, argv);
  return obj;
}
static VALUE lwitem_initialize(int argc, VALUE *argv, VALUE self) {
  QListWidgetItem *it = get_ptr<QListWidgetItem>(self);
  for (int i = 0; i < argc; i++) {
    if (NIL_P(argv[i])) continue;
    if (RB_TYPE_P(argv[i], T_STRING))           it->setText(rb_to_qs(argv[i]));
    else if (rb_obj_is_kind_of(argv[i], cIcon)) it->setIcon(*get_val<QIcon>(argv[i]));
  }
  return self;
}
static VALUE tritem_text(VALUE self, VALUE col) {
  return rb_str_new2(get_ptr<QTreeWidgetItem>(self)->text(NUM2INT(col)).toUtf8().constData());
}
static VALUE tritem_set_text(VALUE self, VALUE col, VALUE t) {
  get_ptr<QTreeWidgetItem>(self)->setText(NUM2INT(col), rb_to_qs(t)); return self;
}
static VALUE tritem_add_child(VALUE self, VALUE kid) {
  get_ptr<QTreeWidgetItem>(self)->addChild(get_ptr<QTreeWidgetItem>(kid)); return self;
}
static VALUE tr_add_top(VALUE self, VALUE item) {
  qcast<QTreeWidget>(self)
    ->addTopLevelItem(get_ptr<QTreeWidgetItem>(item));
  return self;
}
static VALUE tr_top_count(VALUE self) {
  return INT2NUM(qcast<QTreeWidget>(self)->topLevelItemCount());
}

// ---- list ------------------------------------------------------------------
static VALUE lw_add_item(VALUE self, VALUE t) {
  qcast<QListWidget>(self)->addItem(rb_to_qs(t)); return self;
}
static VALUE lw_item_text(VALUE self, VALUE i) {
  QListWidgetItem *it = qcast<QListWidget>(self)->item(NUM2INT(i));
  return it ? rb_str_new2(it->text().toUtf8().constData()) : Qnil;
}

// ---- tabs / containers -----------------------------------------------------
static VALUE tab_add_tab(VALUE self, VALUE w, VALUE label) {
  int i = qcast<QTabWidget>(self)
            ->addTab(qobject_cast<QWidget *>(get_obj(w)), rb_to_qs(label));
  release_ownership(w);
  return INT2NUM(i);
}
static VALUE tab_text(VALUE self, VALUE i) {
  return rb_str_new2(qcast<QTabWidget>(self)
                       ->tabText(NUM2INT(i)).toUtf8().constData());
}
static VALUE splitter_add(VALUE self, VALUE w) {
  qcast<QSplitter>(self)->addWidget(qobject_cast<QWidget *>(get_obj(w)));
  release_ownership(w);
  return self;
}
static VALUE scroll_set_widget(VALUE self, VALUE w) {
  QScrollArea *a = qcast<QScrollArea>(self);
  if (NIL_P(w)) { a->setWidget(NULL); return self; }   // COSMOS clears it with nil
  a->setWidget(qobject_cast<QWidget *>(get_obj(w)));
  release_ownership(w);
  return self;
}
// QFormLayout::addRow has both a widget and a layout overload. COSMOS uses
// the layout one at logging_tab.rb:174 to drop a row of buttons into a form.
// Casting a QLayout to QWidget* yields NULL, and Qt then adds an EMPTY field
// -- no error, the four buttons inside it are simply orphaned and never
// appear. Found by diffing against the Qt4 reference: 10 buttons there, 6 here.
static VALUE form_add_row(VALUE self, VALUE label, VALUE field) {
  QFormLayout *f = qcast<QFormLayout>(self);
  // addRow also takes a WIDGET as the label, not just a string
  // (find_replace_dialog.rb:111 passes a Qt::Label).
  if (!RB_TYPE_P(label, T_STRING)) {
    QWidget *lw = qobject_cast<QWidget *>(get_obj(label));
    if (lw) {
      QObject *fo = get_obj(field);
      if (QWidget *fw = qobject_cast<QWidget *>(fo))      f->addRow(lw, fw);
      else if (QLayout *fl = qobject_cast<QLayout *>(fo)) f->addRow(lw, fl);
      else rb_raise(rb_eArgError, "addRow expects a widget or a layout");
      release_ownership(label);
      release_ownership(field);
      return self;
    }
  }
  QObject *o = get_obj(field);
  if (QWidget *w = qobject_cast<QWidget *>(o)) {
    f->addRow(rb_to_qs(label), w);
  } else if (QLayout *l = qobject_cast<QLayout *>(o)) {
    f->addRow(rb_to_qs(label), l);
  } else {
    rb_raise(rb_eArgError, "addRow expects a widget or a layout");
  }
  release_ownership(field);
  return self;
}

// ---- menus / bars ----------------------------------------------------------
static VALUE menu_add_action(VALUE self, VALUE a) {
  QObject *o = get_obj(self);
  QAction *act = qobject_cast<QAction *>(get_obj(a));
  if (QMenu *m = qobject_cast<QMenu *>(o))        m->addAction(act);
  else if (QToolBar *t = qobject_cast<QToolBar *>(o)) t->addAction(act);
  else rb_raise(rb_eRuntimeError, "addAction: unsupported receiver");
  release_ownership(a);
  return self;
}
static VALUE menu_add_separator(VALUE self) {
  QObject *o = get_obj(self);
  if (QMenu *m = qobject_cast<QMenu *>(o))            m->addSeparator();
  else if (QToolBar *t = qobject_cast<QToolBar *>(o)) t->addSeparator();
  return self;
}
static VALUE menubar_add_menu(VALUE self, VALUE title) {
  QMenu *m = qcast<QMenuBar>(self)->addMenu(rb_to_qs(title));
  return wrap_obj(cMenu, m, false);   // menu bar owns it
}
static VALUE mainwindow_menubar(VALUE self) {
  return wrap_obj(cMenuBar, qcast<QMainWindow>(self)->menuBar(), false);
}
static VALUE mainwindow_statusbar(VALUE self) {
  return wrap_obj(cStatusBar, qcast<QMainWindow>(self)->statusBar(), false);
}
static VALUE statusbar_show_message(int argc, VALUE *argv, VALUE self) {
  VALUE msg, ms; rb_scan_args(argc, argv, "11", &msg, &ms);
  qcast<QStatusBar>(self)
    ->showMessage(rb_to_qs(msg), NIL_P(ms) ? 0 : NUM2INT(ms));
  return self;
}

// ---- numeric widgets -------------------------------------------------------
static VALUE range_set_range(VALUE self, VALUE lo, VALUE hi) {
  QObject *o = get_obj(self);
  if (QProgressBar *p = qobject_cast<QProgressBar *>(o)) p->setRange(NUM2INT(lo), NUM2INT(hi));
  else if (QSlider *sl = qobject_cast<QSlider *>(o))     sl->setRange(NUM2INT(lo), NUM2INT(hi));
  else if (QSpinBox *sb = qobject_cast<QSpinBox *>(o))   sb->setRange(NUM2INT(lo), NUM2INT(hi));
  else rb_raise(rb_eRuntimeError, "setRange: unsupported receiver");
  return self;
}
static VALUE dspin_set_value(VALUE self, VALUE v) {
  qcast<QDoubleSpinBox>(self)->setValue(NUM2DBL(v)); return self;
}
static VALUE dspin_value(VALUE self) {
  return DBL2NUM(qcast<QDoubleSpinBox>(self)->value());
}
static VALUE dspin_set_range(VALUE self, VALUE lo, VALUE hi) {
  qcast<QDoubleSpinBox>(self)->setRange(NUM2DBL(lo), NUM2DBL(hi));
  return self;
}

// ---- timer -----------------------------------------------------------------
static VALUE timer_start(int argc, VALUE *argv, VALUE self) {
  VALUE ms; rb_scan_args(argc, argv, "01", &ms);
  QTimer *t = qcast<QTimer>(self);
  if (NIL_P(ms)) t->start(); else t->start(NUM2INT(ms));
  return self;
}

// ---------------------------------------------------------------------------
// qtbindings compatibility shims.
//
// COSMOS calls these; they are NOT Qt API, they are qtbindings/Smoke
// internals. Providing them here means COSMOS source does not have to change.
// ---------------------------------------------------------------------------
static const char *COMPAT_RUBY =
  "module Qt\n"
  "  # Only ever used in `x.is_a? Qt::Enum` tests. Our enums are plain\n"
  "  # Integers, so the test correctly returns false.\n"
  "  class Enum; end\n"
  "  # Stand-in for a C++ bool& out-parameter.\n"
  "  class Boolean\n"
  "    attr_accessor :value\n"
  "    def initialize(v = false); @value = v; end\n"
  "    def nil?; @value.nil?; end\n"
  "  end\n"
  "  # qtbindings needed a hand-rolled queue because Qt's event loop starved\n"
  "  # Ruby threads. This binding solves that properly (the GVL is released\n"
  "  # around exec() and re-acquired for callbacks), so the queue is always\n"
  "  # empty and COSMOS's `pop.call until empty?` drain loops exit at once.\n"
  "  module RubyThreadFix\n"
  "    def self.queue; @queue ||= Queue.new; end\n"
  "  end\n"
  "  module Internal\n"
  "    def self.setDebug(*args); nil; end\n"
  "    def self.debug_level=(v); nil; end\n"
  "  end\n"
  "  module QtDebugChannel\n"
  "    QTDB_NONE = 0; QTDB_AMBIGUOUS = 1; QTDB_METHOD_MISSING = 2\n"
  "    QTDB_CALLS = 4; QTDB_GC = 8; QTDB_VIRTUAL = 16\n"
  "    QTDB_VERBOSE = 32; QTDB_ALL = 0xFFFF\n"
  "  end\n"
  "  module DebugLevel\n"
  "    Off = 0; Low = 1; High = 2; Extensive = 3\n"
  "  end\n"
  "\n"
  "  # qtbindings encodes SIGNAL()/SLOT() as a '2'/'1' prefix on the signature.\n"
  "  def self.__sig(s)\n"
  "    t = s.to_s\n"
  "    t = t[1..-1] if t =~ /\\A[12]/\n"
  "    # Ruby-declared signals are looked up by this string, so the declaration\n"
  "    # and the connect must normalize to the same key. ruby_editor.rb:27\n"
  "    # declares 'font_changed(const QFont &)' while script_runner_frame.rb:273\n"
  "    # connects 'font_changed(QFont)'; unnormalized those are different hash\n"
  "    # keys and every emission is silently dropped. Mirror what\n"
  "    # QMetaObject::normalizedSignature does to the parameter list.\n"
  "    name, _, params = t.partition(\'(\')\n"
  "    return t if params.empty?\n"
  "    params = params.chomp(\')\')\n"
  "    norm = params.split(\',\').map do |p|\n"
  "      p = p.strip.sub(/\\Aconst\\s+/, \'\')\n"
  "      p = p.sub(/\\s*&\\z/, \'\')\n"
  "      p.gsub(/\\s*\\*/, \'*\').gsub(/\\s+/, \' \').strip\n"
  "    end.reject(&:empty?)\n"
  "    name.strip + \'(\' + norm.join(\',\') + \')\'\n"
  "  end\n"
  "\n"
  "end\n"
  "\n"
  "module Kernel\n"
  "  def SIGNAL(s); \"2\" + s.to_s; end\n"
  "  def SLOT(s);   \"1\" + s.to_s; end\n"
  "end\n";

static VALUE cPalette, cCursor, cTextCursor, cInputDialog, cShortcut;

// ---- Qt::Palette -----------------------------------------------------------
static VALUE palette_new(int argc, VALUE *argv, VALUE klass) {
  VALUE c; rb_scan_args(argc, argv, "01", &c);
  if (NIL_P(c)) return wrap_val<QPalette>(klass, QPalette());
  if (RB_TYPE_P(c, T_FIXNUM))
    return wrap_val<QPalette>(klass, QPalette(QColor((Qt::GlobalColor)NUM2INT(c))));
  return wrap_val<QPalette>(klass, QPalette(*get_val<QColor>(c)));
}
static VALUE widget_set_palette(VALUE self, VALUE p) {
  qcast<QWidget>(self)->setPalette(*get_val<QPalette>(p));
  return self;
}

// ---- Qt::Cursor ------------------------------------------------------------
static VALUE cursor_new(VALUE klass, VALUE shape) {
  return wrap_val<QCursor>(klass, QCursor((Qt::CursorShape)NUM2INT(shape)));
}
static VALUE cursor_pos(VALUE klass) {
  return wrap_val<QPoint>(cPoint, QCursor::pos());
}
static VALUE cursor_set_pos(VALUE klass, VALUE x, VALUE y) {
  QCursor::setPos(NUM2INT(x), NUM2INT(y));
  return Qnil;
}
static VALUE widget_set_cursor(VALUE self, VALUE c) {
  qcast<QWidget>(self)->setCursor(*get_val<QCursor>(c));
  return self;
}

// ---- scroll bar policy on scroll areas -------------------------------------
static VALUE scroll_set_hpolicy(VALUE self, VALUE v) {
  qcast<QAbstractScrollArea>(self)
    ->setHorizontalScrollBarPolicy((Qt::ScrollBarPolicy)NUM2INT(v));
  return self;
}
static VALUE scroll_set_vpolicy(VALUE self, VALUE v) {
  qcast<QAbstractScrollArea>(self)
    ->setVerticalScrollBarPolicy((Qt::ScrollBarPolicy)NUM2INT(v));
  return self;
}

// ---- Qt::InputDialog.getText(parent, title, label, echo, text, boolean) ----
static VALUE inputdlg_get_text(int argc, VALUE *argv, VALUE klass) {
  VALUE parent, title, label, echo, text, okref;
  rb_scan_args(argc, argv, "33", &parent, &title, &label, &echo, &text, &okref);
  bool ok = false;
  QLineEdit::EchoMode mode = NIL_P(echo)
      ? QLineEdit::Normal : (QLineEdit::EchoMode)NUM2INT(echo);
  QWidget *pw = opt_parent(parent);
  const QString ti = rb_to_qs(title), la = rb_to_qs(label), tx = rb_to_qs(text);
  QString r;
  ruby_without_gvl([&] { r = QInputDialog::getText(pw, ti, la, mode, tx, &ok); });
  if (!NIL_P(okref) && rb_respond_to(okref, rb_intern("value=")))
    rb_funcall(okref, rb_intern("value="), 1, ok ? Qtrue : Qfalse);
  return ok ? rb_str_new2(r.toUtf8().constData()) : Qnil;
}

// ---- line edit echo mode ---------------------------------------------------
// QInputDialog::getDouble(parent, title, label, value, min, max, decimals, &ok)
static VALUE inputdlg_get_double(int argc, VALUE *argv, VALUE klass) {
  VALUE parent, title, label, value, lo, hi, dec, okref;
  rb_scan_args(argc, argv, "35", &parent, &title, &label, &value, &lo, &hi, &dec, &okref);
  bool ok = false;
  QWidget *pw = opt_parent(parent);
  const QString ti = rb_to_qs(title), la = rb_to_qs(label);
  const double v  = NIL_P(value) ? 0.0 : NUM2DBL(value);
  const double lv = NIL_P(lo) ? -2147483647.0 : NUM2DBL(lo);
  const double hv = NIL_P(hi) ?  2147483647.0 : NUM2DBL(hi);
  const int    dv = NIL_P(dec) ? 1 : NUM2INT(dec);
  double r = 0.0;
  ruby_without_gvl([&] { r = QInputDialog::getDouble(pw, ti, la, v, lv, hv, dv, &ok); });
  if (!NIL_P(okref) && rb_respond_to(okref, rb_intern("value=")))
    rb_funcall(okref, rb_intern("value="), 1, ok ? Qtrue : Qfalse);
  return ok ? DBL2NUM(r) : Qnil;
}
static VALUE lineedit_set_echo(VALUE self, VALUE m) {
  qcast<QLineEdit>(self)->setEchoMode((QLineEdit::EchoMode)NUM2INT(m));
  return self;
}

// ---- painting value types (Pen / Brush / Gradient / FontMetrics) ----------
static VALUE cPen, cBrush, cLinearGradient, cFontMetrics, cPixmap, cImage, cSettings;

// COSMOS caches colours in Hashes (BRUSHES[color], PENS[color]), so QColor
// wrappers need value equality and a matching hash, not object identity.
static VALUE color_eq(VALUE self, VALUE other) {
  if (!rb_obj_is_kind_of(other, cColor)) return Qfalse;
  return (*get_val<QColor>(self) == *get_val<QColor>(other)) ? Qtrue : Qfalse;
}
static VALUE color_hash(VALUE self) {
  return INT2NUM((int)get_val<QColor>(self)->rgba());
}

static QColor color_arg(VALUE v) {
  if (NIL_P(v)) return QColor();
  if (RB_TYPE_P(v, T_FIXNUM)) return QColor((Qt::GlobalColor)NUM2INT(v));
  if (rb_obj_is_kind_of(v, cColor)) return *get_val<QColor>(v);
  return QColor(rb_to_qs(v));
}

static VALUE pen_new(int argc, VALUE *argv, VALUE klass) {
  VALUE c; rb_scan_args(argc, argv, "01", &c);
  return NIL_P(c) ? wrap_val<QPen>(klass, QPen())
                  : wrap_val<QPen>(klass, QPen(color_arg(c)));
}
static VALUE pen_set_color(VALUE self, VALUE c) { get_val<QPen>(self)->setColor(color_arg(c)); return self; }
static VALUE pen_set_width(VALUE self, VALUE w) { get_val<QPen>(self)->setWidth(NUM2INT(w)); return self; }
static VALUE pen_set_style(VALUE self, VALUE v) {
  get_val<QPen>(self)->setStyle((Qt::PenStyle)NUM2INT(v)); return self;
}
static VALUE pen_color(VALUE self) { return wrap_val<QColor>(cColor, get_val<QPen>(self)->color()); }

static VALUE brush_new(int argc, VALUE *argv, VALUE klass) {
  VALUE c, style; rb_scan_args(argc, argv, "02", &c, &style);
  if (NIL_P(c)) return wrap_val<QBrush>(klass, QBrush());
  if (rb_obj_is_kind_of(c, cLinearGradient))
    return wrap_val<QBrush>(klass, QBrush(*get_val<QLinearGradient>(c)));
  if (NIL_P(style)) return wrap_val<QBrush>(klass, QBrush(color_arg(c)));
  return wrap_val<QBrush>(klass, QBrush(color_arg(c), (Qt::BrushStyle)NUM2INT(style)));
}
static VALUE brush_set_color(VALUE self, VALUE c) { get_val<QBrush>(self)->setColor(color_arg(c)); return self; }
static VALUE brush_color(VALUE self) { return wrap_val<QColor>(cColor, get_val<QBrush>(self)->color()); }

// COSMOS calls both forms: (x1,y1,x2,y2) and (QPointF, QPointF).
static VALUE lingrad_new(int argc, VALUE *argv, VALUE klass) {
  if (argc == 2)
    return wrap_val<QLinearGradient>(klass,
      QLinearGradient(*get_val<QPointF>(argv[0]), *get_val<QPointF>(argv[1])));
  if (argc == 4)
    return wrap_val<QLinearGradient>(klass,
      QLinearGradient(NUM2DBL(argv[0]), NUM2DBL(argv[1]),
                      NUM2DBL(argv[2]), NUM2DBL(argv[3])));
  rb_raise(rb_eArgError, "LinearGradient.new expects 2 points or 4 coordinates");
}
static VALUE lingrad_set_coord_mode(VALUE self, VALUE m) {
  get_val<QLinearGradient>(self)->setCoordinateMode((QGradient::CoordinateMode)NUM2INT(m));
  return self;
}
static VALUE lingrad_set_color_at(VALUE self, VALUE pos, VALUE c) {
  get_val<QLinearGradient>(self)->setColorAt(NUM2DBL(pos), color_arg(c));
  return self;
}

static VALUE fontmetrics_new(VALUE klass, VALUE f) {
  return wrap_val<QFontMetrics>(klass, QFontMetrics(*get_val<QFont>(f)));
}
static VALUE fm_width(VALUE self, VALUE str) {
  // QFontMetrics::width() was removed in Qt6; horizontalAdvance() replaces it.
  return INT2NUM(get_val<QFontMetrics>(self)->horizontalAdvance(rb_to_qs(str)));
}
// QFontMetrics::size(flags, text) -> QSize. scroll_text_dialog.rb:35 uses it
// to size the "Unexpected text output" dialog; without it that dialog -- the
// one COSMOS shows when anything writes to stderr -- raises NoMethodError.
static VALUE fm_size(VALUE self, VALUE flags, VALUE text) {
  QSize sz = get_val<QFontMetrics>(self)->size(NUM2INT(flags), rb_to_qs(text));
  return wrap_val<QSize>(cSize, sz);
}
static VALUE fm_height(VALUE self)  { return INT2NUM(get_val<QFontMetrics>(self)->height()); }
static VALUE fm_ascent(VALUE self)  { return INT2NUM(get_val<QFontMetrics>(self)->ascent()); }
static VALUE fm_descent(VALUE self) { return INT2NUM(get_val<QFontMetrics>(self)->descent()); }

// ---- palette colour/brush setters -----------------------------------------
static VALUE palette_set_color(int argc, VALUE *argv, VALUE self) {
  VALUE a, b, c; rb_scan_args(argc, argv, "21", &a, &b, &c);
  QPalette *pal = get_val<QPalette>(self);
  if (NIL_P(c)) pal->setColor((QPalette::ColorRole)NUM2INT(a), color_arg(b));
  else          pal->setColor((QPalette::ColorGroup)NUM2INT(a),
                              (QPalette::ColorRole)NUM2INT(b), color_arg(c));
  return self;
}
static VALUE palette_set_brush(int argc, VALUE *argv, VALUE self) {
  VALUE a, b, c; rb_scan_args(argc, argv, "21", &a, &b, &c);
  QPalette *pal = get_val<QPalette>(self);
  if (NIL_P(c)) pal->setBrush((QPalette::ColorRole)NUM2INT(a), *get_val<QBrush>(b));
  else          pal->setBrush((QPalette::ColorGroup)NUM2INT(a),
                              (QPalette::ColorRole)NUM2INT(b), *get_val<QBrush>(c));
  return self;
}

// ---- pixmap / icon / image / settings --------------------------------------
static VALUE pixmap_new(int argc, VALUE *argv, VALUE klass) {
  VALUE a, b; rb_scan_args(argc, argv, "02", &a, &b);
  if (NIL_P(a)) return wrap_val<QPixmap>(klass, QPixmap());
  if (NIL_P(b)) return wrap_val<QPixmap>(klass, QPixmap(rb_to_qs(a)));
  return wrap_val<QPixmap>(klass, QPixmap(NUM2INT(a), NUM2INT(b)));
}
static VALUE pixmap_width(VALUE self)  { return INT2NUM(get_val<QPixmap>(self)->width()); }
static VALUE pixmap_height(VALUE self) { return INT2NUM(get_val<QPixmap>(self)->height()); }
static VALUE pixmap_is_null(VALUE self){ return get_val<QPixmap>(self)->isNull() ? Qtrue : Qfalse; }

static VALUE icon_new(int argc, VALUE *argv, VALUE klass) {
  VALUE a; rb_scan_args(argc, argv, "01", &a);
  if (NIL_P(a)) return wrap_val<QIcon>(klass, QIcon());
  if (rb_obj_is_kind_of(a, cPixmap)) return wrap_val<QIcon>(klass, QIcon(*get_val<QPixmap>(a)));
  return wrap_val<QIcon>(klass, QIcon(rb_to_qs(a)));
}
static VALUE icon_is_null(VALUE self) { return get_val<QIcon>(self)->isNull() ? Qtrue : Qfalse; }

static VALUE image_new(int argc, VALUE *argv, VALUE klass) {
  VALUE a; rb_scan_args(argc, argv, "01", &a);
  return NIL_P(a) ? wrap_val<QImage>(klass, QImage())
                  : wrap_val<QImage>(klass, QImage(rb_to_qs(a)));
}
static VALUE image_width(VALUE self)  { return INT2NUM(get_val<QImage>(self)->width()); }
static VALUE image_height(VALUE self) { return INT2NUM(get_val<QImage>(self)->height()); }

static VALUE settings_init(int argc, VALUE *argv, VALUE self) {
  if (get_wrap(self)->ptr) return self;
  VALUE org, app2; rb_scan_args(argc, argv, "02", &org, &app2);
  QSettings *st = (NIL_P(org) || NIL_P(app2))
      ? new QSettings() : new QSettings(rb_to_qs(org), rb_to_qs(app2));
  attach(self, st, true);
  return self;
}
static VALUE settings_set_value(VALUE self, VALUE k, VALUE v) {
  QSettings *st = qcast<QSettings>(self);
  if (rb_obj_is_kind_of(v, cVariant)) st->setValue(rb_to_qs(k), *get_val<QVariant>(v));
  else if (RB_TYPE_P(v, T_FIXNUM))    st->setValue(rb_to_qs(k), NUM2INT(v));
  else                                 st->setValue(rb_to_qs(k), rb_to_qs(v));
  return self;
}
static VALUE settings_value(VALUE self, VALUE k) {
  return wrap_val<QVariant>(cVariant, qcast<QSettings>(self)->value(rb_to_qs(k)));
}

// ---- layout items (COSMOS's removeAll walks takeAt/layout/widget) ---------
static VALUE cLayoutItem, cStackedLayout;

static VALUE layout_take_at(VALUE self, VALUE i) {
  QLayoutItem *it = qcast<QLayout>(self)->takeAt(NUM2INT(i));
  return it ? wrap_ptr<QLayoutItem>(cLayoutItem, it) : Qnil;
}
static VALUE layoutitem_widget(VALUE self) {
  QWidget *w = get_ptr<QLayoutItem>(self)->widget();
  // Resolve the most specific bound class: a widget pulled back out of a
  // layout must come back as the Qt::Label (or whatever) it actually is, or
  // every accessor on it is a NoMethodError.
  return w ? wrap_obj(best_ruby_class(w, cWidget), w, false) : Qnil;
}
static VALUE layoutitem_layout(VALUE self) {
  QLayout *l = get_ptr<QLayoutItem>(self)->layout();
  return l ? wrap_obj(cLayout, l, false) : Qnil;
}
static VALUE layout_set_spacing(VALUE self, VALUE v) {
  qcast<QLayout>(self)->setSpacing(NUM2INT(v));
  return self;
}
static VALUE layout_set_margins(VALUE self, VALUE l, VALUE t, VALUE r, VALUE b) {
  qcast<QLayout>(self)
    ->setContentsMargins(NUM2INT(l), NUM2INT(t), NUM2INT(r), NUM2INT(b));
  return self;
}

// ---- validators / completer -------------------------------------------------
static VALUE cIntValidator, cDoubleValidator, cCompleter, cPainter, cRect, cPointF;
static VALUE cTextBlockKlass = Qnil;   // hoisted: used by the text-editing accessors above
static VALUE cPolygon, cTextDocument, cTextCharFormat, cTextOption, cUrl, cDate;

static VALUE intval_init(int argc, VALUE *argv, VALUE self) {
  if (get_wrap(self)->ptr) return self;
  VALUE lo, hi; rb_scan_args(argc, argv, "02", &lo, &hi);
  QIntValidator *v = (NIL_P(lo) || NIL_P(hi))
      ? new QIntValidator() : new QIntValidator(NUM2INT(lo), NUM2INT(hi));
  attach(self, v, true);
  return self;
}
static VALUE dblval_init(int argc, VALUE *argv, VALUE self) {
  if (get_wrap(self)->ptr) return self;
  VALUE lo, hi, dec; rb_scan_args(argc, argv, "03", &lo, &hi, &dec);
  QDoubleValidator *v = (NIL_P(lo) || NIL_P(hi))
      ? new QDoubleValidator()
      : new QDoubleValidator(NUM2DBL(lo), NUM2DBL(hi), NIL_P(dec) ? 2 : NUM2INT(dec));
  attach(self, v, true);
  return self;
}
static VALUE dblval_set_notation(VALUE self, VALUE n) {
  qcast<QDoubleValidator>(self)
    ->setNotation((QDoubleValidator::Notation)NUM2INT(n));
  return self;
}
static VALUE dblval_set_bottom(VALUE self, VALUE v) {
  qcast<QDoubleValidator>(self)->setBottom(NUM2DBL(v)); return self;
}
static VALUE dblval_set_top(VALUE self, VALUE v) {
  qcast<QDoubleValidator>(self)->setTop(NUM2DBL(v)); return self;
}
static VALUE dblval_set_range(VALUE self, VALUE lo, VALUE hi) {
  qcast<QDoubleValidator>(self)->setRange(NUM2DBL(lo), NUM2DBL(hi)); return self;
}
static VALUE intval_set_bottom(VALUE self, VALUE v) {
  qcast<QIntValidator>(self)->setBottom(NUM2INT(v)); return self;
}
static VALUE intval_set_top(VALUE self, VALUE v) {
  qcast<QIntValidator>(self)->setTop(NUM2INT(v)); return self;
}
static VALUE intval_set_range(VALUE self, VALUE lo, VALUE hi) {
  qcast<QIntValidator>(self)->setRange(NUM2INT(lo), NUM2INT(hi)); return self;
}
static VALUE lineedit_set_validator(VALUE self, VALUE v) {
  qcast<QLineEdit>(self)
    ->setValidator(qobject_cast<QValidator *>(get_obj(v)));
  release_ownership(v);
  return self;
}
static VALUE completer_init(int argc, VALUE *argv, VALUE self) {
  if (get_wrap(self)->ptr) return self;
  VALUE list; rb_scan_args(argc, argv, "01", &list);
  QStringList sl;
  if (!NIL_P(list) && RB_TYPE_P(list, T_ARRAY))
    for (long i = 0; i < RARRAY_LEN(list); i++) sl << rb_to_qs(rb_ary_entry(list, i));
  attach(self, new QCompleter(sl), true);
  return self;
}
static VALUE lineedit_set_completer(VALUE self, VALUE c) {
  qcast<QLineEdit>(self)
    ->setCompleter(qobject_cast<QCompleter *>(get_obj(c)));
  release_ownership(c);
  return self;
}

// ---- geometry value types --------------------------------------------------
static VALUE rect_new(VALUE klass, VALUE x, VALUE y, VALUE w, VALUE h) {
  return wrap_val<QRect>(klass, QRect(NUM2INT(x), NUM2INT(y), NUM2INT(w), NUM2INT(h)));
}
static VALUE rect_w(VALUE self) { return INT2NUM(get_val<QRect>(self)->width()); }
static VALUE rect_h(VALUE self) { return INT2NUM(get_val<QRect>(self)->height()); }
static VALUE pointf_new(VALUE klass, VALUE x, VALUE y) {
  return wrap_val<QPointF>(klass, QPointF(NUM2DBL(x), NUM2DBL(y)));
}
static VALUE pointf_x(VALUE self) { return DBL2NUM(get_val<QPointF>(self)->x()); }
static VALUE pointf_y(VALUE self) { return DBL2NUM(get_val<QPointF>(self)->y()); }

// ---- url / date ------------------------------------------------------------
static VALUE url_new(VALUE klass, VALUE s2) { return wrap_val<QUrl>(klass, QUrl(rb_to_qs(s2))); }
static VALUE url_to_s(VALUE self) { return rb_str_new2(get_val<QUrl>(self)->toString().toUtf8().constData()); }
static VALUE desktop_open_url(VALUE klass, VALUE u) {
  QUrl url = rb_obj_is_kind_of(u, cUrl) ? *get_val<QUrl>(u) : QUrl(rb_to_qs(u));
  return QDesktopServices::openUrl(url) ? Qtrue : Qfalse;
}

// ---- Qt::TextCharFormat (syntax highlighting in script_runner et al) -------
static VALUE tcf_new(VALUE klass) { return wrap_val<QTextCharFormat>(klass, QTextCharFormat()); }
static VALUE tcf_set_font(VALUE self, VALUE f) {
  get_val<QTextCharFormat>(self)->setFont(*get_val<QFont>(f));
  return self;
}
static VALUE tcf_font(VALUE self) {
  return wrap_val<QFont>(cFont, get_val<QTextCharFormat>(self)->font());
}
// currentCharFormat / setCurrentCharFormat exist on both QPlainTextEdit and
// QTextEdit; dispatch on whichever this actually is.
static VALUE edit_current_char_format(VALUE self) {
  QObject *o = get_obj(self);
  if (QPlainTextEdit *p = qobject_cast<QPlainTextEdit *>(o))
    return wrap_val<QTextCharFormat>(cTextCharFormat, p->currentCharFormat());
  if (QTextEdit *t = qobject_cast<QTextEdit *>(o))
    return wrap_val<QTextCharFormat>(cTextCharFormat, t->currentCharFormat());
  rb_raise(rb_eRuntimeError, "currentCharFormat: unsupported receiver");
}
static VALUE edit_set_current_char_format(VALUE self, VALUE fmt) {
  QObject *o = get_obj(self);
  QTextCharFormat *f = get_val<QTextCharFormat>(fmt);
  if (QPlainTextEdit *p = qobject_cast<QPlainTextEdit *>(o)) { p->setCurrentCharFormat(*f); return self; }
  if (QTextEdit *t = qobject_cast<QTextEdit *>(o))           { t->setCurrentCharFormat(*f); return self; }
  rb_raise(rb_eRuntimeError, "setCurrentCharFormat: unsupported receiver");
}

static VALUE tcf_set_foreground(VALUE self, VALUE v) {
  QBrush b = rb_obj_is_kind_of(v, cBrush) ? *get_val<QBrush>(v) : QBrush(color_arg(v));
  get_val<QTextCharFormat>(self)->setForeground(b);
  return self;
}
static VALUE tcf_set_font_weight(VALUE self, VALUE w) {
  get_val<QTextCharFormat>(self)->setFontWeight(NUM2INT(w));
  return self;
}
static VALUE tcf_set_font_italic(VALUE self, VALUE b) {
  get_val<QTextCharFormat>(self)->setFontItalic(RTEST(b));
  return self;
}

// ---- Qt::Char --------------------------------------------------------------
static VALUE qchar_new(VALUE klass, VALUE v) {
  if (RB_TYPE_P(v, T_STRING)) {
    QString q = rb_to_qs(v);
    return wrap_val<QChar>(klass, q.isEmpty() ? QChar() : q.at(0));
  }
  return wrap_val<QChar>(klass, QChar((ushort)NUM2UINT(v)));
}
static VALUE qchar_to_s(VALUE self) {
  return rb_str_new2(QString(*get_val<QChar>(self)).toUtf8().constData());
}
static VALUE qchar_unicode(VALUE self) { return UINT2NUM(get_val<QChar>(self)->unicode()); }

// ---- rendering a widget to an image (works under the offscreen platform) ---
static VALUE widget_grab(VALUE self) {
  QWidget *w = qcast<QWidget>(self);
  return wrap_val<QPixmap>(cPixmap, w->grab());
}
static VALUE pixmap_to_image(VALUE self) {
  return wrap_val<QImage>(cImage, get_val<QPixmap>(self)->toImage());
}
static VALUE image_save(VALUE self, VALUE path) {
  return get_val<QImage>(self)->save(rb_to_qs(path)) ? Qtrue : Qfalse;
}
static VALUE image_is_null(VALUE self) { return get_val<QImage>(self)->isNull() ? Qtrue : Qfalse; }
static VALUE image_pixel_color(VALUE self, VALUE x, VALUE y) {
  return wrap_val<QColor>(cColor, get_val<QImage>(self)->pixelColor(NUM2INT(x), NUM2INT(y)));
}
static VALUE pixmap_save(VALUE self, VALUE path) {
  return get_val<QPixmap>(self)->save(rb_to_qs(path)) ? Qtrue : Qfalse;
}

// ---- common widget geometry / alignment helpers ----------------------------
static VALUE lineedit_set_alignment(VALUE self, VALUE a) {
  qcast<QLineEdit>(self)->setAlignment((Qt::Alignment)NUM2INT(a));
  return self;
}
static VALUE label_set_alignment(VALUE self, VALUE a) {
  qcast<QLabel>(self)->setAlignment((Qt::Alignment)NUM2INT(a));
  return self;
}
// QLayout::setAlignment has both setAlignment(align) and
// setAlignment(widget, align) forms; COSMOS uses both.
static VALUE layout_set_alignment(int argc, VALUE *argv, VALUE self) {
  QLayout *l = qcast<QLayout>(self);
  if (argc == 1) { l->setAlignment((Qt::Alignment)NUM2INT(argv[0])); return self; }
  VALUE target = argv[0];
  Qt::Alignment al = (Qt::Alignment)NUM2INT(argv[1]);
  QObject *t = get_obj(target);
  if (QWidget *w = qobject_cast<QWidget *>(t))      l->setAlignment(w, al);
  else if (QLayout *sub = qobject_cast<QLayout *>(t)) l->setAlignment(sub, al);
  else rb_raise(rb_eArgError, "setAlignment: expected a widget or layout");
  return self;
}
static VALUE twitem_set_text_alignment(VALUE self, VALUE a) {
  get_ptr<QTableWidgetItem>(self)->setTextAlignment((Qt::Alignment)NUM2INT(a));
  return self;
}
static VALUE frame_set_style(VALUE self, VALUE v) {
  qcast<QFrame>(self)->setFrameStyle(NUM2INT(v));
  return self;
}
static VALUE widget_set_fixed_size(VALUE self, VALUE w, VALUE h) {
  qcast<QWidget>(self)->setFixedSize(NUM2INT(w), NUM2INT(h));
  return self;
}

// ---- box / grid layout extras ----------------------------------------------
static VALUE box_add_stretch(int argc, VALUE *argv, VALUE self) {
  VALUE n; rb_scan_args(argc, argv, "01", &n);
  qcast<QBoxLayout>(self)->addStretch(NIL_P(n) ? 0 : NUM2INT(n));
  return self;
}
static VALUE box_add_spacing(VALUE self, VALUE n) {
  qcast<QBoxLayout>(self)->addSpacing(NUM2INT(n));
  return self;
}
static VALUE box_insert_widget(int argc, VALUE *argv, VALUE self) {
  VALUE idx, w, stretch; rb_scan_args(argc, argv, "21", &idx, &w, &stretch);
  qcast<QBoxLayout>(self)
    ->insertWidget(NUM2INT(idx), qobject_cast<QWidget *>(get_obj(w)),
                   NIL_P(stretch) ? 0 : NUM2INT(stretch));
  release_ownership(w);
  return self;
}
static VALUE grid_add_widget(int argc, VALUE *argv, VALUE self) {
  VALUE w, row, col, rs, cs; rb_scan_args(argc, argv, "32", &w, &row, &col, &rs, &cs);
  QGridLayout *g = qcast<QGridLayout>(self);
  QWidget *widget = qobject_cast<QWidget *>(get_obj(w));
  if (NIL_P(rs)) g->addWidget(widget, NUM2INT(row), NUM2INT(col));
  else           g->addWidget(widget, NUM2INT(row), NUM2INT(col),
                              NUM2INT(rs), NIL_P(cs) ? 1 : NUM2INT(cs));
  release_ownership(w);
  return self;
}
static VALUE grid_set_row_stretch(VALUE self, VALUE r, VALUE v) {
  qcast<QGridLayout>(self)->setRowStretch(NUM2INT(r), NUM2INT(v));
  return self;
}
static VALUE grid_set_col_stretch(VALUE self, VALUE c, VALUE v) {
  qcast<QGridLayout>(self)->setColumnStretch(NUM2INT(c), NUM2INT(v));
  return self;
}

// ---- high-frequency widget/item methods from the COSMOS call-site scan -----
static VALUE widget_set_hidden(VALUE self, VALUE b) {
  qcast<QWidget>(self)->setHidden(RTEST(b)); return self;
}
static VALUE widget_set_disabled(VALUE self, VALUE b) {
  qcast<QWidget>(self)->setDisabled(RTEST(b)); return self;
}
static VALUE widget_set_ctx_menu_policy(VALUE self, VALUE v) {
  qcast<QWidget>(self)->setContextMenuPolicy((Qt::ContextMenuPolicy)NUM2INT(v));
  return self;
}
static VALUE obj_set_object_name(VALUE self, VALUE v) {
  get_obj(self)->setObjectName(rb_to_qs(v)); return self;
}
static VALUE obj_object_name(VALUE self) {
  return rb_str_new2(get_obj(self)->objectName().toUtf8().constData());
}
static VALUE obj_block_signals(VALUE self, VALUE b) {
  return get_obj(self)->blockSignals(RTEST(b)) ? Qtrue : Qfalse;
}
static VALUE label_set_buddy(VALUE self, VALUE w) {
  qcast<QLabel>(self)->setBuddy(qobject_cast<QWidget *>(get_obj(w)));
  return self;
}
static VALUE button_set_default(VALUE self, VALUE b) {
  qcast<QPushButton>(self)->setDefault(RTEST(b)); return self;
}
static VALUE combo_set_current_text(VALUE self, VALUE t) {
  qcast<QComboBox>(self)->setCurrentText(rb_to_qs(t)); return self;
}
static VALUE combo_add_items(VALUE self, VALUE ary) {
  QStringList sl;
  for (long i = 0; i < RARRAY_LEN(ary); i++) sl << rb_to_qs(rb_ary_entry(ary, i));
  QObject *o = get_obj(self);
  if (QComboBox *c = qobject_cast<QComboBox *>(o))        c->addItems(sl);
  else if (QListWidget *l = qobject_cast<QListWidget *>(o)) l->addItems(sl);
  else rb_raise(rb_eRuntimeError, "addItems: unsupported receiver");
  return self;
}
static VALUE combo_set_size_adjust(VALUE self, VALUE v) {
  qcast<QComboBox>(self)
    ->setSizeAdjustPolicy((QComboBox::SizeAdjustPolicy)NUM2INT(v));
  return self;
}
static VALUE tw_set_h_header_labels(VALUE self, VALUE ary) {
  QStringList sl;
  for (long i = 0; i < RARRAY_LEN(ary); i++) sl << rb_to_qs(rb_ary_entry(ary, i));
  qcast<QTableWidget>(self)->setHorizontalHeaderLabels(sl);
  return self;
}
static QBrush brush_arg(VALUE v) {
  if (rb_obj_is_kind_of(v, cBrush)) return *get_val<QBrush>(v);
  return QBrush(color_arg(v));
}
static VALUE twitem_set_foreground(VALUE self, VALUE v) {
  get_ptr<QTableWidgetItem>(self)->setForeground(brush_arg(v)); return self;
}
static VALUE twitem_set_background(VALUE self, VALUE v) {
  get_ptr<QTableWidgetItem>(self)->setBackground(brush_arg(v)); return self;
}
static VALUE twitem_set_font(VALUE self, VALUE f) {
  get_ptr<QTableWidgetItem>(self)->setFont(*get_val<QFont>(f)); return self;
}
static VALUE twitem_set_tooltip(VALUE self, VALUE t) {
  get_ptr<QTableWidgetItem>(self)->setToolTip(rb_to_qs(t)); return self;
}
static VALUE tritem_set_foreground(VALUE self, VALUE col, VALUE v) {
  get_ptr<QTreeWidgetItem>(self)->setForeground(NUM2INT(col), brush_arg(v)); return self;
}
static VALUE tritem_set_background(VALUE self, VALUE col, VALUE v) {
  get_ptr<QTreeWidgetItem>(self)->setBackground(NUM2INT(col), brush_arg(v)); return self;
}
static VALUE twitem_set_size_hint(VALUE self, VALUE sz) {
  get_ptr<QTableWidgetItem>(self)->setSizeHint(*get_val<QSize>(sz)); return self;
}
static VALUE twitem_set_data(VALUE self, VALUE role, VALUE v) {
  QVariant qv = rb_obj_is_kind_of(v, cVariant) ? *get_val<QVariant>(v)
              : (RB_TYPE_P(v, T_FIXNUM) ? QVariant(NUM2INT(v)) : QVariant(rb_to_qs(v)));
  get_ptr<QTableWidgetItem>(self)->setData(NUM2INT(role), qv);
  return self;
}
static VALUE twitem_data(VALUE self, VALUE role) {
  return wrap_val<QVariant>(cVariant, get_ptr<QTableWidgetItem>(self)->data(NUM2INT(role)));
}
static VALUE twitem_set_flags(VALUE self, VALUE f) {
  get_ptr<QTableWidgetItem>(self)->setFlags((Qt::ItemFlags)NUM2INT(f)); return self;
}
static VALUE twitem_set_check_state(VALUE self, VALUE st) {
  get_ptr<QTableWidgetItem>(self)->setCheckState((Qt::CheckState)NUM2INT(st)); return self;
}
static VALUE twitem_check_state(VALUE self) {
  return INT2NUM((int)get_ptr<QTableWidgetItem>(self)->checkState());
}
static VALUE layout_item_at(int argc, VALUE *argv, VALUE self) {
  QLayout *l = qcast<QLayout>(self);
  QLayoutItem *it = NULL;
  if (argc == 2) {
    // QFormLayout::itemAt(row, ItemRole) -- logging_tab.rb:53 uses it to read
    // the field widget of a given row back out of the form.
    QFormLayout *f = qobject_cast<QFormLayout *>(l);
    if (!f) rb_raise(rb_eArgError, "itemAt(row, role) requires a Qt::FormLayout");
    it = f->itemAt(NUM2INT(argv[0]), (QFormLayout::ItemRole)NUM2INT(argv[1]));
  } else if (argc == 1) {
    it = l->itemAt(NUM2INT(argv[0]));
  } else {
    rb_raise(rb_eArgError, "itemAt takes 1 or 2 arguments (given %d)", argc);
  }
  return it ? wrap_ptr<QLayoutItem>(cLayoutItem, it) : Qnil;
}
static VALUE layout_set_size_constraint(VALUE self, VALUE v) {
  qcast<QLayout>(self)->setSizeConstraint((QLayout::SizeConstraint)NUM2INT(v));
  return self;
}
static VALUE pte_append(VALUE self, VALUE t) {
  qcast<QPlainTextEdit>(self)->appendPlainText(rb_to_qs(t)); return self;
}
static VALUE tab_set_tab_text(VALUE self, VALUE i, VALUE t) {
  qcast<QTabWidget>(self)->setTabText(NUM2INT(i), rb_to_qs(t)); return self;
}
static VALUE splitter_set_stretch(VALUE self, VALUE i, VALUE f) {
  QObject *o = get_obj(self);
  if (QSplitter *sp = qobject_cast<QSplitter *>(o)) sp->setStretchFactor(NUM2INT(i), NUM2INT(f));
  else if (QBoxLayout *b = qobject_cast<QBoxLayout *>(o)) b->setStretch(NUM2INT(i), NUM2INT(f));
  return self;
}
static VALUE aiv_set_selection_mode(VALUE self, VALUE v) {
  qcast<QAbstractItemView>(self)
    ->setSelectionMode((QAbstractItemView::SelectionMode)NUM2INT(v));
  return self;
}
static VALUE aiv_set_edit_triggers(VALUE self, VALUE v) {
  qcast<QAbstractItemView>(self)
    ->setEditTriggers((QAbstractItemView::EditTriggers)NUM2INT(v));
  return self;
}
static VALUE app_set_override_cursor(VALUE klass, VALUE c) {
  QApplication::setOverrideCursor(*get_val<QCursor>(c)); return Qnil;
}
static VALUE app_restore_override_cursor(VALUE klass) {
  QApplication::restoreOverrideCursor(); return Qnil;
}

// ---- QApplication class-level helpers --------------------------------------
static VALUE app_active_window(VALUE klass) {
  QWidget *w = QApplication::activeWindow();
  return w ? wrap_obj(cWidget, w, false) : Qnil;
}
static VALUE app_quit(VALUE klass) { QCoreApplication::quit(); return Qnil; }
static VALUE app_exit(int argc, VALUE *argv, VALUE self) {
  int code = 0;
  if (argc > 0 && !NIL_P(argv[0])) code = NUM2INT(argv[0]);
  QCoreApplication::exit(code);
  return Qnil;
}
static VALUE app_top_level_widgets(VALUE klass) {
  QWidgetList ws = QApplication::topLevelWidgets();
  VALUE ary = rb_ary_new();
  for (int i = 0; i < ws.size(); i++)
    rb_ary_push(ary, wrap_obj(best_ruby_class(ws.at(i), cWidget), ws.at(i), false));
  return ary;
}
static VALUE app_active_modal(VALUE klass) {
  QWidget *w = QApplication::activeModalWidget();
  return w ? wrap_obj(cWidget, w, false) : Qnil;
}
static VALUE app_process_events_cls(VALUE klass) { QApplication::processEvents(); return Qnil; }
static VALUE app_instance(VALUE klass) {
  QCoreApplication *a = QCoreApplication::instance();
  return a ? wrap_obj(cApplication, a, false) : Qnil;
}

// ---- launch-path methods (Launcher / LegalDialog / QtTool.run) -------------
static QIcon icon_arg(VALUE v) {
  if (rb_obj_is_kind_of(v, cIcon))   return *get_val<QIcon>(v);
  if (rb_obj_is_kind_of(v, cPixmap)) return QIcon(*get_val<QPixmap>(v));
  return QIcon(rb_to_qs(v));
}
struct DlgExec { QDialog *dlg; int result; };
static void *exec_dialog_thunk(void *p) {
  DlgExec *e = (DlgExec *)p;
  e->result = e->dlg->exec();
  return NULL;
}
static VALUE dialog_exec(VALUE self) {
  DlgExec e;
  e.dlg = qcast<QDialog>(self);
  e.result = 0;
  rb_thread_call_without_gvl(exec_dialog_thunk, &e, RUBY_UBF_IO, NULL);
  return INT2NUM(e.result);
}
// Qt::MessageBox had only static helpers and constants -- no constructor and
// no instance methods -- so `Qt::MessageBox.new(parent)` raised ArgumentError
// at all 6 call sites, including exception_dialog.rb:44. That is the dialog
// COSMOS shows when anything goes wrong, so the error reporter crashed
// whenever it was needed. It descends from Qt::Dialog now, which supplies
// exec/dispose/setWindowTitle/font.
static QObject *ctor_message_box(int argc, VALUE *argv) {
  QWidget *p = parent_arg(argc, argv);
  if (p) { g_ctor_took_parent = true; return new QMessageBox(p); }
  return new QMessageBox();
}
static VALUE msgbox_set_icon(VALUE self, VALUE v) {
  qcast<QMessageBox>(self)->setIcon((QMessageBox::Icon)NUM2INT(v));
  return self;
}
static VALUE msgbox_set_text(VALUE self, VALUE v) {
  qcast<QMessageBox>(self)->setText(rb_to_qs(v));
  return self;
}
static VALUE msgbox_set_informative(VALUE self, VALUE v) {
  qcast<QMessageBox>(self)->setInformativeText(rb_to_qs(v));
  return self;
}
static VALUE msgbox_set_detailed(VALUE self, VALUE v) {
  qcast<QMessageBox>(self)->setDetailedText(rb_to_qs(v));
  return self;
}
static VALUE msgbox_set_text_format(VALUE self, VALUE v) {
  qcast<QMessageBox>(self)->setTextFormat((Qt::TextFormat)NUM2INT(v));
  return self;
}
static VALUE msgbox_set_standard_buttons(VALUE self, VALUE v) {
  qcast<QMessageBox>(self)->setStandardButtons(
    (QMessageBox::StandardButtons)QFlags<QMessageBox::StandardButton>(
      (QMessageBox::StandardButton)NUM2INT(v)));
  return self;
}
// COSMOS passes either a StandardButton constant or a QPushButton it added.
static VALUE msgbox_set_default_button(VALUE self, VALUE v) {
  QMessageBox *m = qcast<QMessageBox>(self);
  if (RB_TYPE_P(v, T_FIXNUM)) {
    m->setDefaultButton((QMessageBox::StandardButton)NUM2INT(v));
  } else if (QPushButton *b = qobject_cast<QPushButton *>(get_obj(v))) {
    m->setDefaultButton(b);
  }
  return self;
}
// addButton(text, role) -> the new button; addButton(button, role) -> button.
static VALUE msgbox_add_button(VALUE self, VALUE b, VALUE role) {
  QMessageBox *m = qcast<QMessageBox>(self);
  QMessageBox::ButtonRole r = (QMessageBox::ButtonRole)NUM2INT(role);
  if (RB_TYPE_P(b, T_STRING)) {
    QPushButton *btn = m->addButton(rb_to_qs(b), r);
    return btn ? wrap_obj(cPushButton, btn, false) : Qnil;
  }
  if (QAbstractButton *btn = qobject_cast<QAbstractButton *>(get_obj(b))) {
    m->addButton(btn, r);
    // QMessageBox reparents the button and will delete it. Without this the
    // Ruby wrapper still believes it owns the QPushButton, so both sides free
    // it and the process dies during teardown.
    release_ownership(b);
  }
  return b;
}
static VALUE msgbox_clicked_button(VALUE self) {
  QAbstractButton *b = qcast<QMessageBox>(self)->clickedButton();
  return b ? wrap_obj(cAbstractButton, b, false) : Qnil;
}

// Batch of Qt methods COSMOS calls that were never bound. Found by diffing
// every camelCase call in lib/cosmos/{gui,tools} against the binding's method
// table, rather than waiting for each one to surface as a click-time
// NoMethodError.
static VALUE widget_set_geometry(int argc, VALUE *argv, VALUE self) {
  QWidget *w = qcast<QWidget>(self);
  if (argc == 4) w->setGeometry(NUM2INT(argv[0]), NUM2INT(argv[1]),
                                NUM2INT(argv[2]), NUM2INT(argv[3]));
  else if (argc == 1) w->setGeometry(*get_val<QRect>(argv[0]));
  else rb_raise(rb_eArgError, "setGeometry takes a Rect or x,y,w,h");
  return self;
}
// setParent(parent) and setParent(parent, windowFlags) -- COSMOS uses the
// two-argument form to re-parent a dialog and set its flags at once
// (find_replace_dialog.rb:174).
static VALUE widget_set_parent(int argc, VALUE *argv, VALUE self) {
  QWidget *pw = (argc > 0 && !NIL_P(argv[0]))
    ? qobject_cast<QWidget *>(get_obj(argv[0])) : NULL;
  if (argc > 1 && !NIL_P(argv[1])) {
    qcast<QWidget>(self)->setParent(pw, (Qt::WindowFlags)QFlags<Qt::WindowType>(
      (Qt::WindowType)NUM2INT(argv[1])));
  } else {
    qcast<QWidget>(self)->setParent(pw);
  }
  if (pw) release_ownership(self);   // Qt owns it once it has a parent
  return self;
}
static VALUE widget_window_flags(VALUE self) {
  return INT2NUM((int)qcast<QWidget>(self)->windowFlags().toInt());
}
static VALUE tb_previous(VALUE self) {
  return wrap_val<QTextBlock>(cTextBlockKlass, get_val<QTextBlock>(self)->previous());
}
static VALUE edit_find(int argc, VALUE *argv, VALUE self) {
  QString needle = rb_to_qs(argv[0]);
  QObject *o = get_obj(self);
  bool found = false;
  if (QPlainTextEdit *p = qobject_cast<QPlainTextEdit *>(o))      found = p->find(needle);
  else if (QTextEdit *t = qobject_cast<QTextEdit *>(o))           found = t->find(needle);
  else rb_raise(rb_eRuntimeError, "find: unsupported receiver");
  return found ? Qtrue : Qfalse;
}
// Qt::Rect only had width/height; ruby_editor.rb:411 calls contains().
static VALUE rect_contains(int argc, VALUE *argv, VALUE self) {
  QRect *r = get_val<QRect>(self);
  if (argc >= 2 && RB_TYPE_P(argv[0], T_FIXNUM))
    return r->contains(NUM2INT(argv[0]), NUM2INT(argv[1])) ? Qtrue : Qfalse;
  if (argc >= 1 && rb_obj_is_kind_of(argv[0], cPoint))
    return r->contains(*get_val<QPoint>(argv[0])) ? Qtrue : Qfalse;
  if (argc >= 1 && rb_obj_is_kind_of(argv[0], cRect))
    return r->contains(*get_val<QRect>(argv[0])) ? Qtrue : Qfalse;
  return Qfalse;
}
static VALUE rect_x(VALUE self)      { return INT2NUM(get_val<QRect>(self)->x()); }
static VALUE rect_y(VALUE self)      { return INT2NUM(get_val<QRect>(self)->y()); }
static VALUE rect_right(VALUE self)  { return INT2NUM(get_val<QRect>(self)->right()); }
static VALUE rect_bottom(VALUE self) { return INT2NUM(get_val<QRect>(self)->bottom()); }
static VALUE rect_valid(VALUE self)  { return get_val<QRect>(self)->isValid() ? Qtrue : Qfalse; }
static VALUE rect_set_width(VALUE self, VALUE w) {
  get_val<QRect>(self)->setWidth(NUM2INT(w)); return self;
}
static VALUE doc_size(VALUE self) {
  // QTextDocument::size() is a QSizeF; COSMOS reads .height off it.
  QSizeF sz = qcast<QTextDocument>(self)->size();
  return wrap_val<QSize>(cSize, QSize((int)sz.width(), (int)sz.height()));
}
static VALUE tabwidget_tab_bar(VALUE self) {
  QTabBar *b = qcast<QTabWidget>(self)->tabBar();
  return b ? wrap_obj(best_ruby_class(b, cWidget), b, false) : Qnil;
}
static VALUE widget_rect(VALUE self) {
  return wrap_val<QRect>(cRect, qcast<QWidget>(self)->rect());
}
static VALUE widget_contents_rect(VALUE self) {
  return wrap_val<QRect>(cRect, qcast<QWidget>(self)->contentsRect());
}
static VALUE font_set_family(VALUE self, VALUE v) {
  get_val<QFont>(self)->setFamily(rb_to_qs(v)); return self;
}
static VALUE font_set_point_size(VALUE self, VALUE v) {
  get_val<QFont>(self)->setPointSize(NUM2INT(v)); return self;
}
static VALUE font_set_italic(VALUE self, VALUE v) {
  get_val<QFont>(self)->setItalic(RTEST(v)); return self;
}
static VALUE tw_h_header_item(VALUE self, VALUE i) {
  QTableWidgetItem *it = qcast<QTableWidget>(self)->horizontalHeaderItem(NUM2INT(i));
  return it ? wrap_ptr<QTableWidgetItem>(cTableWidgetItem, it) : Qnil;
}
static VALUE tw_v_header_item(VALUE self, VALUE i) {
  QTableWidgetItem *it = qcast<QTableWidget>(self)->verticalHeaderItem(NUM2INT(i));
  return it ? wrap_ptr<QTableWidgetItem>(cTableWidgetItem, it) : Qnil;
}
static VALUE tw_set_v_header_labels(VALUE self, VALUE ary) {
  QStringList l;
  for (long i = 0; i < RARRAY_LEN(ary); i++) l << rb_to_qs(rb_ary_entry(ary, i));
  qcast<QTableWidget>(self)->setVerticalHeaderLabels(l);
  return self;
}
static VALUE tw_set_row_hidden(VALUE self, VALUE r, VALUE b) {
  qcast<QTableWidget>(self)->setRowHidden(NUM2INT(r), RTEST(b)); return self;
}
static VALUE tw_select_row(VALUE self, VALUE r) {
  qcast<QTableWidget>(self)->selectRow(NUM2INT(r)); return self;
}
static VALUE tw_row_height(VALUE self, VALUE r) {
  return INT2NUM(qcast<QTableWidget>(self)->rowHeight(NUM2INT(r)));
}
static VALUE tw_set_span(VALUE self, VALUE r, VALUE c, VALUE rs, VALUE cs) {
  qcast<QTableWidget>(self)->setSpan(NUM2INT(r), NUM2INT(c), NUM2INT(rs), NUM2INT(cs));
  return self;
}
static VALUE twi_text_color(VALUE self) {
  return wrap_val<QColor>(cColor, get_ptr<QTableWidgetItem>(self)->foreground().color());
}
// QComboBox::removeItem(index) -- a different method from
// QLayout::removeItem, which is why a name-based scan thought this was bound.
// qt.rb:573 clearItems() calls it for every row.
static VALUE cb_remove_item(VALUE self, VALUE i) {
  qcast<QComboBox>(self)->removeItem(NUM2INT(i)); return self;
}
static VALUE cb_insert_item(int argc, VALUE *argv, VALUE self) {
  QComboBox *c = qcast<QComboBox>(self);
  if (argc >= 2) c->insertItem(NUM2INT(argv[0]), rb_to_qs(argv[1]));
  else           c->addItem(rb_to_qs(argv[0]));
  return self;
}
static VALUE cb_max_count(VALUE self) {
  return INT2NUM(qcast<QComboBox>(self)->maxCount());
}
static VALUE cb_max_visible(VALUE self) {
  return INT2NUM(qcast<QComboBox>(self)->maxVisibleItems());
}
static VALUE slider_set_tick_interval(VALUE self, VALUE v) {
  qcast<QSlider>(self)->setTickInterval(NUM2INT(v)); return self;
}
static VALUE slider_set_tick_position(VALUE self, VALUE v) {
  qcast<QSlider>(self)->setTickPosition((QSlider::TickPosition)NUM2INT(v)); return self;
}
static VALUE slider_set_tracking(VALUE self, VALUE v) {
  qcast<QAbstractSlider>(self)->setTracking(RTEST(v)); return self;
}
static VALUE slider_set_position(VALUE self, VALUE v) {
  qcast<QAbstractSlider>(self)->setSliderPosition(NUM2INT(v)); return self;
}
static VALUE slider_position(VALUE self) {
  return INT2NUM(qcast<QAbstractSlider>(self)->sliderPosition());
}
static VALUE tab_set_tab_enabled(VALUE self, VALUE i, VALUE b) {
  qcast<QTabWidget>(self)->setTabEnabled(NUM2INT(i), RTEST(b)); return self;
}

// ---- batch 2: text editing -------------------------------------------
static VALUE tc_selection_start(VALUE self) {
  return INT2NUM(get_val<QTextCursor>(self)->selectionStart());
}
static VALUE tc_selection_end(VALUE self) {
  return INT2NUM(get_val<QTextCursor>(self)->selectionEnd());
}
static VALUE tc_at_block_start(VALUE self) {
  return get_val<QTextCursor>(self)->atBlockStart() ? Qtrue : Qfalse;
}
static VALUE tc_at_block_end(VALUE self) {
  return get_val<QTextCursor>(self)->atBlockEnd() ? Qtrue : Qfalse;
}
static VALUE tc_delete_prev_char(VALUE self) {
  get_val<QTextCursor>(self)->deletePreviousChar(); return self;
}
static VALUE tc_delete_char(VALUE self) {
  get_val<QTextCursor>(self)->deleteChar(); return self;
}
static VALUE tc_select_all(VALUE self) {
  get_val<QTextCursor>(self)->select(QTextCursor::Document); return self;
}
static VALUE tb_set_user_state(VALUE self, VALUE v) {
  get_val<QTextBlock>(self)->setUserState(NUM2INT(v)); return self;
}
static VALUE tb_user_state(VALUE self) {
  return INT2NUM(get_val<QTextBlock>(self)->userState());
}
static VALUE tb_first_line_number(VALUE self) {
  return INT2NUM(get_val<QTextBlock>(self)->firstLineNumber());
}
static VALUE doc_first_block(VALUE self) {
  return wrap_val<QTextBlock>(cTextBlockKlass, qcast<QTextDocument>(self)->firstBlock());
}
static VALUE doc_find_block_by_line(VALUE self, VALUE n) {
  return wrap_val<QTextBlock>(cTextBlockKlass,
                              qcast<QTextDocument>(self)->findBlockByLineNumber(NUM2INT(n)));
}
// moveCursor / cursorRect / selectAll exist on both editor classes.
static VALUE edit_move_cursor(int argc, VALUE *argv, VALUE self) {
  QTextCursor::MoveOperation op = (QTextCursor::MoveOperation)NUM2INT(argv[0]);
  QTextCursor::MoveMode mode = (argc > 1 && !NIL_P(argv[1]))
    ? (QTextCursor::MoveMode)NUM2INT(argv[1]) : QTextCursor::MoveAnchor;
  QObject *o = get_obj(self);
  if (QPlainTextEdit *p = qobject_cast<QPlainTextEdit *>(o)) p->moveCursor(op, mode);
  else if (QTextEdit *t = qobject_cast<QTextEdit *>(o))      t->moveCursor(op, mode);
  else rb_raise(rb_eRuntimeError, "moveCursor: unsupported receiver");
  return self;
}
static VALUE edit_cursor_rect(VALUE self) {
  QObject *o = get_obj(self);
  if (QPlainTextEdit *p = qobject_cast<QPlainTextEdit *>(o))
    return wrap_val<QRect>(cRect, p->cursorRect());
  if (QTextEdit *t = qobject_cast<QTextEdit *>(o))
    return wrap_val<QRect>(cRect, t->cursorRect());
  rb_raise(rb_eRuntimeError, "cursorRect: unsupported receiver");
}
static VALUE edit_select_all(VALUE self) {
  QObject *o = get_obj(self);
  if (QPlainTextEdit *p = qobject_cast<QPlainTextEdit *>(o)) { p->selectAll(); return self; }
  if (QTextEdit *t = qobject_cast<QTextEdit *>(o))           { t->selectAll(); return self; }
  if (QLineEdit *l = qobject_cast<QLineEdit *>(o))           { l->selectAll(); return self; }
  rb_raise(rb_eRuntimeError, "selectAll: unsupported receiver");
}

// ---- batch 3: Qt::Polygon ---------------------------------------------
// rangebar_widget.rb builds triangles with Polygon.new(n) + setPoint(i,x,y)
// and draws them with drawPolygon. Polygon had no constructor at all, and
// drawPolygon was wired to painter_draw_rect -- a stub that drew the bounding
// RECTANGLE instead of the shape.
static VALUE polygon_new(int argc, VALUE *argv, VALUE klass) {
  int n = (argc > 0 && !NIL_P(argv[0])) ? NUM2INT(argv[0]) : 0;
  return wrap_val<QPolygon>(klass, QPolygon(n));
}
static VALUE polygon_set_point(VALUE self, VALUE i, VALUE x, VALUE y) {
  QPolygon *p = get_val<QPolygon>(self);
  int idx = NUM2INT(i);
  if (idx < 0) rb_raise(rb_eIndexError, "setPoint: negative index");
  if (idx >= p->size()) p->resize(idx + 1);
  p->setPoint(idx, NUM2INT(x), NUM2INT(y));
  return self;
}
static VALUE polygon_point(VALUE self, VALUE i) {
  QPolygon *p = get_val<QPolygon>(self);
  int idx = NUM2INT(i);
  if (idx < 0 || idx >= p->size()) return Qnil;
  return wrap_val<QPoint>(cPoint, p->at(idx));
}
static VALUE polygon_size(VALUE self) {
  return INT2NUM(get_val<QPolygon>(self)->size());
}
// ---- batch 4: layouts, item views, combo/tab, misc ---------------------
static VALUE boxlayout_insert_layout(int argc, VALUE *argv, VALUE self) {
  QBoxLayout *b = qcast<QBoxLayout>(self);
  QLayout *l = qobject_cast<QLayout *>(get_obj(argv[1]));
  if (!l) rb_raise(rb_eArgError, "insertLayout expects a layout");
  int stretch = (argc > 2 && !NIL_P(argv[2])) ? NUM2INT(argv[2]) : 0;
  b->insertLayout(NUM2INT(argv[0]), l, stretch);
  release_ownership(argv[1]);
  return self;
}
static VALUE layout_remove_item(VALUE self, VALUE item) {
  QLayout *lay = qcast<QLayout>(self);
  QObject *o = get_obj(item);
  if (QLayout *inner = qobject_cast<QLayout *>(o)) lay->removeItem(inner);
  else if (QWidget *w = qobject_cast<QWidget *>(o)) lay->removeWidget(w);
  else rb_raise(rb_eArgError, "removeItem expects a layout or widget");
  return self;
}
static VALUE layout_set_margin(VALUE self, VALUE m) {
  int v = NUM2INT(m);
  qcast<QLayout>(self)->setContentsMargins(v, v, v, v);   // Qt4 setMargin
  return self;
}
static VALUE lw_item(VALUE self, VALUE i) {
  QListWidgetItem *it = qcast<QListWidget>(self)->item(NUM2INT(i));
  return it ? wrap_ptr<QListWidgetItem>(cListWidgetItem, it) : Qnil;
}
static VALUE lw_find_items(int argc, VALUE *argv, VALUE self) {
  Qt::MatchFlags f = (argc > 1 && !NIL_P(argv[1]))
    ? (Qt::MatchFlags)QFlags<Qt::MatchFlag>((Qt::MatchFlag)NUM2INT(argv[1]))
    : Qt::MatchFlags(Qt::MatchExactly);
  QList<QListWidgetItem *> found = qcast<QListWidget>(self)->findItems(rb_to_qs(argv[0]), f);
  VALUE ary = rb_ary_new();
  for (int i = 0; i < found.size(); i++)
    rb_ary_push(ary, wrap_ptr<QListWidgetItem>(cListWidgetItem, found.at(i)));
  return ary;
}
static VALUE lw_current_item(VALUE self) {
  QListWidgetItem *it = qcast<QListWidget>(self)->currentItem();
  return it ? wrap_ptr<QListWidgetItem>(cListWidgetItem, it) : Qnil;
}
static VALUE lw_selected_items(VALUE self) {
  QList<QListWidgetItem *> sel = qcast<QListWidget>(self)->selectedItems();
  VALUE ary = rb_ary_new();
  for (int i = 0; i < sel.size(); i++)
    rb_ary_push(ary, wrap_ptr<QListWidgetItem>(cListWidgetItem, sel.at(i)));
  return ary;
}
static VALUE lwi_set_selected(VALUE self, VALUE b) {
  get_ptr<QListWidgetItem>(self)->setSelected(RTEST(b)); return self;
}
static VALUE lwi_is_selected(VALUE self) {
  return get_ptr<QListWidgetItem>(self)->isSelected() ? Qtrue : Qfalse;
}
static VALUE tree_set_header_labels(VALUE self, VALUE ary) {
  QStringList l;
  for (long i = 0; i < RARRAY_LEN(ary); i++) l << rb_to_qs(rb_ary_entry(ary, i));
  qcast<QTreeWidget>(self)->setHeaderLabels(l);
  return self;
}
static VALUE tree_set_item_widget(VALUE self, VALUE item, VALUE col, VALUE w) {
  QWidget *widget = qobject_cast<QWidget *>(get_obj(w));
  qcast<QTreeWidget>(self)->setItemWidget(get_ptr<QTreeWidgetItem>(item), NUM2INT(col), widget);
  release_ownership(w);
  return self;
}
static VALUE tree_resize_col(VALUE self, VALUE c) {
  qcast<QTreeWidget>(self)->resizeColumnToContents(NUM2INT(c)); return self;
}
static VALUE twi_set_expanded(VALUE self, VALUE b) {
  get_ptr<QTreeWidgetItem>(self)->setExpanded(RTEST(b)); return self;
}
static VALUE cb_item_data(VALUE self, VALUE i) {
  QVariant v = qcast<QComboBox>(self)->itemData(NUM2INT(i));
  return v.isValid() ? wrap_val<QVariant>(cVariant, v) : Qnil;
}
static VALUE cb_set_item_data(VALUE self, VALUE i, VALUE v) {
  qcast<QComboBox>(self)->setItemData(NUM2INT(i), *get_val<QVariant>(v)); return self;
}
static VALUE tab_rect(VALUE self, VALUE i) {
  return wrap_val<QRect>(cRect, qcast<QTabWidget>(self)->tabBar()->tabRect(NUM2INT(i)));
}
static VALUE tab_set_tab_icon(VALUE self, VALUE i, VALUE ic) {
  qcast<QTabWidget>(self)->setTabIcon(NUM2INT(i), icon_arg(ic)); return self;
}
static VALUE tab_current_widget(VALUE self) {
  QWidget *w = qcast<QTabWidget>(self)->currentWidget();
  return w ? wrap_obj(best_ruby_class(w, cWidget), w, false) : Qnil;
}
static VALUE sb_clear_message(VALUE self) {
  qcast<QStatusBar>(self)->clearMessage(); return self;
}
static VALUE scroll_ensure_visible(int argc, VALUE *argv, VALUE self) {
  QWidget *w = qobject_cast<QWidget *>(get_obj(argv[0]));
  int mx = (argc > 1 && !NIL_P(argv[1])) ? NUM2INT(argv[1]) : 50;
  int my = (argc > 2 && !NIL_P(argv[2])) ? NUM2INT(argv[2]) : 50;
  qcast<QScrollArea>(self)->ensureWidgetVisible(w, mx, my);
  return self;
}
static VALUE label_word_wrap(VALUE self) {
  return qcast<QLabel>(self)->wordWrap() ? Qtrue : Qfalse;
}

// ---- batch 5: final sweep ----------------------------------------------
static VALUE fm_bounding_rect(VALUE self, VALUE text) {
  return wrap_val<QRect>(cRect, get_val<QFontMetrics>(self)->boundingRect(rb_to_qs(text)));
}
static VALUE widget_insert_action(VALUE self, VALUE before, VALUE act) {
  QWidget *w = qcast<QWidget>(self);
  w->insertAction(qobject_cast<QAction *>(get_obj(before)),
                  qobject_cast<QAction *>(get_obj(act)));
  release_ownership(act);
  return self;
}
static VALUE view_size_hint_for_column(VALUE self, VALUE c) {
  return INT2NUM(qcast<QAbstractItemView>(self)->sizeHintForColumn(NUM2INT(c)));
}
static VALUE icon_add_pixmap(int argc, VALUE *argv, VALUE self) {
  QIcon *ic = get_val<QIcon>(self);
  QPixmap *pm = get_val<QPixmap>(argv[0]);
  QIcon::Mode mode = (argc > 1 && !NIL_P(argv[1])) ? (QIcon::Mode)NUM2INT(argv[1]) : QIcon::Normal;
  ic->addPixmap(*pm, mode);
  return self;
}
static VALUE toolbar_set_floatable(VALUE self, VALUE b) {
  qcast<QToolBar>(self)->setFloatable(RTEST(b)); return self;
}
static VALUE mw_add_toolbar(int argc, VALUE *argv, VALUE self) {
  QMainWindow *m = qcast<QMainWindow>(self);
  // addToolBar(area, bar) or addToolBar(bar)
  int i = (argc > 1) ? 1 : 0;
  QToolBar *tb = qobject_cast<QToolBar *>(get_obj(argv[i]));
  if (!tb) rb_raise(rb_eArgError, "addToolBar expects a Qt::ToolBar");
  if (argc > 1) m->addToolBar((Qt::ToolBarArea)NUM2INT(argv[0]), tb);
  else          m->addToolBar(tb);
  release_ownership(argv[i]);
  return self;
}
static VALUE cal_selected_date(VALUE self) {
  return wrap_val<QDate>(cDate, qcast<QCalendarWidget>(self)->selectedDate());
}
static VALUE cal_set_selected_date(VALUE self, VALUE d) {
  qcast<QCalendarWidget>(self)->setSelectedDate(*get_val<QDate>(d)); return self;
}
static VALUE cal_set_v_header_format(VALUE self, VALUE f) {
  qcast<QCalendarWidget>(self)
    ->setVerticalHeaderFormat((QCalendarWidget::VerticalHeaderFormat)NUM2INT(f));
  return self;
}
static VALUE variant_to_string_list(VALUE self) {
  QStringList l = get_val<QVariant>(self)->toStringList();
  VALUE ary = rb_ary_new();
  for (int i = 0; i < l.size(); i++)
    rb_ary_push(ary, rb_str_new2(l.at(i).toUtf8().constData()));
  return ary;
}
static VALUE url_to_local_file(VALUE self) {
  return rb_str_new2(get_val<QUrl>(self)->toLocalFile().toUtf8().constData());
}

// Bound so a Ruby override's `super` reaches Qt's default implementation.
// 25 COSMOS event handlers call super; without these it is a NoMethodError
// ("super: no superclass method `closeEvent'"). Takes any args and ignores
// them -- the event pointer is captured by the dispatching virtual.
static VALUE qt_base_event(int, VALUE *, VALUE) {
  ruby_call_base_event();
  return Qnil;
}

// ---- found by the UI exerciser sweep -----------------------------------
// Qt::Date was declared but never defined as a class, so
// Qt::Date.new(y, m, d) hit Object#initialize (calendar_dialog.rb:44).
static VALUE date_new(int argc, VALUE *argv, VALUE klass) {
  if (argc >= 3)
    return wrap_val<QDate>(klass, QDate(NUM2INT(argv[0]), NUM2INT(argv[1]), NUM2INT(argv[2])));
  return wrap_val<QDate>(klass, QDate::currentDate());
}
static VALUE date_year(VALUE self)  { return INT2NUM(get_val<QDate>(self)->year()); }
static VALUE date_month(VALUE self) { return INT2NUM(get_val<QDate>(self)->month()); }
static VALUE date_day(VALUE self)   { return INT2NUM(get_val<QDate>(self)->day()); }
static VALUE date_valid(VALUE self) { return get_val<QDate>(self)->isValid() ? Qtrue : Qfalse; }

// QCheckBox::setCheckState / checkState (limits_monitor.rb:683)
static VALUE cbx_set_check_state(VALUE self, VALUE v) {
  qcast<QCheckBox>(self)->setCheckState((Qt::CheckState)NUM2INT(v)); return self;
}
static VALUE cbx_check_state(VALUE self) {
  return INT2NUM((int)qcast<QCheckBox>(self)->checkState());
}
// QListWidgetItem::text / setText (tlm_extractor.rb:1075)
static VALUE lwi_text(VALUE self) {
  return rb_str_new2(get_ptr<QListWidgetItem>(self)->text().toUtf8().constData());
}
static VALUE lwi_set_text(VALUE self, VALUE t) {
  get_ptr<QListWidgetItem>(self)->setText(rb_to_qs(t)); return self;
}
// QVariant::value -- qtbindings' generic accessor; COSMOS reads screen
// settings back with it (10 call paths in TlmViewer).
static VALUE variant_value(VALUE self) {
  QVariant *v = get_val<QVariant>(self);
  switch (v->typeId()) {
    case QMetaType::Bool:        return v->toBool() ? Qtrue : Qfalse;
    case QMetaType::Int:         return INT2NUM(v->toInt());
    case QMetaType::UInt:        return UINT2NUM(v->toUInt());
    case QMetaType::LongLong:    return LL2NUM(v->toLongLong());
    case QMetaType::Double:
    case QMetaType::Float:       return rb_float_new(v->toDouble());
    case QMetaType::QPoint:      return wrap_val<QPoint>(cPoint, v->toPoint());
    case QMetaType::QSize:       return wrap_val<QSize>(cSize, v->toSize());
    case QMetaType::QStringList: {
      QStringList l = v->toStringList();
      VALUE ary = rb_ary_new();
      for (int i = 0; i < l.size(); i++)
        rb_ary_push(ary, rb_str_new2(l.at(i).toUtf8().constData()));
      return ary;
    }
    default: break;
  }
  if (!v->isValid()) return Qnil;
  return rb_str_new2(v->toString().toUtf8().constData());
}

// ---- batch 6: found sweeping ScriptRunner / CmdSequence ----------------
static VALUE widget_clear_focus(VALUE self) {
  qcast<QWidget>(self)->clearFocus(); return self;
}
static VALUE widget_scroll(VALUE self, VALUE dx, VALUE dy) {
  qcast<QWidget>(self)->scroll(NUM2INT(dx), NUM2INT(dy)); return self;
}
// QWidget::actions() -- script_runner.rb:451 walks a menu's actions to
// rebuild the recent-files list.
static VALUE widget_actions(VALUE self) {
  QList<QAction *> acts = qcast<QWidget>(self)->actions();
  VALUE ary = rb_ary_new();
  for (int i = 0; i < acts.size(); i++)
    rb_ary_push(ary, wrap_obj(cAction, acts.at(i), false));
  return ary;
}
// cursor.selection.toPlainText (qt.rb:530). QTextDocumentFragment has no
// binding of its own; return a tiny object exposing toPlainText.
static VALUE tc_selection_text(VALUE self) {
  return rb_str_new2(get_val<QTextCursor>(self)->selectedText().toUtf8().constData());
}
static VALUE edit_paste(VALUE self) {
  QObject *o = get_obj(self);
  if (QPlainTextEdit *p = qobject_cast<QPlainTextEdit *>(o)) { p->paste(); return self; }
  if (QTextEdit *t = qobject_cast<QTextEdit *>(o))           { t->paste(); return self; }
  if (QLineEdit *l = qobject_cast<QLineEdit *>(o))           { l->paste(); return self; }
  rb_raise(rb_eRuntimeError, "paste: unsupported receiver");
}
static VALUE edit_copy(VALUE self) {
  QObject *o = get_obj(self);
  if (QPlainTextEdit *p = qobject_cast<QPlainTextEdit *>(o)) { p->copy(); return self; }
  if (QTextEdit *t = qobject_cast<QTextEdit *>(o))           { t->copy(); return self; }
  if (QLineEdit *l = qobject_cast<QLineEdit *>(o))           { l->copy(); return self; }
  rb_raise(rb_eRuntimeError, "copy: unsupported receiver");
}
static VALUE edit_cut(VALUE self) {
  QObject *o = get_obj(self);
  if (QPlainTextEdit *p = qobject_cast<QPlainTextEdit *>(o)) { p->cut(); return self; }
  if (QTextEdit *t = qobject_cast<QTextEdit *>(o))           { t->cut(); return self; }
  if (QLineEdit *l = qobject_cast<QLineEdit *>(o))           { l->cut(); return self; }
  rb_raise(rb_eRuntimeError, "cut: unsupported receiver");
}

static VALUE widget_close(VALUE self) {
  QWidget *w = qobject_cast<QWidget *>(get_obj(self));
  return (w && w->close()) ? Qtrue : Qfalse;
}

static VALUE widget_set_window_icon(VALUE self, VALUE i) {
  QObject *o = get_obj(self);
  if (QWidget *w = qobject_cast<QWidget *>(o)) w->setWindowIcon(icon_arg(i));
  else QApplication::setWindowIcon(icon_arg(i));
  return self;
}
static VALUE app_set_window_icon(VALUE klass, VALUE i) {
  QApplication::setWindowIcon(icon_arg(i)); return Qnil;
}
static VALUE app_add_library_path(VALUE self, VALUE p2) {
  QCoreApplication::addLibraryPath(rb_to_qs(p2)); return self;
}
static VALUE app_close_all_windows(VALUE self) {
  QApplication::closeAllWindows(); return self;
}
static VALUE button_set_icon(VALUE self, VALUE i) {
  QObject *o = get_obj(self);
  if (QAbstractButton *b = qobject_cast<QAbstractButton *>(o)) b->setIcon(icon_arg(i));
  else if (QAction *a = qobject_cast<QAction *>(o))            a->setIcon(icon_arg(i));
  else rb_raise(rb_eRuntimeError, "setIcon: unsupported receiver");
  return self;
}
static VALUE widget_set_icon_size(VALUE self, VALUE sz) {
  QSize q = rb_obj_is_kind_of(sz, cSize) ? *get_val<QSize>(sz) : QSize(NUM2INT(sz), NUM2INT(sz));
  QObject *o = get_obj(self);
  if (QAbstractButton *b = qobject_cast<QAbstractButton *>(o)) b->setIconSize(q);
  else if (QToolBar *t = qobject_cast<QToolBar *>(o))          t->setIconSize(q);
  return self;
}
static VALUE frame_frame_width(VALUE self) {
  return INT2NUM(qcast<QFrame>(self)->frameWidth());
}
static VALUE header_length(VALUE self) {
  return INT2NUM(qcast<QHeaderView>(self)->length());
}
static VALUE widget_set_maximum_size(int argc, VALUE *argv, VALUE self) {
  QWidget *w = qcast<QWidget>(self);
  if (argc == 1) w->setMaximumSize(*get_val<QSize>(argv[0]));
  else           w->setMaximumSize(NUM2INT(argv[0]), NUM2INT(argv[1]));
  return self;
}
static VALUE frame_set_line_width(VALUE self, VALUE n) {
  qcast<QFrame>(self)->setLineWidth(NUM2INT(n)); return self;
}
static VALUE frame_set_mid_line_width(VALUE self, VALUE n) {
  qcast<QFrame>(self)->setMidLineWidth(NUM2INT(n)); return self;
}
static VALUE widget_set_minimum_size(int argc, VALUE *argv, VALUE self) {
  QWidget *w = qcast<QWidget>(self);
  if (argc == 1) w->setMinimumSize(*get_val<QSize>(argv[0]));
  else           w->setMinimumSize(NUM2INT(argv[0]), NUM2INT(argv[1]));
  return self;
}
static VALUE label_set_pixmap(VALUE self, VALUE p2) {
  qcast<QLabel>(self)->setPixmap(*get_val<QPixmap>(p2)); return self;
}
static VALUE label_set_text_format(VALUE self, VALUE f) {
  qcast<QLabel>(self)->setTextFormat((Qt::TextFormat)NUM2INT(f)); return self;
}
static VALUE textedit_set_wrap_mode(VALUE self, VALUE m) {
  qcast<QTextEdit>(self)
    ->setLineWrapMode((QTextEdit::LineWrapMode)NUM2INT(m));
  return self;
}
static VALUE cTextDocumentKlass = Qnil;
static VALUE te_document(VALUE self) {
  QObject *o = get_obj(self);
  QTextDocument *d = qobject_cast<QPlainTextEdit *>(o)
      ? qobject_cast<QPlainTextEdit *>(o)->document()
      : qobject_cast<QTextEdit *>(o)->document();
  return wrap_obj(cTextDocumentKlass, d, false);
}
static VALUE doc_default_font(VALUE self) {
  return wrap_val<QFont>(cFont, qcast<QTextDocument>(self)->defaultFont());
}
static VALUE doc_set_default_font(VALUE self, VALUE f) {
  qcast<QTextDocument>(self)->setDefaultFont(*get_val<QFont>(f));
  return self;
}
static VALUE completer_model(VALUE self) {
  QAbstractItemModel *m = qcast<QCompleter>(self)->model();
  return m ? wrap_obj(cAbstractItemModel, m, false) : Qnil;
}
static VALUE view_model(VALUE self) {
  QAbstractItemModel *m = qcast<QAbstractItemView>(self)->model();
  return m ? wrap_obj(cAbstractItemModel, m, false) : Qnil;
}
static VALUE view_is_column_hidden(VALUE self, VALUE c) {
  return qcast<QTreeView>(self)->isColumnHidden(NUM2INT(c)) ? Qtrue : Qfalse;
}
static VALUE header_section_resize_mode(VALUE self, VALUE i) {
  return INT2NUM((int)qcast<QHeaderView>(self)->sectionResizeMode(NUM2INT(i)));
}

static VALUE tab_widget_at(VALUE self, VALUE i) {
  QWidget *w = qcast<QTabWidget>(self)->widget(NUM2INT(i));
  return w ? wrap_obj(cWidget, w, false) : Qnil;
}
static VALUE doc_block_count(VALUE self) {
  return INT2NUM(qcast<QTextDocument>(self)->blockCount());
}
static VALUE doc_is_modified(VALUE self) {
  return qcast<QTextDocument>(self)->isModified() ? Qtrue : Qfalse;
}
static VALUE doc_set_modified(int argc, VALUE *argv, VALUE self) {
  VALUE b; rb_scan_args(argc, argv, "01", &b);
  qcast<QTextDocument>(self)->setModified(NIL_P(b) ? true : RTEST(b));
  return self;
}
static VALUE icon_pixmap(int argc, VALUE *argv, VALUE self) {
  VALUE w, h; rb_scan_args(argc, argv, "11", &w, &h);
  int ww = NUM2INT(w), hh = NIL_P(h) ? ww : NUM2INT(h);
  return wrap_val<QPixmap>(cPixmap, get_val<QIcon>(self)->pixmap(QSize(ww, hh)));
}
static VALUE pte_set_wrap_mode(VALUE self, VALUE m) {
  qcast<QPlainTextEdit>(self)
    ->setLineWrapMode((QPlainTextEdit::LineWrapMode)NUM2INT(m));
  return self;
}
static VALUE style_standard_icon(VALUE self, VALUE sp) {
  QStyle *st = qcast<QStyle>(self);
  return wrap_val<QIcon>(cIcon, st->standardIcon((QStyle::StandardPixmap)NUM2INT(sp)));
}
static VALUE textedit_set_text_color(VALUE self, VALUE c) {
  qcast<QTextEdit>(self)->setTextColor(color_arg(c)); return self;
}
static VALUE dbb_add_button(VALUE self, VALUE b) {
  QDialogButtonBox *box = qcast<QDialogButtonBox>(self);
  if (RB_TYPE_P(b, T_FIXNUM))
    box->addButton((QDialogButtonBox::StandardButton)NUM2INT(b));
  else {
    box->addButton(qobject_cast<QAbstractButton *>(get_obj(b)), QDialogButtonBox::ActionRole);
    release_ownership(b);
  }
  return self;
}

// ---- widget accessors ------------------------------------------------------
static VALUE widget_palette(VALUE self) {
  return wrap_val<QPalette>(cPalette, qcast<QWidget>(self)->palette());
}
static VALUE widget_font(VALUE self) {
  return wrap_val<QFont>(cFont, qcast<QWidget>(self)->font());
}
static VALUE widget_font_metrics(VALUE self) {
  return wrap_val<QFontMetrics>(cFontMetrics,
    QFontMetrics(qcast<QWidget>(self)->font()));
}
static VALUE widget_set_autofill(VALUE self, VALUE b) {
  qcast<QWidget>(self)->setAutoFillBackground(RTEST(b)); return self;
}
static VALUE widget_set_margins(VALUE self, VALUE l, VALUE t, VALUE r, VALUE b) {
  qcast<QWidget>(self)
    ->setContentsMargins(NUM2INT(l), NUM2INT(t), NUM2INT(r), NUM2INT(b));
  return self;
}
static VALUE widget_set_mouse_tracking(VALUE self, VALUE b) {
  qcast<QWidget>(self)->setMouseTracking(RTEST(b)); return self;
}
static VALUE widget_set_focus_policy(VALUE self, VALUE v) {
  qcast<QWidget>(self)->setFocusPolicy((Qt::FocusPolicy)NUM2INT(v));
  return self;
}
static VALUE widget_size_hint(VALUE self) {
  return wrap_val<QSize>(cSize, qcast<QWidget>(self)->sizeHint());
}
static VALUE widget_min_size_hint(VALUE self) {
  return wrap_val<QSize>(cSize, qcast<QWidget>(self)->minimumSizeHint());
}
static VALUE widget_frame_geometry(VALUE self) {
  return wrap_val<QRect>(cRect, qcast<QWidget>(self)->frameGeometry());
}
static VALUE combo_insert_item(int argc, VALUE *argv, VALUE self) {
  VALUE idx, text, data; rb_scan_args(argc, argv, "21", &idx, &text, &data);
  QComboBox *c = qcast<QComboBox>(self);
  if (NIL_P(data)) c->insertItem(NUM2INT(idx), rb_to_qs(text));
  else {
    QVariant v;
    if (RB_TYPE_P(data, T_FIXNUM))      v = QVariant(NUM2INT(data));
    else if (RB_TYPE_P(data, T_STRING)) v = QVariant(rb_to_qs(data));
    else if (rb_respond_to(data, rb_intern("toString")))
      v = QVariant(rb_to_qs(rb_funcall(data, rb_intern("toString"), 0)));
    c->insertItem(NUM2INT(idx), rb_to_qs(text), v);
  }
  return self;
}
static VALUE widget_geometry(VALUE self) {
  return wrap_val<QRect>(cRect, qcast<QWidget>(self)->geometry());
}
static VALUE widget_layout(VALUE self) {
  QLayout *l = qcast<QWidget>(self)->layout();
  // wrap_obj consults the identity map, so a layout created from Ruby comes
  // back as the original Ruby object with its original class.
  return l ? wrap_obj(cLayout, l, false) : Qnil;
}
static VALUE widget_parent_widget(VALUE self) {
  QWidget *p2 = qcast<QWidget>(self)->parentWidget();
  return p2 ? wrap_obj(cWidget, p2, false) : Qnil;
}
static VALUE obj_parent(VALUE self) {
  QObject *p2 = get_obj(self)->parent();
  return p2 ? wrap_obj(cQtObject, p2, false) : Qnil;
}

// ---- Qt::DesktopWidget compatibility ---------------------------------------
//
// QApplication::desktop() and QDesktopWidget were REMOVED in Qt6. COSMOS uses
// desktop.screen.width/height to clamp restored window geometry, so this shim
// answers those queries from QScreen instead.
static VALUE cDesktopWidget;

static QRect primary_geometry() {
  QScreen *sc = QGuiApplication::primaryScreen();
  return sc ? sc->geometry() : QRect(0, 0, 1024, 768);
}
static QRect primary_available() {
  QScreen *sc = QGuiApplication::primaryScreen();
  return sc ? sc->availableGeometry() : QRect(0, 0, 1024, 768);
}
static VALUE desktop_self(VALUE self)   { return self; }
static VALUE desktop_width(VALUE self)  { return INT2NUM(primary_geometry().width()); }
static VALUE desktop_height(VALUE self) { return INT2NUM(primary_geometry().height()); }
static VALUE desktop_available(int argc, VALUE *argv, VALUE self) {
  return wrap_val<QRect>(cRect, primary_available());
}
static VALUE desktop_screen_geometry(int argc, VALUE *argv, VALUE self) {
  return wrap_val<QRect>(cRect, primary_geometry());
}
static VALUE app_desktop(VALUE klass) {
  return rb_funcall(cDesktopWidget, rb_intern("new"), 0);
}

// ---- QVariant conversions COSMOS relies on ---------------------------------
static VALUE variant_to_size(VALUE self) {
  return wrap_val<QSize>(cSize, get_val<QVariant>(self)->toSize());
}
static VALUE variant_to_point(VALUE self) {
  return wrap_val<QPoint>(cPoint, get_val<QVariant>(self)->toPoint());
}
static VALUE settings_contains(VALUE self, VALUE k) {
  return qcast<QSettings>(self)->contains(rb_to_qs(k)) ? Qtrue : Qfalse;
}
static VALUE settings_set_value_variant(VALUE self, VALUE k, VALUE v) {
  QSettings *st = qcast<QSettings>(self);
  if (rb_obj_is_kind_of(v, cSize))       st->setValue(rb_to_qs(k), *get_val<QSize>(v));
  else if (rb_obj_is_kind_of(v, cPoint)) st->setValue(rb_to_qs(k), *get_val<QPoint>(v));
  else if (rb_obj_is_kind_of(v, cVariant)) st->setValue(rb_to_qs(k), *get_val<QVariant>(v));
  else if (RB_TYPE_P(v, T_FIXNUM))       st->setValue(rb_to_qs(k), NUM2INT(v));
  else                                    st->setValue(rb_to_qs(k), rb_to_qs(v));
  return self;
}

// ---- resize/move accepting value types -------------------------------------
static VALUE widget_resize_v(int argc, VALUE *argv, VALUE self) {
  QWidget *w = qcast<QWidget>(self);
  if (argc == 1) w->resize(*get_val<QSize>(argv[0]));
  else           w->resize(NUM2INT(argv[0]), NUM2INT(argv[1]));
  return self;
}
static VALUE widget_move(int argc, VALUE *argv, VALUE self) {
  QWidget *w = qcast<QWidget>(self);
  if (argc == 1) w->move(*get_val<QPoint>(argv[0]));
  else           w->move(NUM2INT(argv[0]), NUM2INT(argv[1]));
  return self;
}

// ---------------------------------------------------------------------------
// Qt::Painter
//
// QPainter is a fourth wrapper kind: not a QObject, and not copyable, so it
// can use neither the QObject wrapper nor the value wrapper. It owns a
// heap QPainter and ends any active paint session before deleting it.
//
// COSMOS reopens Qt::Painter (lib/cosmos/gui/qt.rb:707) and calls super from
// its own setPen/setBrush, so these must be real Ruby methods on the class.
// ---------------------------------------------------------------------------
struct PainterWrap { QPainter *p; bool owned; };

static void painter_free(void *v) {
  PainterWrap *w = (PainterWrap *)v;
  if (w->p && w->owned) {          // borrowed painters (delegate paint) are Qt's
    if (w->p->isActive()) w->p->end();
    delete w->p;
  }
  xfree(w);
}
static size_t painter_size(const void *) { return sizeof(PainterWrap); }
static const rb_data_type_t painter_type = {
  "Qt::Painter",
  { NULL, painter_free, painter_size, { NULL, NULL } },
  NULL, NULL, RUBY_TYPED_FREE_IMMEDIATELY
};

static PainterWrap *painter_wrap(VALUE self) {
  PainterWrap *w;
  TypedData_Get_Struct(self, PainterWrap, &painter_type, w);
  return w;
}
static QPainter *painter_of(VALUE self) {
  PainterWrap *w = painter_wrap(self);
  if (!w->p) rb_raise(rb_eRuntimeError, "Qt::Painter is not initialized");
  return w->p;
}
static VALUE painter_alloc(VALUE klass) {
  PainterWrap *w;
  VALUE o = TypedData_Make_Struct(klass, PainterWrap, &painter_type, w);
  w->p = NULL;
  w->owned = true;
  return o;
}
static QPaintDevice *paint_device(VALUE v) {
  if (rb_obj_is_kind_of(v, cPixmap)) return get_val<QPixmap>(v);
  if (rb_obj_is_kind_of(v, cImage))  return get_val<QImage>(v);
  QWidget *w = qobject_cast<QWidget *>(get_obj(v));
  if (!w) rb_raise(rb_eArgError, "Qt::Painter: unsupported paint device");
  return w;
}

static VALUE painter_initialize(int argc, VALUE *argv, VALUE self) {
  PainterWrap *w = painter_wrap(self);
  if (w->p) return self;
  VALUE dev; rb_scan_args(argc, argv, "01", &dev);
  w->p = NIL_P(dev) ? new QPainter() : new QPainter(paint_device(dev));
  w->owned = true;
  return self;
}
static VALUE painter_begin(VALUE self, VALUE dev) {
  return painter_of(self)->begin(paint_device(dev)) ? Qtrue : Qfalse;
}
static VALUE painter_end(VALUE self) {
  QPainter *p2 = painter_of(self);
  return (p2->isActive() && p2->end()) ? Qtrue : Qfalse;
}
static VALUE painter_is_active(VALUE self) { return painter_of(self)->isActive() ? Qtrue : Qfalse; }
static VALUE painter_dispose(VALUE self) {
  PainterWrap *w = painter_wrap(self);
  if (w->p && w->owned) { if (w->p->isActive()) w->p->end(); delete w->p; }
  w->p = NULL;
  return Qnil;
}
static VALUE painter_save(VALUE self)    { painter_of(self)->save();    return self; }
static VALUE painter_restore(VALUE self) { painter_of(self)->restore(); return self; }

// COSMOS passes a Qt::Color (from Cosmos.getColor) or a Qt::Pen.
static VALUE painter_set_pen(VALUE self, VALUE v) {
  if (rb_obj_is_kind_of(v, cPen)) painter_of(self)->setPen(*get_val<QPen>(v));
  else                            painter_of(self)->setPen(color_arg(v));
  return self;
}
// nil means "no brush" -- COSMOS calls setBrush(nil) to clear a fill.
static VALUE painter_set_brush(VALUE self, VALUE v) {
  if (NIL_P(v))                           painter_of(self)->setBrush(Qt::NoBrush);
  else if (rb_obj_is_kind_of(v, cBrush))  painter_of(self)->setBrush(*get_val<QBrush>(v));
  else                                    painter_of(self)->setBrush(QBrush(color_arg(v)));
  return self;
}
static VALUE painter_set_font(VALUE self, VALUE f) {
  painter_of(self)->setFont(*get_val<QFont>(f)); return self;
}
static VALUE painter_set_background(VALUE self, VALUE b) {
  if (rb_obj_is_kind_of(b, cBrush)) painter_of(self)->setBackground(*get_val<QBrush>(b));
  else                              painter_of(self)->setBackground(QBrush(color_arg(b)));
  return self;
}
static VALUE painter_set_background_mode(VALUE self, VALUE m) {
  painter_of(self)->setBackgroundMode((Qt::BGMode)NUM2INT(m)); return self;
}
static VALUE painter_set_render_hint(int argc, VALUE *argv, VALUE self) {
  VALUE h, on; rb_scan_args(argc, argv, "11", &h, &on);
  painter_of(self)->setRenderHint((QPainter::RenderHint)NUM2INT(h),
                                  NIL_P(on) ? true : RTEST(on));
  return self;
}

static VALUE painter_draw_line(VALUE self, VALUE x1, VALUE y1, VALUE x2, VALUE y2) {
  painter_of(self)->drawLine(NUM2INT(x1), NUM2INT(y1), NUM2INT(x2), NUM2INT(y2));
  return self;
}
static VALUE painter_draw_polygon(VALUE self, VALUE poly) {
  painter_of(self)->drawPolygon(*get_val<QPolygon>(poly));
  return self;
}
static VALUE painter_draw_rect(VALUE self, VALUE x, VALUE y, VALUE w, VALUE h) {
  painter_of(self)->drawRect(NUM2INT(x), NUM2INT(y), NUM2INT(w), NUM2INT(h));
  return self;
}
static VALUE painter_draw_ellipse(VALUE self, VALUE x, VALUE y, VALUE w, VALUE h) {
  painter_of(self)->drawEllipse(NUM2INT(x), NUM2INT(y), NUM2INT(w), NUM2INT(h));
  return self;
}
static VALUE painter_draw_text(int argc, VALUE *argv, VALUE self) {
  VALUE a, b, c; rb_scan_args(argc, argv, "21", &a, &b, &c);
  if (NIL_P(c)) {   // drawText(rect, text)
    painter_of(self)->drawText(*get_val<QRect>(a), rb_to_qs(b));
  } else {          // drawText(x, y, text)
    painter_of(self)->drawText(NUM2INT(a), NUM2INT(b), rb_to_qs(c));
  }
  return self;
}
static VALUE painter_fill_rect(int argc, VALUE *argv, VALUE self) {
  VALUE x, y, w, h, col; rb_scan_args(argc, argv, "23", &x, &y, &w, &h, &col);
  if (NIL_P(w)) {   // fillRect(rect, color)
    painter_of(self)->fillRect(*get_val<QRect>(x), color_arg(y));
  } else {
    painter_of(self)->fillRect(NUM2INT(x), NUM2INT(y), NUM2INT(w), NUM2INT(h),
                               NIL_P(col) ? QColor(Qt::black) : color_arg(col));
  }
  return self;
}
static VALUE painter_draw_image(VALUE self, VALUE x, VALUE y, VALUE img) {
  painter_of(self)->drawImage(NUM2INT(x), NUM2INT(y), *get_val<QImage>(img));
  return self;
}

// ---------------------------------------------------------------------------
// Virtual dispatch: Qt C++ -> Ruby override
//
// COSMOS defines paintEvent/resizeEvent/mouse*Event in Ruby subclasses of
// Qt::Widget (lib/cosmos/gui/line_graph/line_graph.rb:349). RubyWidget's
// virtuals call in here so those overrides actually run.
// ---------------------------------------------------------------------------
static VALUE cKeyEventCls = Qnil, cMouseEventCls = Qnil;

struct EvCall { VALUE obj; ID mid; int argc; VALUE argv[4]; };

static VALUE ev_invoke(VALUE p) {
  EvCall *c = (EvCall *)p;
  return rb_funcallv(c->obj, c->mid, c->argc, c->argv);
}
// The GVL is already held: callers wrap the whole callback, argument
// construction included, in ruby_with_gvl.
// Returns false when the Ruby override raised: the exception is reported and
// contained (never longjmp through Qt's C++ frames), and the caller falls back
// to Qt's default implementation. Claiming success here is what turned a
// broken Ruby paintEvent into a silently blank widget.
static bool ev_invoke_protected(EvCall *c, const char *context) {
  int state = 0;
  rb_protect(ev_invoke, (VALUE)c, &state);
  return !ruby_contain_error(state, context);
}

// PRECONDITION: the GVL is held. argv was built under it too -- see
// ruby_with_gvl in rubycallback.h for why that matters.
bool ruby_event_dispatch_n(QObject *obj, const char *method, int argc, VALUE *argv) {
  VALUE self;
  {
    ObjMapLock lk(g_objmap_mutex);
    std::map<QObject *, ObjRef>::iterator it = g_objmap.find(obj);
    if (it == g_objmap.end()) return false;
    self = it->second.v;
  }
  ID mid = rb_intern(method);
  if (!rb_respond_to(self, mid)) return false;   // no Ruby override
  EvCall c;
  c.obj = self; c.mid = mid; c.argc = argc;
  for (int i = 0; i < argc && i < 4; i++) c.argv[i] = argv[i];
  return ev_invoke_protected(&c, method);
}

bool ruby_event_dispatch(QObject *obj, const char *method, VALUE arg) {
  return ruby_event_dispatch_n(obj, method, 1, &arg);
}

// ---- event objects --------------------------------------------------------
// A QEvent lives only for the duration of its dispatch, so Ruby is handed a
// snapshot object rather than a pointer: a handler that stashed the event
// would otherwise be left holding freed memory. accept/ignore is recorded on
// the snapshot and written back onto the real QEvent once dispatch returns,
// which is what lets a closeEvent override actually cancel the close.
// Previously every no-argument virtual passed Qnil, so `event.ignore` raised
// NoMethodError on nil, the raise was swallowed, and the window closed anyway.
static VALUE cEventCls = Qnil, cCloseEventCls = Qnil, cPaintEventCls = Qnil,
             cWheelEventCls = Qnil, cShowEventCls = Qnil, cResizeEventCls = Qnil,
             cFocusEventCls = Qnil, cLeaveEventCls = Qnil,
             cDragEnterEventCls = Qnil, cDragMoveEventCls = Qnil,
             cDropEventCls = Qnil, cMimeDataCls = Qnil;

static VALUE new_event(VALUE klass, int type) {
  if (NIL_P(klass)) return Qnil;
  VALUE ev = rb_obj_alloc(klass);
  rb_ivar_set(ev, rb_intern("@accepted"), Qtrue);  // Qt's default for these
  rb_ivar_set(ev, rb_intern("@type"), INT2NUM(type));
  return ev;
}

VALUE ruby_make_plain_event(int kind, int type) {
  VALUE k;
  switch (kind) {
    case RUBY_EV_CLOSE:  k = cCloseEventCls;  break;
    case RUBY_EV_SHOW:   k = cShowEventCls;   break;
    case RUBY_EV_RESIZE: k = cResizeEventCls; break;
    case RUBY_EV_FOCUS:  k = cFocusEventCls;  break;
    case RUBY_EV_LEAVE:  k = cLeaveEventCls;  break;
    default:             k = cEventCls;       break;
  }
  return new_event(k, type);
}

VALUE ruby_make_paint_event(int x, int y, int w, int h, int type) {
  VALUE ev = new_event(cPaintEventCls, type);
  if (!NIL_P(ev))
    rb_ivar_set(ev, rb_intern("@rect"), wrap_val<QRect>(cRect, QRect(x, y, w, h)));
  return ev;
}

VALUE ruby_make_wheel_event(int dx, int dy, int mods, int type) {
  VALUE ev = new_event(cWheelEventCls, type);
  if (NIL_P(ev)) return ev;
  rb_ivar_set(ev, rb_intern("@angle_x"),   INT2NUM(dx));
  rb_ivar_set(ev, rb_intern("@angle_y"),   INT2NUM(dy));
  rb_ivar_set(ev, rb_intern("@modifiers"), INT2NUM(mods));
  return ev;
}

VALUE ruby_make_key_event(int key, const char *text, int mods, int type) {
  VALUE ev = new_event(cKeyEventCls, type);
  if (NIL_P(ev)) return ev;
  rb_ivar_set(ev, rb_intern("@key"),       INT2NUM(key));
  rb_ivar_set(ev, rb_intern("@text"),      rb_str_new2(text ? text : ""));
  rb_ivar_set(ev, rb_intern("@modifiers"), INT2NUM(mods));
  return ev;
}

VALUE ruby_make_mouse_event(int x, int y, int button, int buttons, int mods, int type) {
  VALUE ev = new_event(cMouseEventCls, type);
  if (NIL_P(ev)) return ev;
  rb_ivar_set(ev, rb_intern("@x"),         INT2NUM(x));
  rb_ivar_set(ev, rb_intern("@y"),         INT2NUM(y));
  rb_ivar_set(ev, rb_intern("@button"),    INT2NUM(button));
  rb_ivar_set(ev, rb_intern("@buttons"),   INT2NUM(buttons));
  rb_ivar_set(ev, rb_intern("@modifiers"), INT2NUM(mods));
  rb_ivar_set(ev, rb_intern("@pos"),       wrap_val<QPoint>(cPoint, QPoint(x, y)));
  return ev;
}

// The QMimeData behind a drop also dies with the event, so it is snapshotted
// to exactly what COSMOS reads off it: hasUrls, urls and text.
static VALUE make_mime_snapshot(const void *mimev) {
  const QMimeData *md = static_cast<const QMimeData *>(mimev);
  if (NIL_P(cMimeDataCls)) return Qnil;
  VALUE m = rb_obj_alloc(cMimeDataCls);
  rb_ivar_set(m, rb_intern("@hasUrls"), (md && md->hasUrls()) ? Qtrue : Qfalse);
  VALUE arr = rb_ary_new();
  if (md) {
    const QList<QUrl> us = md->urls();
    for (int i = 0; i < us.size(); i++) rb_ary_push(arr, wrap_val<QUrl>(cUrl, us.at(i)));
  }
  rb_ivar_set(m, rb_intern("@urls"), arr);
  rb_ivar_set(m, rb_intern("@text"),
              rb_str_new2(md ? md->text().toUtf8().constData() : ""));
  return m;
}

VALUE ruby_make_drop_event(int kind, const void *mime, int type) {
  VALUE k = (kind == RUBY_EV_DRAGENTER) ? cDragEnterEventCls
          : (kind == RUBY_EV_DRAGMOVE)  ? cDragMoveEventCls
                                        : cDropEventCls;
  VALUE ev = new_event(k, type);
  if (NIL_P(ev)) return ev;
  rb_ivar_set(ev, rb_intern("@proposed"), Qfalse);
  rb_ivar_set(ev, rb_intern("@mimeData"), make_mime_snapshot(mime));
  return ev;
}

bool ruby_event_accepted(VALUE ev) {
  if (NIL_P(ev)) return true;
  return RTEST(rb_ivar_get(ev, rb_intern("@accepted")));
}
bool ruby_event_proposed(VALUE ev) {
  if (NIL_P(ev)) return false;
  return RTEST(rb_ivar_get(ev, rb_intern("@proposed")));
}

static VALUE event_accept(VALUE self) {
  rb_ivar_set(self, rb_intern("@accepted"), Qtrue);  return Qnil;
}
static VALUE event_ignore(VALUE self) {
  rb_ivar_set(self, rb_intern("@accepted"), Qfalse); return Qnil;
}
static VALUE event_is_accepted(VALUE self) {
  return rb_ivar_get(self, rb_intern("@accepted"));
}
static VALUE event_set_accepted(VALUE self, VALUE v) {
  rb_ivar_set(self, rb_intern("@accepted"), RTEST(v) ? Qtrue : Qfalse); return v;
}
static VALUE event_accept_proposed(VALUE self) {
  rb_ivar_set(self, rb_intern("@proposed"), Qtrue);
  rb_ivar_set(self, rb_intern("@accepted"), Qtrue);
  return Qnil;
}
// Qt4 reported one number per wheel notch; Qt6 splits it into angleDelta.
static VALUE wheel_delta(VALUE self) { return rb_ivar_get(self, rb_intern("@angle_y")); }

// ---- remaining widget/painter odds and ends --------------------------------
static VALUE widget_set_background_role(VALUE self, VALUE r) {
  qcast<QWidget>(self)->setBackgroundRole((QPalette::ColorRole)NUM2INT(r));
  return self;
}
static VALUE widget_unset_cursor(VALUE self) {
  qcast<QWidget>(self)->unsetCursor(); return self;
}
static VALUE widget_map_from_global(VALUE self, VALUE pt) {
  return wrap_val<QPoint>(cPoint,
    qcast<QWidget>(self)->mapFromGlobal(*get_val<QPoint>(pt)));
}
static VALUE widget_map_to_global(VALUE self, VALUE pt) {
  return wrap_val<QPoint>(cPoint,
    qcast<QWidget>(self)->mapToGlobal(*get_val<QPoint>(pt)));
}
// COSMOS guards drawing with `if @painter.isActive and @painter.paintEngine`.
static VALUE painter_paint_engine(VALUE self) {
  QPainter *p2 = painter_wrap(self)->p;
  return (p2 && p2->paintEngine()) ? Qtrue : Qnil;
}

// ---- Qt::GLWidget (QGLWidget was removed in Qt6) ---------------------------
static VALUE glw_make_current(VALUE self) {
  qcast<QOpenGLWidget>(self)->makeCurrent(); return self;
}
static VALUE glw_done_current(VALUE self) {
  qcast<QOpenGLWidget>(self)->doneCurrent(); return self;
}
// Qt4's updateGL(); QOpenGLWidget schedules a repaint with update() and swaps
// buffers itself, so there is no swapBuffers equivalent to call.
static VALUE glw_update_gl(VALUE self) {
  qcast<QOpenGLWidget>(self)->update(); return self;
}
static VALUE glw_is_valid(VALUE self) {
  return qcast<QOpenGLWidget>(self)->isValid() ? Qtrue : Qfalse;
}
static VALUE glw_grab_framebuffer(VALUE self) {
  return wrap_val<QImage>(cImage,
    qcast<QOpenGLWidget>(self)->grabFramebuffer());
}

// ---------------------------------------------------------------------------
// Model/view support
//
// COSMOS subclasses Qt::StyledItemDelegate and overrides createEditor /
// setEditorData / setModelData / paint (cmd_sender/cmd_param_table_item_delegate.rb,
// table_manager/table_manager.rb). Those are C++ virtuals, so they need the
// same dispatch treatment as paintEvent, plus wrapping for the QModelIndex and
// the borrowed QPainter they are handed.
// ---------------------------------------------------------------------------
static VALUE cModelIndex = Qnil, cPainterKlass = Qnil, cStyleOptionViewItemKlass = Qnil;

// Converts one signal argument, given its QMetaType id and the raw pointer Qt
// handed us, into a Ruby VALUE. The GVL must be held (the caller builds every
// argument inside ruby_run_with_gvl). Unknown types become nil rather than
// raising, so an unmapped parameter degrades instead of breaking the signal.
VALUE ruby_value_from_meta(int typeId, void *data) {
  if (!data) return Qnil;
  switch (typeId) {
    case QMetaType::Void:        return Qnil;
    case QMetaType::Bool:        return *(bool *)data ? Qtrue : Qfalse;
    case QMetaType::Int:         return INT2NUM(*(int *)data);
    case QMetaType::UInt:        return UINT2NUM(*(unsigned int *)data);
    case QMetaType::Long:        return LONG2NUM(*(long *)data);
    case QMetaType::ULong:       return ULONG2NUM(*(unsigned long *)data);
    case QMetaType::LongLong:    return LL2NUM(*(qlonglong *)data);
    case QMetaType::ULongLong:   return ULL2NUM(*(qulonglong *)data);
    case QMetaType::Double:      return rb_float_new(*(double *)data);
    case QMetaType::Float:       return rb_float_new(*(float *)data);
    case QMetaType::QString:     return rb_str_new2(((QString *)data)->toUtf8().constData());
    case QMetaType::QModelIndex: return ruby_wrap_model_index(*(QModelIndex *)data);
    case QMetaType::QPoint:      return wrap_val<QPoint>(cPoint, *(QPoint *)data);
    case QMetaType::QRect:       return wrap_val<QRect>(cRect, *(QRect *)data);
    case QMetaType::QSize:       return wrap_val<QSize>(cSize, *(QSize *)data);
    case QMetaType::QFont:       return wrap_val<QFont>(cFont, *(QFont *)data);
    case QMetaType::QColor:      return wrap_val<QColor>(cColor, *(QColor *)data);
    default: break;
  }
  QMetaType mt(typeId);
  if (mt.flags() & QMetaType::PointerToQObject)
    return ruby_wrap_qobject(*(QObject **)data);
  const char *n = mt.name();
  if (!n) return Qnil;
  // The item classes are not QObjects, so they need their own wrappers.
  if (!strcmp(n, "QTreeWidgetItem*"))
    return wrap_ptr<QTreeWidgetItem>(cTreeWidgetItem, *(QTreeWidgetItem **)data);
  if (!strcmp(n, "QTableWidgetItem*"))
    return wrap_ptr<QTableWidgetItem>(cTableWidgetItem, *(QTableWidgetItem **)data);
  if (!strcmp(n, "QListWidgetItem*"))
    return wrap_ptr<QListWidgetItem>(cListWidgetItem, *(QListWidgetItem **)data);
  return Qnil;
}

VALUE ruby_wrap_model_index(const QModelIndex &idx) {
  return wrap_val<QModelIndex>(cModelIndex, idx);
}
// A copy, not a pointer: the QStyleOptionViewItem the view hands the delegate
// is a stack temporary that is gone the moment paint() returns.
VALUE ruby_wrap_style_option_view_item(const void *opt) {
  if (NIL_P(cStyleOptionViewItemKlass)) return Qnil;
  return wrap_val<QStyleOptionViewItem>(
      cStyleOptionViewItemKlass, *static_cast<const QStyleOptionViewItem *>(opt));
}
// Non-owning: the delegate's QPainter belongs to the view.
VALUE ruby_wrap_painter_borrowed(QPainter *p2) {
  PainterWrap *w;
  VALUE o = TypedData_Make_Struct(cPainterKlass, PainterWrap, &painter_type, w);
  w->p = p2;
  w->owned = false;
  return o;
}
VALUE ruby_wrap_qobject(QObject *o) {
  if (!o) return Qnil;
  {
    ObjMapLock lk(g_objmap_mutex);
    std::map<QObject *, ObjRef>::iterator it = g_objmap.find(o);
    if (it != g_objmap.end()) return it->second.v;
  }
  VALUE fallback = qobject_cast<QWidget *>(o) ? cWidget : cQtObject;
  return wrap_obj(best_ruby_class(o, fallback), o, false);
}
QWidget *ruby_unwrap_widget(VALUE v) {
  if (NIL_P(v)) return NULL;
  return qobject_cast<QWidget *>(get_obj(v));
}

// Dispatch that returns the Ruby result (createEditor must hand back a widget).
static VALUE ev_call_ret(VALUE p) {
  EvCall *c = (EvCall *)p;
  return rb_funcallv(c->obj, c->mid, c->argc, c->argv);
}
struct RetCall { EvCall ev; VALUE result; };
static bool ev_call_ret_protected(RetCall *r, const char *context) {
  int state = 0;
  r->result = rb_protect(ev_call_ret, (VALUE)&r->ev, &state);
  if (ruby_contain_error(state, context)) { r->result = Qnil; return false; }
  return true;
}
// PRECONDITION: the GVL is held (see ruby_event_dispatch_n).
VALUE ruby_event_call(QObject *obj, const char *method, int argc, VALUE *argv, bool *handled) {
  *handled = false;
  VALUE self;
  {
    ObjMapLock lk(g_objmap_mutex);
    std::map<QObject *, ObjRef>::iterator it = g_objmap.find(obj);
    if (it == g_objmap.end()) return Qnil;
    self = it->second.v;
  }
  ID mid = rb_intern(method);
  if (!rb_respond_to(self, mid)) return Qnil;
  RetCall r;
  r.ev.obj = self; r.ev.mid = mid; r.ev.argc = argc;
  for (int i = 0; i < argc && i < 4; i++) r.ev.argv[i] = argv[i];
  r.result = Qnil;
  *handled = ev_call_ret_protected(&r, method);
  return r.result;
}

// ---- Qt::ModelIndex --------------------------------------------------------
static VALUE mi_row(VALUE self)    { return INT2NUM(get_val<QModelIndex>(self)->row()); }
static VALUE mi_column(VALUE self) { return INT2NUM(get_val<QModelIndex>(self)->column()); }
static VALUE mi_valid(VALUE self)  { return get_val<QModelIndex>(self)->isValid() ? Qtrue : Qfalse; }
static VALUE mi_data(int argc, VALUE *argv, VALUE self) {
  VALUE role; rb_scan_args(argc, argv, "01", &role);
  QVariant v = get_val<QModelIndex>(self)->data(NIL_P(role) ? Qt::DisplayRole : NUM2INT(role));
  return wrap_val<QVariant>(cVariant, v);
}
static VALUE mi_parent(VALUE self) {
  return ruby_wrap_model_index(get_val<QModelIndex>(self)->parent());
}

// ---- views / models --------------------------------------------------------
static VALUE view_set_model(VALUE self, VALUE m) {
  QAbstractItemModel *mod = qobject_cast<QAbstractItemModel *>(get_obj(m));
  QObject *o = get_obj(self);
  if (QAbstractItemView *v = qobject_cast<QAbstractItemView *>(o))  v->setModel(mod);
  else if (QComboBox *c = qobject_cast<QComboBox *>(o))             c->setModel(mod);
  else rb_raise(rb_eRuntimeError, "setModel: unsupported receiver");
  return self;
}
static VALUE view_set_root_index(VALUE self, VALUE idx) {
  qcast<QAbstractItemView>(self)->setRootIndex(*get_val<QModelIndex>(idx));
  return self;
}
static VALUE view_set_item_delegate(VALUE self, VALUE d) {
  QAbstractItemDelegate *del = qobject_cast<QAbstractItemDelegate *>(get_obj(d));
  qcast<QAbstractItemView>(self)->setItemDelegate(del);
  release_ownership(d);
  return self;
}
static VALUE view_set_current_index(VALUE self, VALUE idx) {
  qcast<QAbstractItemView>(self)->setCurrentIndex(*get_val<QModelIndex>(idx));
  return self;
}
// QAbstractItemView::indexAt(viewport point) -> QModelIndex.
// status_tab.rb:262 maps a cell widget's position back to its row to decide
// which background task the START/STOP button belongs to.
static VALUE view_index_at(VALUE self, VALUE pt) {
  QPoint *p = get_val<QPoint>(pt);
  if (!p) return ruby_wrap_model_index(QModelIndex());
  return ruby_wrap_model_index(qcast<QAbstractItemView>(self)->indexAt(*p));
}
static VALUE view_row_at(VALUE self, VALUE y) {
  return INT2NUM(qcast<QTableView>(self)->rowAt(NUM2INT(y)));
}
static VALUE view_column_at(VALUE self, VALUE x) {
  return INT2NUM(qcast<QTableView>(self)->columnAt(NUM2INT(x)));
}
static VALUE view_current_index(VALUE self) {
  return ruby_wrap_model_index(
    qcast<QAbstractItemView>(self)->currentIndex());
}
static VALUE view_scroll_to(VALUE self, VALUE idx) {
  qcast<QAbstractItemView>(self)->scrollTo(*get_val<QModelIndex>(idx));
  return self;
}
static VALUE tree_expand(VALUE self, VALUE idx) {
  qcast<QTreeView>(self)->expand(*get_val<QModelIndex>(idx)); return self;
}
static VALUE tree_collapse(VALUE self, VALUE idx) {
  qcast<QTreeView>(self)->collapse(*get_val<QModelIndex>(idx)); return self;
}
static VALUE view_set_column_hidden(VALUE self, VALUE col, VALUE hide) {
  QObject *o = get_obj(self);
  if (QTreeView *t = qobject_cast<QTreeView *>(o)) t->setColumnHidden(NUM2INT(col), RTEST(hide));
  return self;
}
static VALUE fsmodel_set_root_path(VALUE self, VALUE path) {
  return ruby_wrap_model_index(
    qcast<QFileSystemModel>(self)->setRootPath(rb_to_qs(path)));
}
static VALUE fsmodel_index(VALUE self, VALUE path) {
  return ruby_wrap_model_index(
    qcast<QFileSystemModel>(self)->index(rb_to_qs(path)));
}
static VALUE fsmodel_file_path(VALUE self, VALUE idx) {
  return rb_str_new2(qcast<QFileSystemModel>(self)
                       ->filePath(*get_val<QModelIndex>(idx)).toUtf8().constData());
}
static VALUE slmodel_init(int argc, VALUE *argv, VALUE self) {
  if (get_wrap(self)->ptr) return self;
  VALUE list, parent; rb_scan_args(argc, argv, "02", &list, &parent);
  QStringList sl;
  if (!NIL_P(list) && RB_TYPE_P(list, T_ARRAY))
    for (long i = 0; i < RARRAY_LEN(list); i++) sl << rb_to_qs(rb_ary_entry(list, i));
  attach(self, new QStringListModel(sl), true);
  return self;
}
// Qt4's QHeaderView::setResizeMode was renamed setSectionResizeMode in Qt5.
static VALUE header_set_resize_mode(int argc, VALUE *argv, VALUE self) {
  QHeaderView *h = qcast<QHeaderView>(self);
  if (argc == 1) h->setSectionResizeMode((QHeaderView::ResizeMode)NUM2INT(argv[0]));
  else           h->setSectionResizeMode(NUM2INT(argv[0]),
                                         (QHeaderView::ResizeMode)NUM2INT(argv[1]));
  return self;
}
static VALUE cHeaderViewKlass = Qnil;
static VALUE table_vertical_header(VALUE self) {
  return wrap_obj(cHeaderViewKlass,
                  qcast<QTableView>(self)->verticalHeader(), false);
}
static VALUE table_horizontal_header(VALUE self) {
  return wrap_obj(cHeaderViewKlass,
                  qcast<QTableView>(self)->horizontalHeader(), false);
}
static VALUE tw_cell_widget(VALUE self, VALUE r, VALUE c) {
  QWidget *w = qcast<QTableWidget>(self)->cellWidget(NUM2INT(r), NUM2INT(c));
  return w ? wrap_obj(best_ruby_class(w, cWidget), w, false) : Qnil;
}
static VALUE tw_remove_cell_widget(VALUE self, VALUE r, VALUE c) {
  qcast<QTableWidget>(self)->removeCellWidget(NUM2INT(r), NUM2INT(c)); return self;
}
static VALUE tw_clear_contents(VALUE self) {
  qcast<QTableWidget>(self)->clearContents(); return self;
}
static VALUE tw_edit_item(VALUE self, VALUE item) {
  qcast<QTableWidget>(self)->editItem(get_ptr<QTableWidgetItem>(item));
  return self;
}
static VALUE tw_set_current_cell(VALUE self, VALUE r, VALUE c) {
  qcast<QTableWidget>(self)->setCurrentCell(NUM2INT(r), NUM2INT(c));
  return self;
}
static VALUE completer_set_model(VALUE self, VALUE m) {
  qcast<QCompleter>(self)
    ->setModel(qobject_cast<QAbstractItemModel *>(get_obj(m)));
  return self;
}

// ---- batch: methods the tools need at runtime ------------------------------
static VALUE cMovieKlass = Qnil, cTextCursorKlass = Qnil;

// Qt::Movie (animated splash/status icons)
static VALUE movie_init(int argc, VALUE *argv, VALUE self) {
  if (get_wrap(self)->ptr) return self;
  VALUE f; rb_scan_args(argc, argv, "01", &f);
  attach(self, NIL_P(f) ? new QMovie() : new QMovie(rb_to_qs(f)), true);
  return self;
}
static VALUE movie_start(VALUE self) { qcast<QMovie>(self)->start(); return self; }
static VALUE movie_stop(VALUE self)  { qcast<QMovie>(self)->stop();  return self; }
static VALUE label_set_movie(VALUE self, VALUE m) {
  qcast<QLabel>(self)->setMovie(qobject_cast<QMovie *>(get_obj(m)));
  return self;
}

static VALUE mainwindow_central_widget(VALUE self) {
  QWidget *w = qcast<QMainWindow>(self)->centralWidget();
  return w ? wrap_obj(cWidget, w, false) : Qnil;
}
static VALUE widget_add_action(VALUE self, VALUE a) {
  qcast<QWidget>(self)->addAction(qobject_cast<QAction *>(get_obj(a)));
  return self;
}
static VALUE splitter_set_orientation(VALUE self, VALUE o) {
  qcast<QSplitter>(self)->setOrientation((Qt::Orientation)NUM2INT(o));
  return self;
}
// QMenu has no setText; qtbindings routed it to the menu's own action.
static VALUE menu_set_text(VALUE self, VALUE t) {
  qcast<QMenu>(self)->setTitle(rb_to_qs(t)); return self;
}
static VALUE widget_set_accept_drops(VALUE self, VALUE b) {
  qcast<QWidget>(self)->setAcceptDrops(RTEST(b)); return self;
}
static VALUE widget_set_focus(int argc, VALUE *argv, VALUE self) {
  qcast<QWidget>(self)->setFocus(); return self;
}
static VALUE widget_set_window_flags(VALUE self, VALUE f) {
  qcast<QWidget>(self)->setWindowFlags((Qt::WindowFlags)NUM2INT(f));
  return self;
}
static VALUE widget_remove_action(VALUE self, VALUE a) {
  qcast<QWidget>(self)->removeAction(qobject_cast<QAction *>(get_obj(a)));
  return self;
}
static VALUE menu_insert_separator(VALUE self, VALUE before) {
  QMenu *m = qcast<QMenu>(self);
  m->insertSeparator(NIL_P(before) ? NULL : qobject_cast<QAction *>(get_obj(before)));
  return self;
}
static VALUE actiongroup_add_action(VALUE self, VALUE a) {
  qcast<QActionGroup>(self)->addAction(qobject_cast<QAction *>(get_obj(a)));
  return self;
}
static VALUE actiongroup_set_exclusive(VALUE self, VALUE b) {
  qcast<QActionGroup>(self)->setExclusive(RTEST(b)); return self;
}
static VALUE image_bits(VALUE self) {
  // COSMOS does img.bits.unpack(...), so this must be the raw pixel buffer.
  QImage *im = get_val<QImage>(self);
  return rb_str_new((const char *)im->constBits(), (long)im->sizeInBytes());
}

// Qt::TextCursor -- script_runner / config_editor drive the editor through it.
static VALUE tc_new(int argc, VALUE *argv, VALUE klass) {
  VALUE doc; rb_scan_args(argc, argv, "01", &doc);
  if (NIL_P(doc)) return wrap_val<QTextCursor>(klass, QTextCursor());
  QTextDocument *d = qobject_cast<QTextDocument *>(get_obj(doc));
  return wrap_val<QTextCursor>(klass, QTextCursor(d));
}
// QTextEdit::ExtraSelection is a plain struct of {cursor, format}; COSMOS's
// RubyEditor uses it for current-line highlighting. Modelled as a small Ruby
// object whose ivars are read back here.
static VALUE te_set_extra_selections(VALUE self, VALUE ary) {
  QList<QTextEdit::ExtraSelection> sels;
  for (long i = 0; i < RARRAY_LEN(ary); i++) {
    VALUE e = rb_ary_entry(ary, i);
    VALUE cur = rb_ivar_get(e, rb_intern("@cursor"));
    VALUE fmt = rb_ivar_get(e, rb_intern("@format"));
    QTextEdit::ExtraSelection sel;
    if (!NIL_P(cur)) sel.cursor = *get_val<QTextCursor>(cur);
    if (!NIL_P(fmt)) sel.format = *get_val<QTextCharFormat>(fmt);
    sels << sel;
  }
  QObject *o = get_obj(self);
  if (QPlainTextEdit *pe = qobject_cast<QPlainTextEdit *>(o)) pe->setExtraSelections(sels);
  else qobject_cast<QTextEdit *>(o)->setExtraSelections(sels);
  return self;
}
static VALUE tcf_set_property(VALUE self, VALUE prop, VALUE v) {
  QTextCharFormat *f = get_val<QTextCharFormat>(self);
  if (rb_obj_is_kind_of(v, cVariant)) f->setProperty(NUM2INT(prop), *get_val<QVariant>(v));
  else if (v == Qtrue || v == Qfalse) f->setProperty(NUM2INT(prop), QVariant(RTEST(v)));
  else if (RB_TYPE_P(v, T_FIXNUM))    f->setProperty(NUM2INT(prop), QVariant(NUM2INT(v)));
  else                                f->setProperty(NUM2INT(prop), QVariant(rb_to_qs(v)));
  return self;
}
static VALUE tc_move_position(int argc, VALUE *argv, VALUE self) {
  VALUE op, mode, n; rb_scan_args(argc, argv, "12", &op, &mode, &n);
  return get_val<QTextCursor>(self)->movePosition(
    (QTextCursor::MoveOperation)NUM2INT(op),
    NIL_P(mode) ? QTextCursor::MoveAnchor : (QTextCursor::MoveMode)NUM2INT(mode),
    NIL_P(n) ? 1 : NUM2INT(n)) ? Qtrue : Qfalse;
}
static VALUE tc_insert_text(VALUE self, VALUE t) {
  get_val<QTextCursor>(self)->insertText(rb_to_qs(t)); return self;
}
static VALUE tc_set_position(int argc, VALUE *argv, VALUE self) {
  VALUE pos, mode; rb_scan_args(argc, argv, "11", &pos, &mode);
  get_val<QTextCursor>(self)->setPosition(NUM2INT(pos),
    NIL_P(mode) ? QTextCursor::MoveAnchor : (QTextCursor::MoveMode)NUM2INT(mode));
  return self;
}
static VALUE tc_position(VALUE self)       { return INT2NUM(get_val<QTextCursor>(self)->position()); }
static VALUE tc_block_number(VALUE self)   { return INT2NUM(get_val<QTextCursor>(self)->blockNumber()); }
static VALUE tb_text(VALUE self) {
  return rb_str_new2(get_val<QTextBlock>(self)->text().toUtf8().constData());
}
static VALUE tb_number(VALUE self)   { return INT2NUM(get_val<QTextBlock>(self)->blockNumber()); }
static VALUE tb_position(VALUE self) { return INT2NUM(get_val<QTextBlock>(self)->position()); }
static VALUE tb_length(VALUE self)   { return INT2NUM(get_val<QTextBlock>(self)->length()); }
static VALUE tb_valid(VALUE self)    { return get_val<QTextBlock>(self)->isValid() ? Qtrue : Qfalse; }
static VALUE tb_next(VALUE self) {
  return wrap_val<QTextBlock>(cTextBlockKlass, get_val<QTextBlock>(self)->next());
}
static VALUE tc_block(VALUE self) {
  return wrap_val<QTextBlock>(cTextBlockKlass, get_val<QTextCursor>(self)->block());
}
static VALUE doc_find_block_by_number(VALUE self, VALUE n) {
  return wrap_val<QTextBlock>(cTextBlockKlass,
    qcast<QTextDocument>(self)->findBlockByNumber(NUM2INT(n)));
}
static VALUE tc_position_in_block(VALUE self) {
  return INT2NUM(get_val<QTextCursor>(self)->positionInBlock());
}
static VALUE tc_anchor(VALUE self)          { return INT2NUM(get_val<QTextCursor>(self)->anchor()); }
static VALUE tc_has_selection(VALUE self)   { return get_val<QTextCursor>(self)->hasSelection() ? Qtrue : Qfalse; }
static VALUE tc_clear_selection(VALUE self) { get_val<QTextCursor>(self)->clearSelection(); return self; }
static VALUE tc_remove_selected(VALUE self) { get_val<QTextCursor>(self)->removeSelectedText(); return self; }
static VALUE tc_begin_edit(VALUE self)      { get_val<QTextCursor>(self)->beginEditBlock(); return self; }
static VALUE tc_end_edit(VALUE self)        { get_val<QTextCursor>(self)->endEditBlock(); return self; }
static VALUE tc_at_end(VALUE self)          { return get_val<QTextCursor>(self)->atEnd() ? Qtrue : Qfalse; }
static VALUE tc_at_start(VALUE self)        { return get_val<QTextCursor>(self)->atStart() ? Qtrue : Qfalse; }
static VALUE tc_block_text(VALUE self) {
  return rb_str_new2(get_val<QTextCursor>(self)->block().text().toUtf8().constData());
}
static VALUE tc_selected_text(VALUE self)  {
  return rb_str_new2(get_val<QTextCursor>(self)->selectedText().toUtf8().constData());
}
static VALUE tc_select(VALUE self, VALUE sel) {
  get_val<QTextCursor>(self)->select((QTextCursor::SelectionType)NUM2INT(sel)); return self;
}
static VALUE te_text_cursor(VALUE self) {
  QObject *o = get_obj(self);
  if (QPlainTextEdit *pe = qobject_cast<QPlainTextEdit *>(o))
    return wrap_val<QTextCursor>(cTextCursorKlass, pe->textCursor());
  return wrap_val<QTextCursor>(cTextCursorKlass,
                               qobject_cast<QTextEdit *>(o)->textCursor());
}
static VALUE te_set_text_cursor(VALUE self, VALUE c) {
  QObject *o = get_obj(self);
  if (QPlainTextEdit *pe = qobject_cast<QPlainTextEdit *>(o)) pe->setTextCursor(*get_val<QTextCursor>(c));
  else qobject_cast<QTextEdit *>(o)->setTextCursor(*get_val<QTextCursor>(c));
  return self;
}
static VALUE te_ensure_cursor_visible(VALUE self) {
  QObject *o = get_obj(self);
  if (QPlainTextEdit *pe = qobject_cast<QPlainTextEdit *>(o)) pe->ensureCursorVisible();
  else qobject_cast<QTextEdit *>(o)->ensureCursorVisible();
  return self;
}
static VALUE pte_set_max_blocks(VALUE self, VALUE n) {
  qcast<QPlainTextEdit>(self)->setMaximumBlockCount(NUM2INT(n)); return self;
}
static VALUE te_set_word_wrap_mode(VALUE self, VALUE m) {
  QObject *o = get_obj(self);
  if (QPlainTextEdit *pe = qobject_cast<QPlainTextEdit *>(o))
    pe->setWordWrapMode((QTextOption::WrapMode)NUM2INT(m));
  else qobject_cast<QTextEdit *>(o)->setWordWrapMode((QTextOption::WrapMode)NUM2INT(m));
  return self;
}
static VALUE label_set_text_interaction(VALUE self, VALUE f) {
  qcast<QLabel>(self)->setTextInteractionFlags((Qt::TextInteractionFlags)NUM2INT(f));
  return self;
}
static VALUE button_set_auto_default(VALUE self, VALUE b) {
  qcast<QPushButton>(self)->setAutoDefault(RTEST(b)); return self;
}
static VALUE lw_set_current_row(VALUE self, VALUE r) {
  qcast<QListWidget>(self)->setCurrentRow(NUM2INT(r)); return self;
}
static VALUE lw_current_row(VALUE self) {
  return INT2NUM(qcast<QListWidget>(self)->currentRow());
}
static VALUE tw_set_cell_widget(VALUE self, VALUE r, VALUE c, VALUE w) {
  qcast<QTableWidget>(self)
    ->setCellWidget(NUM2INT(r), NUM2INT(c), qobject_cast<QWidget *>(get_obj(w)));
  release_ownership(w);
  return self;
}
static VALUE tab_remove_tab(VALUE self, VALUE i) {
  qcast<QTabWidget>(self)->removeTab(NUM2INT(i)); return self;
}
static VALUE combo_find_text(VALUE self, VALUE t) {
  return INT2NUM(qcast<QComboBox>(self)->findText(rb_to_qs(t)));
}
static VALUE header_set_stretch_last(VALUE self, VALUE b) {
  qcast<QHeaderView>(self)->setStretchLastSection(RTEST(b)); return self;
}
static VALUE toolbar_set_movable(VALUE self, VALUE b) {
  qcast<QToolBar>(self)->setMovable(RTEST(b)); return self;
}
static VALUE layout_remove_widget(VALUE self, VALUE w) {
  qcast<QLayout>(self)->removeWidget(qobject_cast<QWidget *>(get_obj(w)));
  return self;
}
static VALUE table_resize_rows(VALUE self) {
  qcast<QTableView>(self)->resizeRowsToContents(); return self;
}
static VALUE completer_set_prefix(VALUE self, VALUE t) {
  qcast<QCompleter>(self)->setCompletionPrefix(rb_to_qs(t)); return self;
}

// ---- cross-thread GUI marshalling ------------------------------------------
// Qt widgets may only be touched from the GUI thread. COSMOS calls
// Qt.execute_in_main_thread from background threads; this posts the block to
// the main thread's event loop via a queued invocation.
// Runs a one-shot posted block, then drops its GC anchor. Both halves must
// happen under the same GVL acquisition.
struct OneShot { VALUE proc; };
static VALUE do_one_shot(VALUE p) {
  return rb_funcall(((OneShot *)p)->proc, rb_intern("call"), 0);
}
static void *one_shot_with_gvl(void *p) {
  int state = 0;
  rb_protect(do_one_shot, (VALUE)p, &state);
  if (state) { rb_set_errinfo(Qnil); }   // never longjmp through Qt's frames
  rb_ary_delete(g_procs, ((OneShot *)p)->proc);
  return NULL;
}
void ruby_invoke_proc_once(VALUE proc) {
  if (proc == Qnil) return;
  OneShot o; o.proc = proc;
  ruby_run_with_gvl(one_shot_with_gvl, &o);
}

static VALUE qt_post_to_main(int argc, VALUE *argv, VALUE self) {
  VALUE blk;
  rb_scan_args(argc, argv, "00&", &blk);
  if (NIL_P(blk)) rb_raise(rb_eArgError, "post_to_main_thread requires a block");
  rb_ary_push(g_procs, blk);
  // Do NOT construct a QObject here: this runs on a background Ruby thread and
  // "Cannot create children for a parent that is in a different thread" is a
  // crash, not a warning. Post a plain functor to qApp's thread instead.
  VALUE proc = blk;
  QMetaObject::invokeMethod(qApp, [proc]() { ruby_invoke_proc_once(proc); },
                            Qt::QueuedConnection);
  return Qtrue;
}
static VALUE qt_on_main_thread_p(VALUE self) {
  QCoreApplication *a = QCoreApplication::instance();
  return (a && QThread::currentThread() == a->thread()) ? Qtrue : Qfalse;
}

// addMenu(text), addMenu(submenu) and addMenu(icon, text) are all used.
static VALUE menu_add_menu(int argc, VALUE *argv, VALUE self) {
  QMenu *m = qcast<QMenu>(self);
  VALUE a, b; rb_scan_args(argc, argv, "11", &a, &b);
  if (!NIL_P(b)) return wrap_obj(cMenu, m->addMenu(icon_arg(a), rb_to_qs(b)), false);
  if (RB_TYPE_P(a, T_STRING)) return wrap_obj(cMenu, m->addMenu(rb_to_qs(a)), false);
  m->addMenu(qobject_cast<QMenu *>(get_obj(a)));
  release_ownership(a);
  return a;
}
static VALUE menu_add_actions(VALUE self, VALUE ary) {
  QMenu *m = qcast<QMenu>(self);
  for (long i = 0; i < RARRAY_LEN(ary); i++) {
    VALUE a = rb_ary_entry(ary, i);
    m->addAction(qobject_cast<QAction *>(get_obj(a)));
    release_ownership(a);
  }
  return self;
}
static VALUE combo_set_completer(VALUE self, VALUE c) {
  qcast<QComboBox>(self)
    ->setCompleter(qobject_cast<QCompleter *>(get_obj(c)));
  release_ownership(c);
  return self;
}
static VALUE statusbar_add_permanent(VALUE self, VALUE w) {
  qcast<QStatusBar>(self)
    ->addPermanentWidget(qobject_cast<QWidget *>(get_obj(w)));
  release_ownership(w);
  return self;
}
// setViewportMargins is protected on QAbstractScrollArea; COSMOS's RubyEditor
// needs it to reserve room for the line-number gutter.
class ScrollAreaAccess : public QAbstractScrollArea {
public:
  using QAbstractScrollArea::setViewportMargins;
};
static VALUE scrollarea_set_viewport_margins(VALUE self, VALUE l, VALUE t, VALUE r, VALUE b) {
  QAbstractScrollArea *a = qcast<QAbstractScrollArea>(self);
  static_cast<ScrollAreaAccess *>(a)->setViewportMargins(
    NUM2INT(l), NUM2INT(t), NUM2INT(r), NUM2INT(b));
  return self;
}
static VALUE scrollarea_viewport(VALUE self) {
  return wrap_obj(cWidget,
    qcast<QAbstractScrollArea>(self)->viewport(), false);
}
static VALUE cScrollBarKlass = Qnil;
static VALUE scrollarea_vscrollbar(VALUE self) {
  return wrap_obj(cScrollBarKlass,
    qcast<QAbstractScrollArea>(self)->verticalScrollBar(), false);
}
static VALUE scrollarea_hscrollbar(VALUE self) {
  return wrap_obj(cScrollBarKlass,
    qcast<QAbstractScrollArea>(self)->horizontalScrollBar(), false);
}
static VALUE splitter_set_sizes(VALUE self, VALUE ary) {
  QList<int> sizes;
  for (long i = 0; i < RARRAY_LEN(ary); i++) sizes << NUM2INT(rb_ary_entry(ary, i));
  qcast<QSplitter>(self)->setSizes(sizes);
  return self;
}
static VALUE app_style(VALUE klass) {
  return wrap_obj(rb_const_get(mQt, rb_intern("Style")), QApplication::style(), false);
}
static VALUE tab_set_movable(VALUE self, VALUE b) {
  qcast<QTabWidget>(self)->setMovable(RTEST(b)); return self;
}
static VALUE lw_set_sorting_enabled(VALUE self, VALUE b) {
  qcast<QListWidget>(self)->setSortingEnabled(RTEST(b)); return self;
}
static VALUE tw_set_sorting_enabled(VALUE self, VALUE b) {
  qcast<QTableWidget>(self)->setSortingEnabled(RTEST(b)); return self;
}
static VALUE actiongroup_actions(VALUE self) {
  QList<QAction *> as = qcast<QActionGroup>(self)->actions();
  VALUE ary = rb_ary_new();
  for (int i = 0; i < as.size(); i++) rb_ary_push(ary, wrap_obj(cAction, as[i], false));
  return ary;
}
static VALUE completer_set_case_sensitivity(VALUE self, VALUE v) {
  qcast<QCompleter>(self)->setCaseSensitivity((Qt::CaseSensitivity)NUM2INT(v));
  return self;
}
static VALUE aiv_set_drag_drop_mode(VALUE self, VALUE v) {
  qcast<QAbstractItemView>(self)
    ->setDragDropMode((QAbstractItemView::DragDropMode)NUM2INT(v));
  return self;
}
static VALUE menu_set_icon(VALUE self, VALUE i) {
  qcast<QMenu>(self)->setIcon(icon_arg(i)); return self;
}
static VALUE completer_set_widget(VALUE self, VALUE w) {
  qcast<QCompleter>(self)->setWidget(qobject_cast<QWidget *>(get_obj(w)));
  return self;
}
static VALUE pixmap_from_image(VALUE klass, VALUE img) {
  return wrap_val<QPixmap>(cPixmap, QPixmap::fromImage(*get_val<QImage>(img)));
}
static VALUE pixmap_grab_widget(int argc, VALUE *argv, VALUE klass) {
  VALUE w; rb_scan_args(argc, argv, "1*", &w, NULL);
  return wrap_val<QPixmap>(cPixmap, qobject_cast<QWidget *>(get_obj(w))->grab());
}
static VALUE app_start_drag_distance(VALUE klass) {
  return INT2NUM(QApplication::startDragDistance());
}
static VALUE dialog_set_modal(VALUE self, VALUE b) {
  qcast<QDialog>(self)->setModal(RTEST(b)); return self;
}

// ---- introspection: needed to drive a running GUI from a test --------------
static VALUE obj_find_children(int argc, VALUE *argv, VALUE self) {
  VALUE want; rb_scan_args(argc, argv, "01", &want);
  QList<QObject *> kids = get_obj(self)->findChildren<QObject *>();
  QByteArray filter = NIL_P(want) ? QByteArray() : QByteArray(StringValueCStr(want));
  VALUE ary = rb_ary_new();
  for (int i = 0; i < kids.size(); i++) {
    QObject *k = kids.at(i);
    if (!filter.isEmpty()) {
      bool match = false;
      for (const QMetaObject *mo = k->metaObject(); mo; mo = mo->superClass())
        if (filter == mo->className()) { match = true; break; }
      if (!match) continue;
    }
    rb_ary_push(ary, wrap_obj(best_ruby_class(k, cQtObject), k, false));
  }
  return ary;
}
static VALUE obj_find_child(VALUE self, VALUE name) {
  QObject *k = get_obj(self)->findChild<QObject *>(rb_to_qs(name));
  return k ? wrap_obj(best_ruby_class(k, cQtObject), k, false) : Qnil;
}
static VALUE obj_qt_class_name(VALUE self) {
  return rb_str_new2(get_obj(self)->metaObject()->className());
}
// Route an existing wrapper's class through the map (COSMOS subclasses stay put).
static VALUE widget_children_widgets(VALUE self) {
  QList<QWidget *> kids = qcast<QWidget>(self)->findChildren<QWidget *>();
  VALUE ary = rb_ary_new();
  for (int i = 0; i < kids.size(); i++)
    rb_ary_push(ary, wrap_obj(best_ruby_class(kids.at(i), cWidget), kids.at(i), false));
  return ary;
}

// ---------- connect by signal NAME (the COSMOS idiom) ----------
static VALUE qt_connect(int argc, VALUE *argv, VALUE self) {
  VALUE sender, signal, blk;
  rb_scan_args(argc, argv, "20&", &sender, &signal, &blk);
  if (NIL_P(blk)) rb_raise(rb_eArgError, "connect requires a block");

  QObject *s = get_obj(sender);
  QByteArray sig = QMetaObject::normalizedSignature(StringValueCStr(signal));
  int sidx = s->metaObject()->indexOfSignal(sig);
  if (sidx < 0) {
    // Qt6 renamed several string-valued signals; COSMOS still uses Qt4 names.
    static const char *renames[][2] = {
      { "activated(QString)",           "textActivated(QString)" },
      { "highlighted(QString)",         "textHighlighted(QString)" },
      { "currentIndexChanged(QString)", "currentTextChanged(QString)" },
      { NULL, NULL }
    };
    for (int i = 0; renames[i][0]; i++) {
      if (sig == renames[i][0]) {
        sidx = s->metaObject()->indexOfSignal(
                 QMetaObject::normalizedSignature(renames[i][1]));
        break;
      }
    }
  }
  if (sidx < 0) rb_raise(rb_eArgError, "no such signal: %s", StringValueCStr(signal));

  rb_ary_push(g_procs, blk);                       // keep the proc alive

  // Forward every parameter, whatever its type. Record the signal's metatype
  // ids now; qt_metacall only gets raw void* and needs them to convert.
  QMetaMethod sm = s->metaObject()->method(sidx);
  QList<int> ptypes;
  for (int i = 0; i < sm.parameterCount(); i++)
    ptypes << sm.parameterMetaType(i).id();

  RubyGenericSlot *cb = new RubyGenericSlot(blk, s, ptypes);
  if (!QMetaObject::connect(s, sidx, cb, RubyGenericSlot::slotIndex()))
    rb_raise(rb_eRuntimeError, "QMetaObject::connect failed");
  return Qtrue;
}

static VALUE qt_version(VALUE self) { return rb_str_new2(qVersion()); }

// Qt::GlobalColor values. COSMOS writes these lowercase (Qt::black), which
// Ruby parses as a method call, not a constant -- so they must be methods.
template <int C> static VALUE global_color(VALUE self) { return INT2NUM(C); }

// --- Qt::Object base methods -------------------------------------------
static VALUE obj_destroyed_p(VALUE self) {
  return get_wrap(self)->ptr ? Qfalse : Qtrue;
}
static VALUE obj_owned_p(VALUE self) {
  return get_wrap(self)->owned ? Qtrue : Qfalse;
}
static VALUE obj_destroy(VALUE self) {
  QtWrap *w = get_wrap(self);
  if (w->ptr) {
    QObject *o = w->ptr;
    { ObjMapLock lk(g_objmap_mutex); g_objmap.erase(o); }
    w->ptr = NULL;
    w->owned = false;
    delete o;            // children go with it; their wrappers dangle safely
  }
  return Qnil;
}
static VALUE obj_object_count(VALUE self) {
  ObjMapLock lk(g_objmap_mutex);
  return INT2NUM((int)g_objmap.size());
}

// ---------------------------------------------------------------------------
// Method definition strategy.
//
// COSMOS reopens Qt classes and calls super from the reopened method, e.g.
//   class Qt::Painter
//     def setPen(c) super(Cosmos.getColor(c)) end
//   end
// A method defined directly ON the class would be REPLACED by that reopen,
// leaving super with nothing to call. So every bound method goes into a
// per-class module that is included into the class: the reopened method wins
// lookup, and super finds ours in the included module.
// ---------------------------------------------------------------------------
static std::map<VALUE, VALUE> g_impl_modules;

// Instance methods live in Klass::Impl so a COSMOS reopen can call super.
// Class methods need the same treatment: script_module_gui.rb:28 reopens
// `def self.critical` and calls super. rb_define_singleton_method puts the
// method directly in the singleton class, where the reopen REPLACES it --
// so super had nothing to find. Extending from a module fixes that.
static std::map<VALUE, VALUE> g_class_impl_modules;
static VALUE class_impl_module(VALUE klass) {
  std::map<VALUE, VALUE>::iterator it = g_class_impl_modules.find(klass);
  if (it != g_class_impl_modules.end()) return it->second;
  VALUE m = rb_define_module_under(klass, "ClassImpl");
  rb_extend_object(klass, m);
  g_class_impl_modules[klass] = m;
  return m;
}
#define QSDEF(klass, name, fn, arity) \
  rb_define_method(class_impl_module(klass), name, fn, arity)

static VALUE impl_module(VALUE klass) {
  std::map<VALUE, VALUE>::iterator it = g_impl_modules.find(klass);
  if (it != g_impl_modules.end()) return it->second;
  VALUE m = rb_define_module_under(klass, "Impl");
  rb_include_module(klass, m);
  g_impl_modules[klass] = m;
  return m;
}
// Guard: using a class VALUE before Init assigns it yields 0 (== Qfalse), which
// surfaces as a baffling "can't modify frozen FalseClass". Fail loudly instead.
#define QDEF(klass, name, fn, arity) do { \
    if (!(klass) || (klass) == Qfalse) \
      rb_fatal("qt6 binding: %s defined before its class was created", name); \
    rb_define_method(impl_module(klass), name, fn, arity); \
  } while (0)

static VALUE variant_from_value(VALUE klass, VALUE v) {
  if (RB_TYPE_P(v, T_FIXNUM))     return wrap_val<QVariant>(cVariant, QVariant(NUM2INT(v)));
  if (RB_TYPE_P(v, T_FLOAT))      return wrap_val<QVariant>(cVariant, QVariant(NUM2DBL(v)));
  if (v == Qtrue || v == Qfalse)  return wrap_val<QVariant>(cVariant, QVariant(RTEST(v)));
  if (rb_obj_is_kind_of(v, cPixmap))
    return wrap_val<QVariant>(cVariant, QVariant::fromValue(*get_val<QPixmap>(v)));
  if (rb_obj_is_kind_of(v, cSize))
    return wrap_val<QVariant>(cVariant, QVariant::fromValue(*get_val<QSize>(v)));
  if (rb_obj_is_kind_of(v, cPoint))
    return wrap_val<QVariant>(cVariant, QVariant::fromValue(*get_val<QPoint>(v)));
  return wrap_val<QVariant>(cVariant, QVariant(rb_to_qs(v)));
}


// Qt::Shortcut was declared but never given a constructor, so every
// Qt::Shortcut.new fell through to ctor_plain<QObject> and built a bare
// QObject: the key sequence was never installed and connect() silently took
// the "no such signal" path and returned true. 9 shortcuts were dead
// (script_runner_frame.rb:160-166 F5/F6/F7/F10, Ctrl+Tab, Delete).
static QObject *ctor_shortcut(int argc, VALUE *argv) {
  QKeySequence ks;
  QWidget *p = parent_arg(argc, argv);
  for (int i = 0; i < argc; i++) {
    if (rb_obj_is_kind_of(argv[i], cKeySequence)) { ks = *get_val<QKeySequence>(argv[i]); break; }
    if (RB_TYPE_P(argv[i], T_STRING))  { ks = QKeySequence(rb_to_qs(argv[i])); break; }
    if (RB_TYPE_P(argv[i], T_FIXNUM))  { ks = QKeySequence((Qt::Key)NUM2INT(argv[i])); break; }
  }
  QShortcut *sc = new QShortcut(ks, p);
  if (p) g_ctor_took_parent = true;
  return sc;
}

// Ruby syntax colouring in Script Runner / Test Runner / Config Editor.
// highlightBlock is pure virtual and setFormat is protected, so a forwarding
// subclass is the only way to reach them from Ruby.
class RubyHighlighter : public QSyntaxHighlighter {
public:
  explicit RubyHighlighter(QTextDocument *doc) : QSyntaxHighlighter(doc) {}
  using QSyntaxHighlighter::setFormat;
protected:
  void highlightBlock(const QString &text) override {
    const QByteArray t = text.toUtf8();
    ruby_with_gvl([&] {
      ruby_event_dispatch(this, "highlightBlock", rb_str_new2(t.constData()));
    });
  }
};
static QObject *ctor_highlighter(int argc, VALUE *argv) {
  QTextDocument *doc = NULL;
  for (int i = 0; i < argc; i++)
    if (!NIL_P(argv[i]) && rb_obj_is_kind_of(argv[i], cQtBase))
      doc = qobject_cast<QTextDocument *>(get_obj(argv[i]));
  RubyHighlighter *h = new RubyHighlighter(doc);
  if (doc) g_ctor_took_parent = true;   // the document parents it
  return h;
}
static VALUE highlighter_set_format(VALUE self, VALUE start, VALUE count, VALUE fmt) {
  RubyHighlighter *h = static_cast<RubyHighlighter *>(
      qobject_cast<QSyntaxHighlighter *>(get_obj(self)));
  if (!h) rb_raise(rb_eTypeError, "not a Qt::SyntaxHighlighter");
  h->setFormat(NUM2INT(start), NUM2INT(count), *get_val<QTextCharFormat>(fmt));
  return self;
}

// qt.rb:645-656 installs a filter object that eats Delete/Backspace; none of
// installEventFilter, removeEventFilter or eventFilter was bound, so
// Qt::ColorListWidget#set_read_only raised NoMethodError.
class RubyFilterObject : public QObject {
public:
  explicit RubyFilterObject(QObject *parent = nullptr) : QObject(parent) {}
protected:
  bool eventFilter(QObject *watched, QEvent *ev) override {
    bool filtered = false, handled = false;
    const int type = (int)ev->type();
    const int key = (ev->type() == QEvent::KeyPress || ev->type() == QEvent::KeyRelease)
                      ? static_cast<QKeyEvent *>(ev)->key() : 0;
    ruby_with_gvl([&] {
      VALUE args[2];
      args[0] = ruby_wrap_qobject(watched);
      args[1] = (key ? ruby_make_key_event(key, "", 0, type)
                     : ruby_make_plain_event(RUBY_EV_PLAIN, type));
      VALUE r = ruby_event_call(this, "eventFilter", 2, args, &handled);
      filtered = handled && RTEST(r);
    });
    return filtered ? true : QObject::eventFilter(watched, ev);
  }
};
static VALUE obj_install_event_filter(VALUE self, VALUE f) {
  get_obj(self)->installEventFilter(get_obj(f));
  return self;
}
static VALUE obj_remove_event_filter(VALUE self, VALUE f) {
  get_obj(self)->removeEventFilter(get_obj(f));
  return self;
}

// cmd_param_table_item_delegate.rb:33-34 emits commitData then closeEditor;
// neither was bound, so the user's Cmd Sender selection was silently dropped.
static VALUE delegate_commit_data(VALUE self, VALUE editor) {
  QAbstractItemDelegate *d = qcast<QAbstractItemDelegate>(self);
  emit d->commitData(qobject_cast<QWidget *>(get_obj(editor)));
  return self;
}
static VALUE delegate_close_editor(int argc, VALUE *argv, VALUE self) {
  VALUE editor, hint;
  rb_scan_args(argc, argv, "11", &editor, &hint);
  QAbstractItemDelegate *d = qcast<QAbstractItemDelegate>(self);
  QAbstractItemDelegate::EndEditHint h = NIL_P(hint)
      ? QAbstractItemDelegate::NoHint
      : (QAbstractItemDelegate::EndEditHint)NUM2INT(hint);
  emit d->closeEditor(qobject_cast<QWidget *>(get_obj(editor)), h);
  return self;
}

// ---- QStyle drawing path ---------------------------------------------------
// cmd_param_table_item_delegate.rb#paint draws a combo box as a button so the
// user can tell the cell is clickable, and table_manager.rb does the same.
// The option structs existed as empty classes with no storage and no
// constructor, so every one of those paints raised.
static VALUE cStyleOptionButtonKlass = Qnil;

static VALUE sovi_new(int argc, VALUE *argv, VALUE klass) {
  VALUE src;
  rb_scan_args(argc, argv, "01", &src);
  // Qt4's StyleOptionViewItemV4.new(option) copy constructor.
  if (!NIL_P(src) && rb_obj_is_kind_of(src, cStyleOptionViewItemKlass))
    return wrap_val<QStyleOptionViewItem>(klass, *get_val<QStyleOptionViewItem>(src));
  return wrap_val<QStyleOptionViewItem>(klass, QStyleOptionViewItem());
}
static VALUE sovi_rect(VALUE self) {
  return wrap_val<QRect>(cRect, get_val<QStyleOptionViewItem>(self)->rect);
}
static VALUE sovi_set_rect(VALUE self, VALUE r) {
  get_val<QStyleOptionViewItem>(self)->rect = *get_val<QRect>(r); return r;
}
static VALUE sovi_set_text(VALUE self, VALUE t) {
  get_val<QStyleOptionViewItem>(self)->text = rb_to_qs(t); return t;
}
static VALUE sovi_text(VALUE self) {
  return rb_str_new2(get_val<QStyleOptionViewItem>(self)->text.toUtf8().constData());
}
static VALUE sovi_set_features(VALUE self, VALUE f) {
  get_val<QStyleOptionViewItem>(self)->features =
      QStyleOptionViewItem::ViewItemFeatures(NUM2INT(f));
  return f;
}
static VALUE sob_new(int argc, VALUE *argv, VALUE klass) {
  rb_scan_args(argc, argv, "0");
  return wrap_val<QStyleOptionButton>(klass, QStyleOptionButton());
}
static VALUE sob_set_rect(VALUE self, VALUE r) {
  get_val<QStyleOptionButton>(self)->rect = *get_val<QRect>(r); return r;
}
static VALUE sob_rect(VALUE self) {
  return wrap_val<QRect>(cRect, get_val<QStyleOptionButton>(self)->rect);
}
static VALUE sob_set_text(VALUE self, VALUE t) {
  get_val<QStyleOptionButton>(self)->text = rb_to_qs(t); return t;
}
static VALUE style_draw_control(VALUE self, VALUE element, VALUE opt, VALUE painter) {
  QStyle *st = qcast<QStyle>(self);
  QPainter *p = painter_of(painter);
  QStyle::ControlElement ce = (QStyle::ControlElement)NUM2INT(element);
  if (rb_obj_is_kind_of(opt, cStyleOptionButtonKlass))
    st->drawControl(ce, get_val<QStyleOptionButton>(opt), p);
  else if (rb_obj_is_kind_of(opt, cStyleOptionViewItemKlass))
    st->drawControl(ce, get_val<QStyleOptionViewItem>(opt), p);
  else
    rb_raise(rb_eTypeError, "drawControl needs a Qt::StyleOption*, got %s",
             rb_obj_classname(opt));
  return self;
}
static VALUE widget_style(VALUE self) {
  return wrap_obj(rb_const_get(mQt, rb_intern("Style")),
                  qcast<QWidget>(self)->style(), false);
}
// QStyledItemDelegate::initStyleOption is protected.
struct DelegateAccess : public QStyledItemDelegate {
  using QStyledItemDelegate::initStyleOption;
};
static VALUE delegate_init_style_option(VALUE self, VALUE opt, VALUE idx) {
  QStyledItemDelegate *d = qcast<QStyledItemDelegate>(self);
  static_cast<DelegateAccess *>(d)->initStyleOption(
      get_val<QStyleOptionViewItem>(opt), *get_val<QModelIndex>(idx));
  return self;
}

// ---- bindings COSMOS calls that were never defined -------------------------
// Each of these raised NoMethodError the first time its control was used.

// QPlainTextEdit's block-geometry API is protected, and RubyEditor's
// line-number / breakpoint gutter needs it on every repaint
// (ruby_editor.rb:345-486). Exposing it changes no behaviour.
struct PlainTextAccess : public QPlainTextEdit {
  using QPlainTextEdit::firstVisibleBlock;
  using QPlainTextEdit::blockBoundingRect;
  using QPlainTextEdit::blockBoundingGeometry;
  using QPlainTextEdit::contentOffset;
};
static PlainTextAccess *pte_acc(VALUE self) {
  return static_cast<PlainTextAccess *>(qcast<QPlainTextEdit>(self));
}
static VALUE pte_first_visible_block(VALUE self) {
  return wrap_val<QTextBlock>(cTextBlockKlass, pte_acc(self)->firstVisibleBlock());
}
static VALUE pte_block_bounding_rect(VALUE self, VALUE b) {
  return wrap_val<QRect>(cRect,
      pte_acc(self)->blockBoundingRect(*get_val<QTextBlock>(b)).toRect());
}
static VALUE pte_block_bounding_geometry(VALUE self, VALUE b) {
  return wrap_val<QRect>(cRect,
      pte_acc(self)->blockBoundingGeometry(*get_val<QTextBlock>(b)).toRect());
}
static VALUE pte_content_offset(VALUE self) {
  return wrap_val<QPoint>(cPoint, pte_acc(self)->contentOffset().toPoint());
}
// ruby_editor.rb:199 -- also the only way to add a breakpoint (:203).
static VALUE pte_std_context_menu(VALUE self) {
  return wrap_obj(cMenu, qcast<QPlainTextEdit>(self)->createStandardContextMenu(), false);
}
static VALUE textblock_is_visible(VALUE self) {
  return get_val<QTextBlock>(self)->isVisible() ? Qtrue : Qfalse;
}
static VALUE rect_translated(VALUE self, VALUE pt) {
  return wrap_val<QRect>(cRect, get_val<QRect>(self)->translated(*get_val<QPoint>(pt)));
}
// Qt4's value types had explicit destructors and COSMOS still calls dispose on
// them (ruby_editor.rb:377). These are copies owned by Ruby's GC: no-op.
static VALUE value_dispose_noop(VALUE) { return Qnil; }

// qt.rb:354 walks the tree with topLevelItem (test_runner.rb:752, :862).
static VALUE treew_top_level_item(VALUE self, VALUE i) {
  QTreeWidgetItem *it = qcast<QTreeWidget>(self)->topLevelItem(NUM2INT(i));
  return it ? wrap_ptr<QTreeWidgetItem>(cTreeWidgetItem, it) : Qnil;
}
// Qt::ColorListWidget needs all three (qt.rb:598, 614, 632, 685).
static VALUE listw_set_uniform_item_sizes(VALUE self, VALUE v) {
  qcast<QListWidget>(self)->setUniformItemSizes(RTEST(v)); return self;
}
static VALUE listw_take_item(VALUE self, VALUE i) {
  QListWidgetItem *it = qcast<QListWidget>(self)->takeItem(NUM2INT(i));
  return it ? wrap_ptr<QListWidgetItem>(cListWidgetItem, it) : Qnil;
}
static VALUE listw_visual_item_rect(VALUE self, VALUE item) {
  return wrap_val<QRect>(cRect,
      qcast<QListWidget>(self)->visualItemRect(get_ptr<QListWidgetItem>(item)));
}
static VALUE fm_line_spacing(VALUE self) {
  return INT2NUM(get_val<QFontMetrics>(self)->lineSpacing());
}
// Cosmos::Widget is a module mixed into both widget and layout classes, so
// widget.rb:207's bare parentWidget also has to resolve on a QLayout.
static VALUE layout_parent_widget(VALUE self) {
  return wrap_obj(cWidget, qcast<QLayout>(self)->parentWidget(), false);
}
static VALUE tabw_set_tab_icon(VALUE self, VALUE i, VALUE icon) {
  qcast<QTabWidget>(self)->setTabIcon(NUM2INT(i), *get_val<QIcon>(icon));
  return self;
}

// The C initialize goes in an included module, not on the class: COSMOS
// reopens some of these classes with their own initialize and calls super.
static void item_init_module(VALUE klass, const char *name,
                             VALUE (*fn)(int, VALUE *, VALUE)) {
  VALUE m = rb_define_module_under(mQt, name);
  rb_define_method(m, "initialize", RUBY_METHOD_FUNC(fn), -1);
  rb_include_module(klass, m);
}

extern "C" void Init_qt6(void) {
  mQt = rb_define_module("Qt");
  rb_define_singleton_method(mQt, "connect_raw",  RUBY_METHOD_FUNC(qt_connect), -1);
  rb_define_singleton_method(mQt, "connect",      RUBY_METHOD_FUNC(qt_connect), -1);
  rb_define_singleton_method(mQt, "qVersion",     RUBY_METHOD_FUNC(qt_version), 0);
  rb_define_singleton_method(mQt, "single_shot",  RUBY_METHOD_FUNC(qt_single_shot), -1);
  rb_define_singleton_method(mQt, "post_to_main_thread", RUBY_METHOD_FUNC(qt_post_to_main), -1);
  rb_define_singleton_method(mQt, "on_main_thread?",     RUBY_METHOD_FUNC(qt_on_main_thread_p), 0);
  rb_define_singleton_method(mQt, "object_count", RUBY_METHOD_FUNC(obj_object_count), 0);

  g_procs = rb_ary_new();
  rb_gc_register_address(&g_procs);
  // ---- Qt::Base / Qt::Object ------------------------------------------
  // The hierarchy must match COSMOS's own line_graph C extension, which does
  //   cQtBase   = rb_define_class_under(mQt, "Base", rb_cObject);
  //   cQtWidget = rb_define_class_under(mQt, "Widget", cQtBase);
  // (ext/cosmos/ext/line_graph/line_graph.c:425). Qt::Widget must therefore
  // descend DIRECTLY from Qt::Base, so all shared behaviour lives on Qt::Base.
  cQtBase = rb_define_class_under(mQt, "Base", rb_cObject);
  // Define (not undefine) the allocator on the base so every Qt class -- and
  // every COSMOS subclass of one -- can be allocated. qt_initialize then
  // raises a clear error if no constructor is registered for the class.
  rb_define_alloc_func(cQtBase, qtobj_alloc);
  cQtObject = rb_define_class_under(mQt, "Object", cQtBase);
  // RubyFilterObject, not a bare QObject: qt.rb:648 builds Qt::Object.new and
  // gives it a singleton eventFilter, which needs a virtual to route through.
  register_ctor(cQtObject, ctor_plain<RubyFilterObject>);   // COSMOS subclasses Qt::Object directly
  QDEF(cQtBase, "initialize", RUBY_METHOD_FUNC(qt_initialize), -1);
  QDEF(cQtBase, "destroyed?", RUBY_METHOD_FUNC(obj_destroyed_p), 0);
  QDEF(cQtBase, "disposed?",  RUBY_METHOD_FUNC(obj_destroyed_p), 0);   // COSMOS spelling (progress_dialog.rb:162, +4)
  QDEF(cQtBase, "owned?",     RUBY_METHOD_FUNC(obj_owned_p), 0);
  QDEF(cQtBase, "destroy!",   RUBY_METHOD_FUNC(obj_destroy), 0);
  QDEF(cQtBase, "dispose",    RUBY_METHOD_FUNC(obj_destroy), 0);
  QDEF(cQtBase, "installEventFilter", RUBY_METHOD_FUNC(obj_install_event_filter), 1);
  QDEF(cQtBase, "removeEventFilter",  RUBY_METHOD_FUNC(obj_remove_event_filter), 1);
  QDEF(cQtBase, "setObjectName", RUBY_METHOD_FUNC(obj_set_object_name), 1);
  QDEF(cQtBase, "objectName",    RUBY_METHOD_FUNC(obj_object_name), 0);
  QDEF(cQtBase, "blockSignals",  RUBY_METHOD_FUNC(obj_block_signals), 1);
  QDEF(cQtBase, "parent",        RUBY_METHOD_FUNC(obj_parent), 0);
  QDEF(cQtBase, "findChildren",  RUBY_METHOD_FUNC(obj_find_children), -1);
  QDEF(cQtBase, "findChild",     RUBY_METHOD_FUNC(obj_find_child), 1);
  QDEF(cQtBase, "qtClassName",   RUBY_METHOD_FUNC(obj_qt_class_name), 0);

  // ---- Qt::Application -----------------------------------------------
  cApplication = rb_define_class_under(mQt, "Application", cQtObject);
  register_ctor(cApplication, ctor_application, false);
  QDEF(cApplication, "exec",          RUBY_METHOD_FUNC(app_exec), 0);
  QDEF(cApplication, "processEvents", RUBY_METHOD_FUNC(app_process_events), 0);
  QDEF(cApplication, "exec_for",      RUBY_METHOD_FUNC(app_exec_for), 1);
  rb_define_singleton_method(cApplication, "setOverrideCursor",     RUBY_METHOD_FUNC(app_set_override_cursor), 1);
  rb_define_singleton_method(cApplication, "restoreOverrideCursor", RUBY_METHOD_FUNC(app_restore_override_cursor), 0);
  rb_define_singleton_method(cApplication, "activeWindow",      RUBY_METHOD_FUNC(app_active_window), 0);
  rb_define_singleton_method(cApplication, "activeModalWidget", RUBY_METHOD_FUNC(app_active_modal), 0);
  rb_define_singleton_method(cApplication, "topLevelWidgets",   RUBY_METHOD_FUNC(app_top_level_widgets), 0);
  // COSMOS calls these on the instance too (qt_tool.rb:497 redirect_io).
  QDEF(cApplication, "activeWindow",      RUBY_METHOD_FUNC(app_active_window), 0);
  QDEF(cApplication, "activeModalWidget", RUBY_METHOD_FUNC(app_active_modal), 0);
  QDEF(cApplication, "topLevelWidgets",   RUBY_METHOD_FUNC(app_top_level_widgets), 0);
  QDEF(cApplication, "quit",              RUBY_METHOD_FUNC(app_quit), 0);
  QDEF(cApplication, "exit",              RUBY_METHOD_FUNC(app_exit), -1);
  QDEF(cApplication, "style",             RUBY_METHOD_FUNC(app_style), 0);
  QDEF(cApplication, "setOverrideCursor",     RUBY_METHOD_FUNC(app_set_override_cursor), 1);
  QDEF(cApplication, "restoreOverrideCursor", RUBY_METHOD_FUNC(app_restore_override_cursor), 0);
  rb_define_singleton_method(cApplication, "quit",            RUBY_METHOD_FUNC(app_quit), 0);
  rb_define_singleton_method(cApplication, "closeAllWindows", RUBY_METHOD_FUNC(app_close_all_windows), 0);
  rb_define_singleton_method(cApplication, "processEvents",   RUBY_METHOD_FUNC(app_process_events_cls), 0);
  rb_define_singleton_method(cApplication, "processEvents",  RUBY_METHOD_FUNC(app_process_events_cls), 0);
  rb_define_singleton_method(cApplication, "instance",       RUBY_METHOD_FUNC(app_instance), 0);
  rb_define_singleton_method(cApplication, "desktop",        RUBY_METHOD_FUNC(app_desktop), 0);
  QDEF(cApplication, "desktop",  RUBY_METHOD_FUNC(app_desktop), 0);   // also as an instance method (script_runner.rb)
  rb_define_singleton_method(cApplication, "style",          RUBY_METHOD_FUNC(app_style), 0);
  rb_define_singleton_method(cApplication, "setWindowIcon",  RUBY_METHOD_FUNC(app_set_window_icon), 1);
  QDEF(cApplication, "setWindowIcon",   RUBY_METHOD_FUNC(widget_set_window_icon), 1);
  QDEF(cApplication, "addLibraryPath",  RUBY_METHOD_FUNC(app_add_library_path), 1);
  QDEF(cApplication, "closeAllWindows", RUBY_METHOD_FUNC(app_close_all_windows), 0);
  // ---- Qt::Widget ----------------------------------------------------
  cWidget = rb_define_class_under(mQt, "Widget", cQtBase);
  register_ctor(cWidget, ctor_plain<RubyWidget>);   // virtual dispatch into Ruby
  QDEF(cWidget, "show",           RUBY_METHOD_FUNC((call_void<QWidget, &QWidget::show>)), 0);
  QDEF(cWidget, "close",          RUBY_METHOD_FUNC(widget_close), 0);
  // Every virtual RubyWidget/RubyMainWindow/RubyDialog forwards to Ruby needs
  // a bound counterpart so `super` inside the override runs Qt's default.
  QDEF(cWidget, "closeEvent",        RUBY_METHOD_FUNC(qt_base_event), -1);
  QDEF(cWidget, "paintEvent",        RUBY_METHOD_FUNC(qt_base_event), -1);
  QDEF(cWidget, "resizeEvent",       RUBY_METHOD_FUNC(qt_base_event), -1);
  QDEF(cWidget, "showEvent",         RUBY_METHOD_FUNC(qt_base_event), -1);
  QDEF(cWidget, "wheelEvent",        RUBY_METHOD_FUNC(qt_base_event), -1);
  QDEF(cWidget, "leaveEvent",        RUBY_METHOD_FUNC(qt_base_event), -1);
  QDEF(cWidget, "focusInEvent",      RUBY_METHOD_FUNC(qt_base_event), -1);
  QDEF(cWidget, "focusOutEvent",     RUBY_METHOD_FUNC(qt_base_event), -1);
  QDEF(cWidget, "keyPressEvent",     RUBY_METHOD_FUNC(qt_base_event), -1);
  QDEF(cWidget, "mousePressEvent",   RUBY_METHOD_FUNC(qt_base_event), -1);
  QDEF(cWidget, "mouseMoveEvent",    RUBY_METHOD_FUNC(qt_base_event), -1);
  QDEF(cWidget, "mouseReleaseEvent", RUBY_METHOD_FUNC(qt_base_event), -1);
  QDEF(cWidget, "insertAction",   RUBY_METHOD_FUNC(widget_insert_action), 2);
  QDEF(cWidget, "adjustSize",     RUBY_METHOD_FUNC((call_void<QWidget, &QWidget::adjustSize>)), 0);
  QDEF(cWidget, "clearFocus",     RUBY_METHOD_FUNC(widget_clear_focus), 0);
  QDEF(cWidget, "scroll",         RUBY_METHOD_FUNC(widget_scroll), 2);
  QDEF(cWidget, "actions",        RUBY_METHOD_FUNC(widget_actions), 0);
  QDEF(cWidget, "showNormal",     RUBY_METHOD_FUNC((call_void<QWidget, &QWidget::showNormal>)), 0);
  QDEF(cWidget, "hasFocus",       RUBY_METHOD_FUNC((get_bool<QWidget, &QWidget::hasFocus>)), 0);
  QDEF(cWidget, "minimumHeight",  RUBY_METHOD_FUNC((get_int<QWidget, &QWidget::minimumHeight>)), 0);
  QDEF(cWidget, "maximumHeight",  RUBY_METHOD_FUNC((get_int<QWidget, &QWidget::maximumHeight>)), 0);
  QDEF(cWidget, "minimumWidth",   RUBY_METHOD_FUNC((get_int<QWidget, &QWidget::minimumWidth>)), 0);
  QDEF(cWidget, "maximumWidth",   RUBY_METHOD_FUNC((get_int<QWidget, &QWidget::maximumWidth>)), 0);
  QDEF(cWidget, "setGeometry",    RUBY_METHOD_FUNC(widget_set_geometry), -1);
  QDEF(cWidget, "contentsRect",   RUBY_METHOD_FUNC(widget_contents_rect), 0);
  QDEF(cWidget, "rect",           RUBY_METHOD_FUNC(widget_rect), 0);
  QDEF(cWidget, "setParent",      RUBY_METHOD_FUNC(widget_set_parent), -1);
  QDEF(cWidget, "windowFlags",    RUBY_METHOD_FUNC(widget_window_flags), 0);
  QDEF(cWidget, "hide",           RUBY_METHOD_FUNC((call_void<QWidget, &QWidget::hide>)), 0);
  QDEF(cWidget, "resize",         RUBY_METHOD_FUNC(widget_resize_v), -1);
  QDEF(cWidget, "move",           RUBY_METHOD_FUNC(widget_move), -1);
  QDEF(cWidget, "setLayout",      RUBY_METHOD_FUNC(widget_set_layout), 1);
  QDEF(cWidget, "setWindowTitle", RUBY_METHOD_FUNC((set_str<QWidget, &QWidget::setWindowTitle>)), 1);
  QDEF(cWidget, "windowTitle",    RUBY_METHOD_FUNC((get_str<QWidget, &QWidget::windowTitle>)), 0);
  QDEF(cWidget, "setEnabled",     RUBY_METHOD_FUNC((set_bool<QWidget, &QWidget::setEnabled>)), 1);
  QDEF(cWidget, "isEnabled",      RUBY_METHOD_FUNC((get_bool<QWidget, &QWidget::isEnabled>)), 0);
  QDEF(cWidget, "setVisible",     RUBY_METHOD_FUNC((set_bool<QWidget, &QWidget::setVisible>)), 1);
  QDEF(cWidget, "isVisible",      RUBY_METHOD_FUNC((get_bool<QWidget, &QWidget::isVisible>)), 0);
  QDEF(cWidget, "setToolTip",     RUBY_METHOD_FUNC((set_str<QWidget, &QWidget::setToolTip>)), 1);
  QDEF(cWidget, "toolTip",        RUBY_METHOD_FUNC((get_str<QWidget, &QWidget::toolTip>)), 0);
  QDEF(cWidget, "setStyleSheet",  RUBY_METHOD_FUNC((set_str<QWidget, &QWidget::setStyleSheet>)), 1);
  QDEF(cWidget, "styleSheet",     RUBY_METHOD_FUNC((get_str<QWidget, &QWidget::styleSheet>)), 0);

  // ---- Qt::Frame / Qt::Label ------------------------------------------
  cFrame = rb_define_class_under(mQt, "Frame", cWidget);
  register_ctor(cFrame, ctor_plain<RubyForward<QFrame> >);
  QDEF(cFrame, "setFrameStyle",    RUBY_METHOD_FUNC(frame_set_style), 1);
  QDEF(cFrame, "setLineWidth",     RUBY_METHOD_FUNC(frame_set_line_width), 1);
  QDEF(cFrame, "setMidLineWidth",  RUBY_METHOD_FUNC(frame_set_mid_line_width), 1);
  QDEF(cFrame, "frameWidth",       RUBY_METHOD_FUNC(frame_frame_width), 0);
#define DEF_FR(n) rb_define_const(cFrame, #n, INT2NUM((int)QFrame::n))
  DEF_FR(NoFrame); DEF_FR(Box); DEF_FR(Panel); DEF_FR(StyledPanel);
  DEF_FR(Plain); DEF_FR(Raised); DEF_FR(Sunken);
  DEF_FR(HLine); DEF_FR(VLine); DEF_FR(WinPanel);
#undef DEF_FR

  // Real Qt chain: QAbstractScrollArea < QFrame, and the item views and text
  // edits sit under it -- that is where setFrameStyle/setLineWidth come from.
  cAbstractScrollArea = rb_define_class_under(mQt, "AbstractScrollArea", cFrame);
  cAbstractItemView   = rb_define_class_under(mQt, "AbstractItemView", cAbstractScrollArea);

  cLabel = rb_define_class_under(mQt, "Label", cFrame);
  register_ctor(cLabel, ctor_str<RubyForward<QLabel> >);
  QDEF(cLabel, "setText", RUBY_METHOD_FUNC((set_str<QLabel, &QLabel::setText>)), 1);
  QDEF(cLabel, "text",    RUBY_METHOD_FUNC((get_str<QLabel, &QLabel::text>)), 0);
  QDEF(cLabel, "setAlignment", RUBY_METHOD_FUNC(label_set_alignment), 1);
  QDEF(cLabel, "wordWrap", RUBY_METHOD_FUNC(label_word_wrap), 0);
  QDEF(cLabel, "setWordWrap",  RUBY_METHOD_FUNC((set_bool<QLabel, &QLabel::setWordWrap>)), 1);
  QDEF(cLabel, "setBuddy",      RUBY_METHOD_FUNC(label_set_buddy), 1);
  QDEF(cLabel, "setPixmap",     RUBY_METHOD_FUNC(label_set_pixmap), 1);
  QDEF(cLabel, "setTextFormat", RUBY_METHOD_FUNC(label_set_text_format), 1);

  // ---- buttons --------------------------------------------------------
  cAbstractButton = rb_define_class_under(mQt, "AbstractButton", cWidget);
  QDEF(cAbstractButton, "setText",     RUBY_METHOD_FUNC((set_str<QAbstractButton, &QAbstractButton::setText>)), 1);
  QDEF(cAbstractButton, "text",        RUBY_METHOD_FUNC((get_str<QAbstractButton, &QAbstractButton::text>)), 0);
  QDEF(cAbstractButton, "setChecked",  RUBY_METHOD_FUNC((set_bool<QAbstractButton, &QAbstractButton::setChecked>)), 1);
  QDEF(cAbstractButton, "isChecked",   RUBY_METHOD_FUNC((get_bool<QAbstractButton, &QAbstractButton::isChecked>)), 0);
  QDEF(cAbstractButton, "setCheckable",RUBY_METHOD_FUNC((set_bool<QAbstractButton, &QAbstractButton::setCheckable>)), 1);
  QDEF(cAbstractButton, "isCheckable", RUBY_METHOD_FUNC((get_bool<QAbstractButton, &QAbstractButton::isCheckable>)), 0);
  QDEF(cAbstractButton, "click",       RUBY_METHOD_FUNC(button_click), 0);
  QDEF(cAbstractButton, "setIcon",     RUBY_METHOD_FUNC(button_set_icon), 1);
  QDEF(cAbstractButton, "setIconSize", RUBY_METHOD_FUNC(widget_set_icon_size), 1);

  cPushButton = rb_define_class_under(mQt, "PushButton", cAbstractButton);
  register_ctor(cPushButton, ctor_str<QPushButton>);
  QDEF(cPushButton, "setDefault", RUBY_METHOD_FUNC(button_set_default), 1);

  cCheckBox = rb_define_class_under(mQt, "CheckBox", cAbstractButton);
  register_ctor(cCheckBox, ctor_str<QCheckBox>);
  QDEF(cCheckBox, "setCheckState", RUBY_METHOD_FUNC(cbx_set_check_state), 1);
  QDEF(cCheckBox, "checkState",    RUBY_METHOD_FUNC(cbx_check_state), 0);

  // ---- inputs ---------------------------------------------------------
  cLineEdit = rb_define_class_under(mQt, "LineEdit", cWidget);
  register_ctor(cLineEdit, ctor_str<RubyForward<QLineEdit> >);
  QDEF(cLineEdit, "paste",     RUBY_METHOD_FUNC(edit_paste), 0);
  QDEF(cLineEdit, "copy",      RUBY_METHOD_FUNC(edit_copy), 0);
  QDEF(cLineEdit, "cut",       RUBY_METHOD_FUNC(edit_cut), 0);
  QDEF(cLineEdit, "selectAll", RUBY_METHOD_FUNC(edit_select_all), 0);
  QDEF(cLineEdit, "setText",     RUBY_METHOD_FUNC((set_str<QLineEdit, &QLineEdit::setText>)), 1);
  QDEF(cLineEdit, "text",        RUBY_METHOD_FUNC((get_str<QLineEdit, &QLineEdit::text>)), 0);
  QDEF(cLineEdit, "setReadOnly", RUBY_METHOD_FUNC((set_bool<QLineEdit, &QLineEdit::setReadOnly>)), 1);
  QDEF(cLineEdit, "isReadOnly",  RUBY_METHOD_FUNC((get_bool<QLineEdit, &QLineEdit::isReadOnly>)), 0);
  QDEF(cLineEdit, "setMaxLength",RUBY_METHOD_FUNC((set_int<QLineEdit, &QLineEdit::setMaxLength>)), 1);
  QDEF(cLineEdit, "maxLength",   RUBY_METHOD_FUNC((get_int<QLineEdit, &QLineEdit::maxLength>)), 0);
  QDEF(cLineEdit, "clear",       RUBY_METHOD_FUNC((call_void<QLineEdit, &QLineEdit::clear>)), 0);

  cComboBox = rb_define_class_under(mQt, "ComboBox", cWidget);
  register_ctor(cComboBox, ctor_plain<RubyForward<QComboBox> >);
  QDEF(cComboBox, "addItem",         RUBY_METHOD_FUNC(combo_add_item), -1);
  QDEF(cComboBox, "insertItem",      RUBY_METHOD_FUNC(combo_insert_item), -1);
  QDEF(cComboBox, "itemText",        RUBY_METHOD_FUNC(combo_item_text), 1);
  QDEF(cComboBox, "setCurrentIndex", RUBY_METHOD_FUNC((set_int<QComboBox, &QComboBox::setCurrentIndex>)), 1);
  QDEF(cComboBox, "currentIndex",    RUBY_METHOD_FUNC((get_int<QComboBox, &QComboBox::currentIndex>)), 0);
  QDEF(cComboBox, "currentText",     RUBY_METHOD_FUNC((get_str<QComboBox, &QComboBox::currentText>)), 0);
  QDEF(cComboBox, "count",           RUBY_METHOD_FUNC((get_int<QComboBox, &QComboBox::count>)), 0);
  QDEF(cComboBox, "itemData",    RUBY_METHOD_FUNC(cb_item_data), 1);
  QDEF(cComboBox, "setItemData", RUBY_METHOD_FUNC(cb_set_item_data), 2);
  QDEF(cComboBox, "removeItem",      RUBY_METHOD_FUNC(cb_remove_item), 1);
  QDEF(cComboBox, "maxCount",        RUBY_METHOD_FUNC(cb_max_count), 0);
  QDEF(cComboBox, "maxVisibleItems", RUBY_METHOD_FUNC(cb_max_visible), 0);
  QDEF(cComboBox, "clear",           RUBY_METHOD_FUNC((call_void<QComboBox, &QComboBox::clear>)), 0);
  QDEF(cComboBox, "setMaxVisibleItems", RUBY_METHOD_FUNC((set_int<QComboBox, &QComboBox::setMaxVisibleItems>)), 1);
  QDEF(cComboBox, "setEditable",     RUBY_METHOD_FUNC((set_bool<QComboBox, &QComboBox::setEditable>)), 1);
  QDEF(cComboBox, "setCurrentText",  RUBY_METHOD_FUNC(combo_set_current_text), 1);
  QDEF(cComboBox, "addItems",        RUBY_METHOD_FUNC(combo_add_items), 1);
  QDEF(cComboBox, "setSizeAdjustPolicy", RUBY_METHOD_FUNC(combo_set_size_adjust), 1);

  cPlainTextEdit = rb_define_class_under(mQt, "PlainTextEdit", cAbstractScrollArea);
  register_ctor(cPlainTextEdit, ctor_str<RubyForward<QPlainTextEdit> >);
  QDEF(cPlainTextEdit, "firstVisibleBlock",        RUBY_METHOD_FUNC(pte_first_visible_block), 0);
  QDEF(cPlainTextEdit, "blockBoundingRect",        RUBY_METHOD_FUNC(pte_block_bounding_rect), 1);
  QDEF(cPlainTextEdit, "blockBoundingGeometry",    RUBY_METHOD_FUNC(pte_block_bounding_geometry), 1);
  QDEF(cPlainTextEdit, "contentOffset",            RUBY_METHOD_FUNC(pte_content_offset), 0);
  QDEF(cPlainTextEdit, "createStandardContextMenu", RUBY_METHOD_FUNC(pte_std_context_menu), 0);
  QDEF(cPlainTextEdit, "currentCharFormat",    RUBY_METHOD_FUNC(edit_current_char_format), 0);
  QDEF(cPlainTextEdit, "moveCursor",  RUBY_METHOD_FUNC(edit_move_cursor), -1);
  QDEF(cPlainTextEdit, "paste",       RUBY_METHOD_FUNC(edit_paste), 0);
  QDEF(cPlainTextEdit, "find",        RUBY_METHOD_FUNC(edit_find), -1);
  QDEF(cPlainTextEdit, "copy",        RUBY_METHOD_FUNC(edit_copy), 0);
  QDEF(cPlainTextEdit, "cut",         RUBY_METHOD_FUNC(edit_cut), 0);
  QDEF(cPlainTextEdit, "cursorRect",  RUBY_METHOD_FUNC(edit_cursor_rect), 0);
  QDEF(cPlainTextEdit, "selectAll",   RUBY_METHOD_FUNC(edit_select_all), 0);
  QDEF(cPlainTextEdit, "setCurrentCharFormat", RUBY_METHOD_FUNC(edit_set_current_char_format), 1);
  QDEF(cPlainTextEdit, "setPlainText", RUBY_METHOD_FUNC((set_str<QPlainTextEdit, &QPlainTextEdit::setPlainText>)), 1);
  QDEF(cPlainTextEdit, "toPlainText",  RUBY_METHOD_FUNC((get_str<QPlainTextEdit, &QPlainTextEdit::toPlainText>)), 0);
  QDEF(cPlainTextEdit, "setReadOnly",  RUBY_METHOD_FUNC((set_bool<QPlainTextEdit, &QPlainTextEdit::setReadOnly>)), 1);
  QDEF(cPlainTextEdit, "isReadOnly",   RUBY_METHOD_FUNC((get_bool<QPlainTextEdit, &QPlainTextEdit::isReadOnly>)), 0);
  QDEF(cPlainTextEdit, "clear",           RUBY_METHOD_FUNC((call_void<QPlainTextEdit, &QPlainTextEdit::clear>)), 0);
  QDEF(cPlainTextEdit, "appendPlainText", RUBY_METHOD_FUNC(pte_append), 1);
  QDEF(cPlainTextEdit, "appendHtml",      RUBY_METHOD_FUNC((set_str<QPlainTextEdit, &QPlainTextEdit::appendHtml>)), 1);
  QDEF(cPlainTextEdit, "centerCursor",    RUBY_METHOD_FUNC((call_void<QPlainTextEdit, &QPlainTextEdit::centerCursor>)), 0);

  cGroupBox = rb_define_class_under(mQt, "GroupBox", cWidget);
  register_ctor(cGroupBox, ctor_str<QGroupBox>);
  QDEF(cGroupBox, "setTitle",    RUBY_METHOD_FUNC((set_str<QGroupBox, &QGroupBox::setTitle>)), 1);
  QDEF(cGroupBox, "title",       RUBY_METHOD_FUNC((get_str<QGroupBox, &QGroupBox::title>)), 0);
  QDEF(cGroupBox, "setCheckable",RUBY_METHOD_FUNC((set_bool<QGroupBox, &QGroupBox::setCheckable>)), 1);
  QDEF(cGroupBox, "isCheckable", RUBY_METHOD_FUNC((get_bool<QGroupBox, &QGroupBox::isCheckable>)), 0);

  // ---- windows --------------------------------------------------------
  cDialog = rb_define_class_under(mQt, "Dialog", cWidget);
  register_ctor(cDialog, ctor_plain<RubyDialog>);   // virtuals -> Ruby (closeEvent)
  // QDialog::exec() -- without this, Ruby's Kernel#exec shadows it.
  QDEF(cDialog, "exec",   RUBY_METHOD_FUNC(dialog_exec), 0);
  QDEF(cDialog, "accept", RUBY_METHOD_FUNC((call_void<QDialog, &QDialog::accept>)), 0);
  QDEF(cDialog, "reject", RUBY_METHOD_FUNC((call_void<QDialog, &QDialog::reject>)), 0);

  cMainWindow = rb_define_class_under(mQt, "MainWindow", cWidget);
  QDEF(cMainWindow, "addToolBar", RUBY_METHOD_FUNC(mw_add_toolbar), -1);
  register_ctor(cMainWindow, ctor_plain<RubyMainWindow>);   // virtuals -> Ruby (closeEvent)
  QDEF(cMainWindow, "setCentralWidget", RUBY_METHOD_FUNC(mainwindow_set_central), 1);
  // ---- layouts --------------------------------------------------------
  cLayout = rb_define_class_under(mQt, "Layout", cQtObject);
  QDEF(cLayout, "addWidget", RUBY_METHOD_FUNC(layout_add_widget), -1);
  QDEF(cLayout, "count",     RUBY_METHOD_FUNC(layout_count), 0);

  QDEF(cLayout, "takeAt",             RUBY_METHOD_FUNC(layout_take_at), 1);
  QDEF(cLayout, "setSpacing",         RUBY_METHOD_FUNC(layout_set_spacing), 1);
  QDEF(cLayout, "setContentsMargins", RUBY_METHOD_FUNC(layout_set_margins), 4);
  QDEF(cLayout, "setAlignment",       RUBY_METHOD_FUNC(layout_set_alignment), -1);
  QDEF(cLayout, "removeItem",  RUBY_METHOD_FUNC(layout_remove_item), 1);
  QDEF(cLayout, "setMargin",   RUBY_METHOD_FUNC(layout_set_margin), 1);
  QDEF(cLayout, "parentWidget", RUBY_METHOD_FUNC(layout_parent_widget), 0);
  QDEF(cLayout, "itemAt",             RUBY_METHOD_FUNC(layout_item_at), -1);
  QDEF(cLayout, "setSizeConstraint",  RUBY_METHOD_FUNC(layout_set_size_constraint), 1);
#define DEF_SC(n) rb_define_const(cLayout, #n, INT2NUM((int)QLayout::n))
  DEF_SC(SetDefaultConstraint); DEF_SC(SetNoConstraint); DEF_SC(SetMinimumSize);
  DEF_SC(SetFixedSize); DEF_SC(SetMaximumSize); DEF_SC(SetMinAndMaxSize);
#undef DEF_SC

  cLayoutItem = rb_define_class_under(mQt, "LayoutItem", rb_cObject);
  QDEF(cLayoutItem, "widget", RUBY_METHOD_FUNC(layoutitem_widget), 0);
  QDEF(cLayoutItem, "layout", RUBY_METHOD_FUNC(layoutitem_layout), 0);

  cBoxLayout = rb_define_class_under(mQt, "BoxLayout", cLayout);
  register_ctor(cBoxLayout, ctor_box_layout);
  QDEF(cBoxLayout, "insertLayout", RUBY_METHOD_FUNC(boxlayout_insert_layout), -1);
  QDEF(cBoxLayout, "addLayout",    RUBY_METHOD_FUNC(layout_add_layout), -1);
  QDEF(cBoxLayout, "addStretch",   RUBY_METHOD_FUNC(box_add_stretch), -1);
  QDEF(cBoxLayout, "addSpacing",   RUBY_METHOD_FUNC(box_add_spacing), 1);
  QDEF(cBoxLayout, "insertWidget", RUBY_METHOD_FUNC(box_insert_widget), -1);

  cVBoxLayout = rb_define_class_under(mQt, "VBoxLayout", cBoxLayout);
  register_ctor(cVBoxLayout, ctor_layout<QVBoxLayout>);
  cHBoxLayout = rb_define_class_under(mQt, "HBoxLayout", cBoxLayout);
  register_ctor(cHBoxLayout, ctor_layout<QHBoxLayout>);
  cStackedLayout = rb_define_class_under(mQt, "StackedLayout", cLayout);
  register_ctor(cStackedLayout, ctor_plain<QStackedLayout>);

  cGridLayout = rb_define_class_under(mQt, "GridLayout", cLayout);
  register_ctor(cGridLayout, ctor_layout<QGridLayout>);
  QDEF(cGridLayout, "addWidget",         RUBY_METHOD_FUNC(grid_add_widget), -1);
  QDEF(cGridLayout, "setRowStretch",     RUBY_METHOD_FUNC(grid_set_row_stretch), 2);
  QDEF(cGridLayout, "setColumnStretch",  RUBY_METHOD_FUNC(grid_set_col_stretch), 2);
  // ---- enum constants --------------------------------------------------
  // Plain integer constants; COSMOS uses these as bare Qt::Foo values.
#define DEF_QT_CONST(name) rb_define_const(mQt, #name, INT2NUM((int)Qt::name))
  // alignment
  DEF_QT_CONST(AlignLeft);   DEF_QT_CONST(AlignRight);  DEF_QT_CONST(AlignHCenter);
  DEF_QT_CONST(AlignTop);    DEF_QT_CONST(AlignBottom); DEF_QT_CONST(AlignVCenter);
  DEF_QT_CONST(AlignCenter);
  // orientation
  DEF_QT_CONST(Horizontal);  DEF_QT_CONST(Vertical);
  // check state
  DEF_QT_CONST(Checked);     DEF_QT_CONST(Unchecked);   DEF_QT_CONST(PartiallyChecked);
  // item flags
  DEF_QT_CONST(NoItemFlags);        DEF_QT_CONST(ItemIsSelectable);
  DEF_QT_CONST(ItemIsEditable);     DEF_QT_CONST(ItemIsEnabled);
  DEF_QT_CONST(ItemIsUserCheckable);
  // window flags
  DEF_QT_CONST(WindowTitleHint);    DEF_QT_CONST(WindowSystemMenuHint);
  DEF_QT_CONST(CustomizeWindowHint);DEF_QT_CONST(WindowStaysOnTopHint);
  DEF_QT_CONST(MSWindowsFixedSizeDialogHint);
  // context menu / focus
  DEF_QT_CONST(CustomContextMenu);  DEF_QT_CONST(PreventContextMenu);
  DEF_QT_CONST(StrongFocus);        DEF_QT_CONST(OtherFocusReason);
  DEF_QT_CONST(PopupFocusReason);
  // item data roles
  DEF_QT_CONST(DecorationRole);     DEF_QT_CONST(EditRole);   DEF_QT_CONST(UserRole);
  // mouse buttons and modifiers
  DEF_QT_CONST(LeftButton);         DEF_QT_CONST(RightButton);
  DEF_QT_CONST(ControlModifier);    DEF_QT_CONST(NoModifier);
  // cursors
  DEF_QT_CONST(ArrowCursor);   DEF_QT_CONST(WaitCursor);      DEF_QT_CONST(CrossCursor);
  DEF_QT_CONST(PointingHandCursor); DEF_QT_CONST(OpenHandCursor);
  DEF_QT_CONST(ClosedHandCursor);   DEF_QT_CONST(SizeAllCursor);
  DEF_QT_CONST(SizeHorCursor);      DEF_QT_CONST(SizeVerCursor);
  // pens / brushes / text
  DEF_QT_CONST(NoPen);   DEF_QT_CONST(DashLine);  DEF_QT_CONST(SolidPattern);
  DEF_QT_CONST(NoBrush); DEF_QT_CONST(RichText);  DEF_QT_CONST(CaseInsensitive);
  DEF_QT_CONST(MatchExactly); DEF_QT_CONST(MoveAction);
  DEF_QT_CONST(TextSelectableByMouse);
  // keys
  DEF_QT_CONST(Key_Escape); DEF_QT_CONST(Key_Return); DEF_QT_CONST(Key_Enter);
  DEF_QT_CONST(Key_Delete); DEF_QT_CONST(Key_Backspace); DEF_QT_CONST(Key_Tab);
  DEF_QT_CONST(Key_Backtab); DEF_QT_CONST(Key_Up); DEF_QT_CONST(Key_Down);
  DEF_QT_CONST(Key_Left); DEF_QT_CONST(Key_Right);
#undef DEF_QT_CONST

  // Qt4 -> Qt6 compatibility aliases. Absorbing these here means COSMOS's own
  // source does not have to change for them (see QT6_PORT_ASSESSMENT.md section 6).
  rb_define_singleton_method(mQt, "black",     RUBY_METHOD_FUNC((global_color<(int)Qt::black>)), 0);
  rb_define_singleton_method(mQt, "white",     RUBY_METHOD_FUNC((global_color<(int)Qt::white>)), 0);
  rb_define_singleton_method(mQt, "red",       RUBY_METHOD_FUNC((global_color<(int)Qt::red>)), 0);
  rb_define_singleton_method(mQt, "lightGray", RUBY_METHOD_FUNC((global_color<(int)Qt::lightGray>)), 0);

  rb_define_const(mQt, "MidButton", INT2NUM((int)Qt::MiddleButton));  // renamed in Qt6
  rb_define_const(mQt, "MiddleButton", INT2NUM((int)Qt::MiddleButton));
  // ---- Qt::Action (QtGui in Qt6) --------------------------------------
  cAction = rb_define_class_under(mQt, "Action", cQtObject);
  register_ctor(cAction, ctor_action);
  QDEF(cAction, "setText",     RUBY_METHOD_FUNC((set_str<QAction, &QAction::setText>)), 1);
  QDEF(cAction, "text",        RUBY_METHOD_FUNC((get_str<QAction, &QAction::text>)), 0);
  QDEF(cAction, "setEnabled",  RUBY_METHOD_FUNC((set_bool<QAction, &QAction::setEnabled>)), 1);
  QDEF(cAction, "isEnabled",   RUBY_METHOD_FUNC((get_bool<QAction, &QAction::isEnabled>)), 0);
  QDEF(cAction, "setCheckable",RUBY_METHOD_FUNC((set_bool<QAction, &QAction::setCheckable>)), 1);
  QDEF(cAction, "isCheckable", RUBY_METHOD_FUNC((get_bool<QAction, &QAction::isCheckable>)), 0);
  QDEF(cAction, "setChecked",  RUBY_METHOD_FUNC((set_bool<QAction, &QAction::setChecked>)), 1);
  QDEF(cAction, "isChecked",   RUBY_METHOD_FUNC((get_bool<QAction, &QAction::isChecked>)), 0);
  QDEF(cAction, "trigger",     RUBY_METHOD_FUNC((call_void<QAction, &QAction::trigger>)), 0);
  QDEF(cAction, "setShortcut", RUBY_METHOD_FUNC(action_set_shortcut), 1);
  QDEF(cAction, "setIcon",       RUBY_METHOD_FUNC(button_set_icon), 1);
  QDEF(cAction, "setStatusTip",  RUBY_METHOD_FUNC((set_str<QAction, &QAction::setStatusTip>)), 1);
  QDEF(cAction, "statusTip",     RUBY_METHOD_FUNC((get_str<QAction, &QAction::statusTip>)), 0);
  QDEF(cAction, "setToolTip",    RUBY_METHOD_FUNC((set_str<QAction, &QAction::setToolTip>)), 1);
  QDEF(cAction, "toolTip",       RUBY_METHOD_FUNC((get_str<QAction, &QAction::toolTip>)), 0);
  QDEF(cAction, "setWhatsThis",  RUBY_METHOD_FUNC((set_str<QAction, &QAction::setWhatsThis>)), 1);
  QDEF(cAction, "setVisible",    RUBY_METHOD_FUNC((set_bool<QAction, &QAction::setVisible>)), 1);
  QDEF(cAction, "isVisible",     RUBY_METHOD_FUNC((get_bool<QAction, &QAction::isVisible>)), 0);
  QDEF(cAction, "setSeparator",  RUBY_METHOD_FUNC((set_bool<QAction, &QAction::setSeparator>)), 1);
  QDEF(cAction, "shortcut",    RUBY_METHOD_FUNC(action_shortcut), 0);
  // ---- value types -----------------------------------------------------
  cKeySequence = rb_define_class_under(mQt, "KeySequence", rb_cObject);
  rb_define_singleton_method(cKeySequence, "new", RUBY_METHOD_FUNC(keyseq_new), -1);
  QDEF(cKeySequence, "toString", RUBY_METHOD_FUNC(keyseq_to_s), 0);
  QDEF(cKeySequence, "to_s",     RUBY_METHOD_FUNC(keyseq_to_s), 0);
  QDEF(cKeySequence, "isEmpty",  RUBY_METHOD_FUNC(keyseq_is_empty), 0);

  cVariant = rb_define_class_under(mQt, "Variant", rb_cObject);
  rb_define_singleton_method(cVariant, "new",       RUBY_METHOD_FUNC(variant_new), -1);
  rb_define_singleton_method(cVariant, "fromValue", RUBY_METHOD_FUNC(variant_from_value), 1);
  QDEF(cVariant, "toStringList", RUBY_METHOD_FUNC(variant_to_string_list), 0);
  QDEF(cVariant, "value",        RUBY_METHOD_FUNC(variant_value), 0);
  QDEF(cVariant, "toString", RUBY_METHOD_FUNC(variant_to_s), 0);
  QDEF(cVariant, "to_s",     RUBY_METHOD_FUNC(variant_to_s), 0);
  QDEF(cVariant, "toInt",    RUBY_METHOD_FUNC(variant_to_i), 0);
  QDEF(cVariant, "toDouble", RUBY_METHOD_FUNC(variant_to_f), 0);
  QDEF(cVariant, "toBool",   RUBY_METHOD_FUNC(variant_to_b), 0);
  QDEF(cVariant, "isValid",  RUBY_METHOD_FUNC(variant_valid), 0);
  QDEF(cVariant, "toSize",   RUBY_METHOD_FUNC(variant_to_size), 0);
  QDEF(cVariant, "toPoint",  RUBY_METHOD_FUNC(variant_to_point), 0);

  cFont = rb_define_class_under(mQt, "Font", rb_cObject);
  rb_define_singleton_method(cFont, "new", RUBY_METHOD_FUNC(font_new), -1);
  QDEF(cFont, "family",    RUBY_METHOD_FUNC(font_family), 0);
  QDEF(cFont, "setFamily",    RUBY_METHOD_FUNC(font_set_family), 1);
  QDEF(cFont, "setPointSize", RUBY_METHOD_FUNC(font_set_point_size), 1);
  QDEF(cFont, "setItalic",    RUBY_METHOD_FUNC(font_set_italic), 1);
  QDEF(cFont, "pointSize", RUBY_METHOD_FUNC(font_point_size), 0);
  QDEF(cFont, "setBold",   RUBY_METHOD_FUNC(font_set_bold), 1);
  QDEF(cFont, "bold",      RUBY_METHOD_FUNC(font_bold), 0);

  cColor = rb_define_class_under(mQt, "Color", rb_cObject);
  rb_define_singleton_method(cColor, "new", RUBY_METHOD_FUNC(color_new), -1);
  QDEF(cColor, "red",   RUBY_METHOD_FUNC(color_red), 0);
  QDEF(cColor, "green", RUBY_METHOD_FUNC(color_green), 0);
  QDEF(cColor, "blue",  RUBY_METHOD_FUNC(color_blue), 0);
  QDEF(cColor, "name",  RUBY_METHOD_FUNC(color_name), 0);

  cSize = rb_define_class_under(mQt, "Size", rb_cObject);
  rb_define_singleton_method(cSize, "new", RUBY_METHOD_FUNC(size_new), 2);
  QDEF(cSize, "width",  RUBY_METHOD_FUNC(size_width), 0);
  QDEF(cSize, "height", RUBY_METHOD_FUNC(size_height), 0);

  cPoint = rb_define_class_under(mQt, "Point", rb_cObject);
  rb_define_singleton_method(cPoint, "new", RUBY_METHOD_FUNC(point_new), 2);
  QDEF(cPoint, "x", RUBY_METHOD_FUNC(point_x), 0);
  QDEF(cPoint, "y", RUBY_METHOD_FUNC(point_y), 0);
  QDEF(cPoint, "dispose", RUBY_METHOD_FUNC(value_dispose_noop), 0);

  // widget methods that depend on the value types above
  QDEF(cWidget, "setFont",       RUBY_METHOD_FUNC(widget_set_font), 1);
  QDEF(cWidget, "setFixedWidth",   RUBY_METHOD_FUNC((set_int<QWidget, &QWidget::setFixedWidth>)), 1);
  QDEF(cWidget, "setFixedHeight",  RUBY_METHOD_FUNC((set_int<QWidget, &QWidget::setFixedHeight>)), 1);
  QDEF(cWidget, "setMinimumWidth", RUBY_METHOD_FUNC((set_int<QWidget, &QWidget::setMinimumWidth>)), 1);
  QDEF(cWidget, "setMaximumWidth", RUBY_METHOD_FUNC((set_int<QWidget, &QWidget::setMaximumWidth>)), 1);
  QDEF(cWidget, "setMinimumHeight",RUBY_METHOD_FUNC((set_int<QWidget, &QWidget::setMinimumHeight>)), 1);
  QDEF(cWidget, "setMaximumHeight",RUBY_METHOD_FUNC((set_int<QWidget, &QWidget::setMaximumHeight>)), 1);
  QDEF(cWidget, "setFixedSize",    RUBY_METHOD_FUNC(widget_set_fixed_size), 2);
  QDEF(cWidget, "width",           RUBY_METHOD_FUNC((get_int<QWidget, &QWidget::width>)), 0);
  QDEF(cWidget, "height",          RUBY_METHOD_FUNC((get_int<QWidget, &QWidget::height>)), 0);
  QDEF(cWidget, "setUpdatesEnabled", RUBY_METHOD_FUNC((set_bool<QWidget, &QWidget::setUpdatesEnabled>)), 1);
  QDEF(cWidget, "update",          RUBY_METHOD_FUNC((call_void<QWidget, &QWidget::update>)), 0);
  QDEF(cWidget, "repaint",         RUBY_METHOD_FUNC((call_void<QWidget, &QWidget::repaint>)), 0);
  QDEF(cWidget, "raise_",          RUBY_METHOD_FUNC((call_void<QWidget, &QWidget::raise>)), 0);
  QDEF(cWidget, "style",           RUBY_METHOD_FUNC(widget_style), 0);
  QDEF(cWidget, "lower",           RUBY_METHOD_FUNC((call_void<QWidget, &QWidget::lower>)), 0);
  QDEF(cWidget, "activateWindow",  RUBY_METHOD_FUNC((call_void<QWidget, &QWidget::activateWindow>)), 0);
  QDEF(cWidget, "setHidden",       RUBY_METHOD_FUNC(widget_set_hidden), 1);
  QDEF(cWidget, "isHidden",        RUBY_METHOD_FUNC((get_bool<QWidget, &QWidget::isHidden>)), 0);
  QDEF(cWidget, "setDisabled",     RUBY_METHOD_FUNC(widget_set_disabled), 1);
  QDEF(cWidget, "setContextMenuPolicy", RUBY_METHOD_FUNC(widget_set_ctx_menu_policy), 1);
  QDEF(cWidget, "setStatusTip",   RUBY_METHOD_FUNC((set_str<QWidget, &QWidget::setStatusTip>)), 1);
  QDEF(cWidget, "setWhatsThis",   RUBY_METHOD_FUNC((set_str<QWidget, &QWidget::setWhatsThis>)), 1);
  QDEF(cWidget, "setWindowIcon",   RUBY_METHOD_FUNC(widget_set_window_icon), 1);
  QDEF(cWidget, "setIconSize",     RUBY_METHOD_FUNC(widget_set_icon_size), 1);
  QDEF(cWidget, "setMinimumSize",  RUBY_METHOD_FUNC(widget_set_minimum_size), -1);
  QDEF(cWidget, "setMaximumSize",  RUBY_METHOD_FUNC(widget_set_maximum_size), -1);
  QDEF(cWidget, "layout",          RUBY_METHOD_FUNC(widget_layout), 0);
  QDEF(cWidget, "childWidgets",    RUBY_METHOD_FUNC(widget_children_widgets), 0);
  QDEF(cWidget, "parentWidget",       RUBY_METHOD_FUNC(widget_parent_widget), 0);
  QDEF(cWidget, "sizeHint",           RUBY_METHOD_FUNC(widget_size_hint), 0);
  QDEF(cWidget, "minimumSizeHint",    RUBY_METHOD_FUNC(widget_min_size_hint), 0);
  QDEF(cWidget, "geometry",           RUBY_METHOD_FUNC(widget_geometry), 0);
  QDEF(cWidget, "frameGeometry",      RUBY_METHOD_FUNC(widget_frame_geometry), 0);
  QDEF(cWidget, "setContentsMargins", RUBY_METHOD_FUNC(widget_set_margins), 4);
  QDEF(cWidget, "setMouseTracking",   RUBY_METHOD_FUNC(widget_set_mouse_tracking), 1);
  QDEF(cWidget, "setFocusPolicy",     RUBY_METHOD_FUNC(widget_set_focus_policy), 1);
  QDEF(cWidget, "palette",            RUBY_METHOD_FUNC(widget_palette), 0);
  QDEF(cWidget, "font",               RUBY_METHOD_FUNC(widget_font), 0);
  QDEF(cWidget, "fontMetrics",        RUBY_METHOD_FUNC(widget_font_metrics), 0);
  QDEF(cWidget, "setAutoFillBackground", RUBY_METHOD_FUNC(widget_set_autofill), 1);
  QDEF(cWidget, "setBackgroundRole",   RUBY_METHOD_FUNC(widget_set_background_role), 1);
  QDEF(cWidget, "unsetCursor",         RUBY_METHOD_FUNC(widget_unset_cursor), 0);
  QDEF(cWidget, "mapFromGlobal",       RUBY_METHOD_FUNC(widget_map_from_global), 1);
  QDEF(cWidget, "mapToGlobal",         RUBY_METHOD_FUNC(widget_map_to_global), 1);
  QDEF(cWidget, "setSizePolicy", RUBY_METHOD_FUNC(widget_set_size_policy), 2);
  QDEF(cWidget, "size",          RUBY_METHOD_FUNC(widget_size), 0);
  QDEF(cWidget, "pos",           RUBY_METHOD_FUNC(widget_pos), 0);

  // ---- Qt::SizePolicy (constants only; COSMOS uses SizePolicy::Fixed) --
  cSizePolicy = rb_define_class_under(mQt, "SizePolicy", rb_cObject);
#define DEF_SP(n) rb_define_const(cSizePolicy, #n, INT2NUM((int)QSizePolicy::n))
  DEF_SP(Fixed); DEF_SP(Minimum); DEF_SP(Maximum); DEF_SP(Preferred);
  DEF_SP(Expanding); DEF_SP(MinimumExpanding); DEF_SP(Ignored);
#undef DEF_SP

  // ---- Qt::MessageBox --------------------------------------------------
  // QMessageBox IS-A QDialog; descending from Qt::Dialog gives it exec,
  // dispose, setWindowTitle and font for free.
  cMessageBox = rb_define_class_under(mQt, "MessageBox", cDialog);
  register_ctor(cMessageBox, ctor_message_box);
  QDEF(cMessageBox, "setIcon",            RUBY_METHOD_FUNC(msgbox_set_icon), 1);
  QDEF(cMessageBox, "setText",            RUBY_METHOD_FUNC(msgbox_set_text), 1);
  QDEF(cMessageBox, "setInformativeText", RUBY_METHOD_FUNC(msgbox_set_informative), 1);
  QDEF(cMessageBox, "setDetailedText",    RUBY_METHOD_FUNC(msgbox_set_detailed), 1);
  QDEF(cMessageBox, "setTextFormat",      RUBY_METHOD_FUNC(msgbox_set_text_format), 1);
  QDEF(cMessageBox, "setStandardButtons", RUBY_METHOD_FUNC(msgbox_set_standard_buttons), 1);
  QDEF(cMessageBox, "setDefaultButton",   RUBY_METHOD_FUNC(msgbox_set_default_button), 1);
  QDEF(cMessageBox, "addButton",          RUBY_METHOD_FUNC(msgbox_add_button), 2);
  QDEF(cMessageBox, "clickedButton",      RUBY_METHOD_FUNC(msgbox_clicked_button), 0);
  QSDEF(cMessageBox, "warning",
        RUBY_METHOD_FUNC((msgbox_static<QMessageBox::Warning>)), -1);
  QSDEF(cMessageBox, "critical",
        RUBY_METHOD_FUNC((msgbox_static<QMessageBox::Critical>)), -1);
  QSDEF(cMessageBox, "information",
        RUBY_METHOD_FUNC((msgbox_static<QMessageBox::Information>)), -1);
  QSDEF(cMessageBox, "question",
        RUBY_METHOD_FUNC((msgbox_static<QMessageBox::Question>)), -1);
#define DEF_MB(n) rb_define_const(cMessageBox, #n, INT2NUM((int)QMessageBox::n))
  DEF_MB(Ok); DEF_MB(Cancel); DEF_MB(Yes); DEF_MB(No); DEF_MB(Abort);
  DEF_MB(Retry); DEF_MB(Ignore); DEF_MB(Close); DEF_MB(Save); DEF_MB(Discard);
#undef DEF_MB

  // ---- Qt::FileDialog --------------------------------------------------
  cFileDialog = rb_define_class_under(mQt, "FileDialog", rb_cObject);
  rb_define_singleton_method(cFileDialog, "getOpenFileName",      RUBY_METHOD_FUNC(filedlg_open), -1);
  rb_define_singleton_method(cFileDialog, "getSaveFileName",      RUBY_METHOD_FUNC(filedlg_save), -1);
  rb_define_singleton_method(cFileDialog, "getExistingDirectory", RUBY_METHOD_FUNC(filedlg_dir), -1);
  rb_define_singleton_method(cFileDialog, "getOpenFileNames",      RUBY_METHOD_FUNC(filedlg_open_many), -1);

  // ---- Qt::CoreApplication ---------------------------------------------
  VALUE cCoreApp = rb_define_class_under(mQt, "CoreApplication", rb_cObject);
  rb_define_singleton_method(cCoreApp, "applicationName",    RUBY_METHOD_FUNC(core_app_name), 0);
  rb_define_singleton_method(cCoreApp, "setApplicationName", RUBY_METHOD_FUNC(core_set_app_name), 1);
  rb_define_singleton_method(cCoreApp, "instance",           RUBY_METHOD_FUNC(app_instance), 0);
  rb_define_singleton_method(cCoreApp, "processEvents",      RUBY_METHOD_FUNC(app_process_events_cls), 0);
  rb_define_singleton_method(cCoreApp, "closeAllWindows",    RUBY_METHOD_FUNC(app_close_all_windows), 0);
  // ---- tables ----------------------------------------------------------
  cTableWidgetItem = rb_define_class_under(mQt, "TableWidgetItem", rb_cObject);
  rb_define_singleton_method(cTableWidgetItem, "new", RUBY_METHOD_FUNC(twitem_new), -1);
  item_init_module(cTableWidgetItem, "TableWidgetItemInit", twitem_initialize);
  QDEF(cTableWidgetItem, "text",    RUBY_METHOD_FUNC(twitem_text), 0);
  QDEF(cTableWidgetItem, "textColor", RUBY_METHOD_FUNC(twi_text_color), 0);
  QDEF(cTableWidgetItem, "setText", RUBY_METHOD_FUNC(twitem_set_text), 1);
  QDEF(cTableWidgetItem, "setTextAlignment", RUBY_METHOD_FUNC(twitem_set_text_alignment), 1);
  QDEF(cTableWidgetItem, "setFlags",      RUBY_METHOD_FUNC(twitem_set_flags), 1);
  QDEF(cTableWidgetItem, "setCheckState", RUBY_METHOD_FUNC(twitem_set_check_state), 1);
  QDEF(cTableWidgetItem, "checkState",     RUBY_METHOD_FUNC(twitem_check_state), 0);
  QDEF(cTableWidgetItem, "setForeground",  RUBY_METHOD_FUNC(twitem_set_foreground), 1);
  QDEF(cTableWidgetItem, "setBackground",  RUBY_METHOD_FUNC(twitem_set_background), 1);
  QDEF(cTableWidgetItem, "setTextColor",   RUBY_METHOD_FUNC(twitem_set_foreground), 1);
  QDEF(cTableWidgetItem, "setFont",        RUBY_METHOD_FUNC(twitem_set_font), 1);
  QDEF(cTableWidgetItem, "setToolTip",     RUBY_METHOD_FUNC(twitem_set_tooltip), 1);
  QDEF(cTableWidgetItem, "setSizeHint",    RUBY_METHOD_FUNC(twitem_set_size_hint), 1);
  QDEF(cTableWidgetItem, "setData",        RUBY_METHOD_FUNC(twitem_set_data), 2);
  QDEF(cTableWidgetItem, "data",           RUBY_METHOD_FUNC(twitem_data), 1);

  cTableWidget = rb_define_class_under(mQt, "TableWidget", cAbstractItemView);
  // cmd_tlm_server_gui.rb:90 reopens Qt::TableWidget purely to override
  // wheelEvent; an unsubclassed QTableWidget has no virtual to route it.
  register_ctor(cTableWidget, ctor_plain<RubyForward<QTableWidget> >);
  QDEF(cTableWidget, "setRowCount",    RUBY_METHOD_FUNC((set_int<QTableWidget, &QTableWidget::setRowCount>)), 1);
  QDEF(cTableWidget, "rowCount",       RUBY_METHOD_FUNC((get_int<QTableWidget, &QTableWidget::rowCount>)), 0);
  QDEF(cTableWidget, "setColumnCount", RUBY_METHOD_FUNC((set_int<QTableWidget, &QTableWidget::setColumnCount>)), 1);
  QDEF(cTableWidget, "columnCount",    RUBY_METHOD_FUNC((get_int<QTableWidget, &QTableWidget::columnCount>)), 0);
  QDEF(cTableWidget, "setItem",        RUBY_METHOD_FUNC(tw_set_item), 3);
  QDEF(cTableWidget, "item",           RUBY_METHOD_FUNC(tw_item), 2);
  QDEF(cTableWidget, "setHorizontalHeaderItem", RUBY_METHOD_FUNC(tw_set_hheader), 2);
  QDEF(cTableWidget, "resizeColumnsToContents", RUBY_METHOD_FUNC((call_void<QTableView, &QTableView::resizeColumnsToContents>)), 0);
  QDEF(cTableWidget, "setHorizontalHeaderLabels", RUBY_METHOD_FUNC(tw_set_h_header_labels), 1);

  // ---- trees -----------------------------------------------------------
  cTreeWidgetItem = rb_define_class_under(mQt, "TreeWidgetItem", rb_cObject);
  rb_define_singleton_method(cTreeWidgetItem, "new", RUBY_METHOD_FUNC(tritem_new), -1);
  item_init_module(cTreeWidgetItem, "TreeWidgetItemInit", tritem_initialize);
  QDEF(cTreeWidgetItem, "text",     RUBY_METHOD_FUNC(tritem_text), 1);
  QDEF(cTreeWidgetItem, "setExpanded", RUBY_METHOD_FUNC(twi_set_expanded), 1);
  QDEF(cTreeWidgetItem, "setText",  RUBY_METHOD_FUNC(tritem_set_text), 2);
  QDEF(cTreeWidgetItem, "addChild",      RUBY_METHOD_FUNC(tritem_add_child), 1);
  QDEF(cTreeWidgetItem, "setForeground", RUBY_METHOD_FUNC(tritem_set_foreground), 2);
  QDEF(cTreeWidgetItem, "setBackground", RUBY_METHOD_FUNC(tritem_set_background), 2);

  cTreeWidget = rb_define_class_under(mQt, "TreeWidget", cAbstractItemView);
  register_ctor(cTreeWidget, ctor_plain<RubyForward<QTreeWidget> >);
  QDEF(cTreeWidget, "topLevelItem", RUBY_METHOD_FUNC(treew_top_level_item), 1);
  QDEF(cTreeWidget, "setColumnCount",     RUBY_METHOD_FUNC((set_int<QTreeWidget, &QTreeWidget::setColumnCount>)), 1);
  QDEF(cTreeWidget, "columnCount",        RUBY_METHOD_FUNC((get_int<QTreeWidget, &QTreeWidget::columnCount>)), 0);
  QDEF(cTreeWidget, "setHeaderLabels",        RUBY_METHOD_FUNC(tree_set_header_labels), 1);
  QDEF(cTreeWidget, "setItemWidget",          RUBY_METHOD_FUNC(tree_set_item_widget), 3);
  QDEF(cTreeWidget, "resizeColumnToContents", RUBY_METHOD_FUNC(tree_resize_col), 1);
  QDEF(cTreeWidget, "addTopLevelItem",    RUBY_METHOD_FUNC(tr_add_top), 1);
  QDEF(cTreeWidget, "topLevelItemCount",  RUBY_METHOD_FUNC(tr_top_count), 0);

  // ---- lists -----------------------------------------------------------
  cListWidget = rb_define_class_under(mQt, "ListWidget", cAbstractItemView);
  // Declared but never defined, so findItems/currentItem had nothing to wrap.
  cListWidgetItem = rb_define_class_under(mQt, "ListWidgetItem", rb_cObject);
  rb_define_singleton_method(cListWidgetItem, "new", RUBY_METHOD_FUNC(lwitem_new), -1);
  item_init_module(cListWidgetItem, "ListWidgetItemInit", lwitem_initialize);
  QDEF(cListWidgetItem, "text",        RUBY_METHOD_FUNC(lwi_text), 0);
  QDEF(cListWidgetItem, "setText",     RUBY_METHOD_FUNC(lwi_set_text), 1);
  QDEF(cListWidgetItem, "setSelected", RUBY_METHOD_FUNC(lwi_set_selected), 1);
  QDEF(cListWidgetItem, "isSelected",  RUBY_METHOD_FUNC(lwi_is_selected), 0);
  register_ctor(cListWidget, ctor_plain<RubyForward<QListWidget> >);
  QDEF(cListWidget, "setUniformItemSizes", RUBY_METHOD_FUNC(listw_set_uniform_item_sizes), 1);
  QDEF(cListWidget, "takeItem",            RUBY_METHOD_FUNC(listw_take_item), 1);
  QDEF(cListWidget, "visualItemRect",      RUBY_METHOD_FUNC(listw_visual_item_rect), 1);
  QDEF(cListWidget, "item",          RUBY_METHOD_FUNC(lw_item), 1);
  QDEF(cListWidget, "findItems",     RUBY_METHOD_FUNC(lw_find_items), -1);
  QDEF(cListWidget, "currentItem",   RUBY_METHOD_FUNC(lw_current_item), 0);
  QDEF(cListWidget, "selectedItems", RUBY_METHOD_FUNC(lw_selected_items), 0);
  QDEF(cListWidget, "addItem",  RUBY_METHOD_FUNC(lw_add_item), 1);
  QDEF(cListWidget, "itemText", RUBY_METHOD_FUNC(lw_item_text), 1);
  QDEF(cListWidget, "count",    RUBY_METHOD_FUNC((get_int<QListWidget, &QListWidget::count>)), 0);
  QDEF(cListWidget, "clear",    RUBY_METHOD_FUNC((call_void<QListWidget, &QListWidget::clear>)), 0);
  QDEF(cListWidget, "addItems", RUBY_METHOD_FUNC(combo_add_items), 1);

  // ---- containers ------------------------------------------------------
  cTabWidget = rb_define_class_under(mQt, "TabWidget", cWidget);
  QDEF(cTabWidget, "setTabIcon", RUBY_METHOD_FUNC(tabw_set_tab_icon), 2);
  register_ctor(cTabWidget, ctor_plain<QTabWidget>);
  QDEF(cTabWidget, "addTab",          RUBY_METHOD_FUNC(tab_add_tab), 2);
  QDEF(cTabWidget, "tabText",         RUBY_METHOD_FUNC(tab_text), 1);
  QDEF(cTabWidget, "count",           RUBY_METHOD_FUNC((get_int<QTabWidget, &QTabWidget::count>)), 0);
  QDEF(cTabWidget, "setCurrentIndex", RUBY_METHOD_FUNC((set_int<QTabWidget, &QTabWidget::setCurrentIndex>)), 1);
  QDEF(cTabWidget, "tabRect",       RUBY_METHOD_FUNC(tab_rect), 1);
  QDEF(cTabWidget, "tabBar",        RUBY_METHOD_FUNC(tabwidget_tab_bar), 0);
  QDEF(cTabWidget, "setTabIcon",    RUBY_METHOD_FUNC(tab_set_tab_icon), 2);
  QDEF(cTabWidget, "currentWidget", RUBY_METHOD_FUNC(tab_current_widget), 0);
  QDEF(cTabWidget, "currentTab",    RUBY_METHOD_FUNC(tab_current_widget), 0);
  QDEF(cTabWidget, "setTabEnabled",   RUBY_METHOD_FUNC(tab_set_tab_enabled), 2);
  QDEF(cTabWidget, "currentIndex",    RUBY_METHOD_FUNC((get_int<QTabWidget, &QTabWidget::currentIndex>)), 0);
  QDEF(cTabWidget, "setTabText",      RUBY_METHOD_FUNC(tab_set_tab_text), 2);

  cSplitter = rb_define_class_under(mQt, "Splitter", cFrame);
  register_ctor(cSplitter, ctor_splitter);   // honours Qt::Vertical/Horizontal
  QDEF(cSplitter, "orientation",    RUBY_METHOD_FUNC(splitter_orientation), 0);
  QDEF(cSplitter, "addWidget", RUBY_METHOD_FUNC(splitter_add), 1);
  QDEF(cSplitter, "count",            RUBY_METHOD_FUNC((get_int<QSplitter, &QSplitter::count>)), 0);
  QDEF(cSplitter, "setStretchFactor", RUBY_METHOD_FUNC(splitter_set_stretch), 2);
  QDEF(cBoxLayout, "setStretch",      RUBY_METHOD_FUNC(splitter_set_stretch), 2);

  cScrollArea = rb_define_class_under(mQt, "ScrollArea", cAbstractScrollArea);
  register_ctor(cScrollArea, ctor_plain<QScrollArea>);
  QDEF(cScrollArea, "ensureWidgetVisible", RUBY_METHOD_FUNC(scroll_ensure_visible), -1);
  QDEF(cScrollArea, "setWidget",          RUBY_METHOD_FUNC(scroll_set_widget), 1);
  QDEF(cScrollArea, "setWidgetResizable", RUBY_METHOD_FUNC((set_bool<QScrollArea, &QScrollArea::setWidgetResizable>)), 1);

  cFormLayout = rb_define_class_under(mQt, "FormLayout", cLayout);
  register_ctor(cFormLayout, ctor_layout<QFormLayout>);
  QDEF(cFormLayout, "addRow", RUBY_METHOD_FUNC(form_add_row), 2);

  // ---- menus and bars --------------------------------------------------
  cMenu = rb_define_class_under(mQt, "Menu", cWidget);
  register_ctor(cMenu, ctor_str<QMenu>);
  QDEF(cMenu, "addAction",    RUBY_METHOD_FUNC(menu_add_action), 1);
  QDEF(cMenu, "addSeparator", RUBY_METHOD_FUNC(menu_add_separator), 0);
  QDEF(cMenu, "title",        RUBY_METHOD_FUNC((get_str<QMenu, &QMenu::title>)), 0);
  QDEF(cMenu, "setTitle",     RUBY_METHOD_FUNC((set_str<QMenu, &QMenu::setTitle>)), 1);

  cMenuBar = rb_define_class_under(mQt, "MenuBar", cWidget);
  register_ctor(cMenuBar, ctor_plain<QMenuBar>);
  QDEF(cMenuBar, "addMenu", RUBY_METHOD_FUNC(menubar_add_menu), 1);

  cToolBar = rb_define_class_under(mQt, "ToolBar", cWidget);
  register_ctor(cToolBar, ctor_str<QToolBar>);
  QDEF(cToolBar, "setFloatable", RUBY_METHOD_FUNC(toolbar_set_floatable), 1);
  QDEF(cToolBar, "addAction",    RUBY_METHOD_FUNC(menu_add_action), 1);
  QDEF(cToolBar, "addSeparator", RUBY_METHOD_FUNC(menu_add_separator), 0);

  cStatusBar = rb_define_class_under(mQt, "StatusBar", cWidget);
  register_ctor(cStatusBar, ctor_plain<QStatusBar>);
  QDEF(cStatusBar, "clearMessage", RUBY_METHOD_FUNC(sb_clear_message), 0);
  QDEF(cStatusBar, "showMessage", RUBY_METHOD_FUNC(statusbar_show_message), -1);
  QDEF(cStatusBar, "currentMessage", RUBY_METHOD_FUNC((get_str<QStatusBar, &QStatusBar::currentMessage>)), 0);
  QDEF(cStatusBar, "addPermanentWidget", RUBY_METHOD_FUNC(statusbar_add_permanent), 1);

  QDEF(cMainWindow, "menuBar",   RUBY_METHOD_FUNC(mainwindow_menubar), 0);
  QDEF(cMainWindow, "statusBar", RUBY_METHOD_FUNC(mainwindow_statusbar), 0);

  // ---- numeric widgets -------------------------------------------------
  cProgressBar = rb_define_class_under(mQt, "ProgressBar", cWidget);
  register_ctor(cProgressBar, ctor_plain<QProgressBar>);
  QDEF(cProgressBar, "setValue", RUBY_METHOD_FUNC((set_int<QProgressBar, &QProgressBar::setValue>)), 1);
  QDEF(cProgressBar, "value",    RUBY_METHOD_FUNC((get_int<QProgressBar, &QProgressBar::value>)), 0);
  QDEF(cProgressBar, "setRange",   RUBY_METHOD_FUNC(range_set_range), 2);
  QDEF(cProgressBar, "setMinimum", RUBY_METHOD_FUNC((set_int<QProgressBar, &QProgressBar::setMinimum>)), 1);
  QDEF(cProgressBar, "setMaximum", RUBY_METHOD_FUNC((set_int<QProgressBar, &QProgressBar::setMaximum>)), 1);

  cRadioButton = rb_define_class_under(mQt, "RadioButton", cAbstractButton);
  register_ctor(cRadioButton, ctor_str<QRadioButton>);

  // QScrollBar shares QAbstractSlider's API; COSMOS uses .maximum/.setValue
  // to keep the message log scrolled to the bottom.
  cScrollBarKlass = rb_define_class_under(mQt, "ScrollBar", cWidget);
  register_ctor(cScrollBarKlass, ctor_plain<QScrollBar>);
  QDEF(cScrollBarKlass, "value",    RUBY_METHOD_FUNC((get_int<QAbstractSlider, &QAbstractSlider::value>)), 0);
  QDEF(cScrollBarKlass, "setValue", RUBY_METHOD_FUNC((set_int<QAbstractSlider, &QAbstractSlider::setValue>)), 1);
  QDEF(cScrollBarKlass, "maximum",  RUBY_METHOD_FUNC((get_int<QAbstractSlider, &QAbstractSlider::maximum>)), 0);
  QDEF(cScrollBarKlass, "minimum",  RUBY_METHOD_FUNC((get_int<QAbstractSlider, &QAbstractSlider::minimum>)), 0);
  QDEF(cScrollBarKlass, "setMaximum", RUBY_METHOD_FUNC((set_int<QAbstractSlider, &QAbstractSlider::setMaximum>)), 1);
  QDEF(cScrollBarKlass, "setMinimum", RUBY_METHOD_FUNC((set_int<QAbstractSlider, &QAbstractSlider::setMinimum>)), 1);
  register_qt_class("QScrollBar", cScrollBarKlass);

  cSlider = rb_define_class_under(mQt, "Slider", cWidget);
  register_ctor(cSlider, ctor_slider);       // honours Qt::Vertical/Horizontal
  QDEF(cSlider, "orientation",    RUBY_METHOD_FUNC(slider_orientation), 0);
  QDEF(cSlider, "setTickInterval",   RUBY_METHOD_FUNC(slider_set_tick_interval), 1);
  QDEF(cSlider, "setTickPosition",   RUBY_METHOD_FUNC(slider_set_tick_position), 1);
  QDEF(cSlider, "setTracking",       RUBY_METHOD_FUNC(slider_set_tracking), 1);
  QDEF(cSlider, "setSliderPosition", RUBY_METHOD_FUNC(slider_set_position), 1);
  QDEF(cSlider, "sliderPosition",    RUBY_METHOD_FUNC(slider_position), 0);
  QDEF(cSlider, "setOrientation", RUBY_METHOD_FUNC(slider_set_orientation), 1);
  QDEF(cSlider, "setValue", RUBY_METHOD_FUNC((set_int<QAbstractSlider, &QAbstractSlider::setValue>)), 1);
  QDEF(cSlider, "value",    RUBY_METHOD_FUNC((get_int<QAbstractSlider, &QAbstractSlider::value>)), 0);
  QDEF(cSlider, "setRange", RUBY_METHOD_FUNC(range_set_range), 2);

  cSpinBox = rb_define_class_under(mQt, "SpinBox", cWidget);
  register_ctor(cSpinBox, ctor_plain<QSpinBox>);
  QDEF(cSpinBox, "setValue", RUBY_METHOD_FUNC((set_int<QSpinBox, &QSpinBox::setValue>)), 1);
  QDEF(cSpinBox, "value",    RUBY_METHOD_FUNC((get_int<QSpinBox, &QSpinBox::value>)), 0);
  QDEF(cSpinBox, "setRange", RUBY_METHOD_FUNC(range_set_range), 2);

  cDoubleSpinBox = rb_define_class_under(mQt, "DoubleSpinBox", cWidget);
  register_ctor(cDoubleSpinBox, ctor_plain<QDoubleSpinBox>);
  QDEF(cDoubleSpinBox, "setValue", RUBY_METHOD_FUNC(dspin_set_value), 1);
  QDEF(cDoubleSpinBox, "value",    RUBY_METHOD_FUNC(dspin_value), 0);
  QDEF(cDoubleSpinBox, "setRange", RUBY_METHOD_FUNC(dspin_set_range), 2);

  // ---- text / timer ----------------------------------------------------
  cTextEdit = rb_define_class_under(mQt, "TextEdit", cAbstractScrollArea);
  register_ctor(cTextEdit, ctor_str<RubyForward<QTextEdit> >);
  QDEF(cTextEdit, "setPlainText", RUBY_METHOD_FUNC((set_str<QTextEdit, &QTextEdit::setPlainText>)), 1);
  QDEF(cTextEdit, "toPlainText",  RUBY_METHOD_FUNC((get_str<QTextEdit, &QTextEdit::toPlainText>)), 0);
  QDEF(cTextEdit, "currentCharFormat",    RUBY_METHOD_FUNC(edit_current_char_format), 0);
  QDEF(cTextEdit, "moveCursor",  RUBY_METHOD_FUNC(edit_move_cursor), -1);
  QDEF(cTextEdit, "cursorRect",  RUBY_METHOD_FUNC(edit_cursor_rect), 0);
  QDEF(cTextEdit, "selectAll",   RUBY_METHOD_FUNC(edit_select_all), 0);
  QDEF(cTextEdit, "paste",       RUBY_METHOD_FUNC(edit_paste), 0);
  QDEF(cTextEdit, "find",        RUBY_METHOD_FUNC(edit_find), -1);
  QDEF(cTextEdit, "copy",        RUBY_METHOD_FUNC(edit_copy), 0);
  QDEF(cTextEdit, "cut",         RUBY_METHOD_FUNC(edit_cut), 0);
  QDEF(cTextEdit, "setCurrentCharFormat", RUBY_METHOD_FUNC(edit_set_current_char_format), 1);
  QDEF(cTextEdit, "setReadOnly",  RUBY_METHOD_FUNC((set_bool<QTextEdit, &QTextEdit::setReadOnly>)), 1);
  QDEF(cTextEdit, "isReadOnly",   RUBY_METHOD_FUNC((get_bool<QTextEdit, &QTextEdit::isReadOnly>)), 0);
  QDEF(cTextEdit, "clear",         RUBY_METHOD_FUNC((call_void<QTextEdit, &QTextEdit::clear>)), 0);
  QDEF(cTextEdit, "setTextColor",  RUBY_METHOD_FUNC(textedit_set_text_color), 1);
  QDEF(cTextEdit, "setText",       RUBY_METHOD_FUNC((set_str<QTextEdit, &QTextEdit::setText>)), 1);
  QDEF(cTextEdit, "setHtml",       RUBY_METHOD_FUNC((set_str<QTextEdit, &QTextEdit::setHtml>)), 1);
  QDEF(cTextEdit, "toHtml",        RUBY_METHOD_FUNC((get_str<QTextEdit, &QTextEdit::toHtml>)), 0);
  QDEF(cTextEdit, "append",        RUBY_METHOD_FUNC((set_str<QTextEdit, &QTextEdit::append>)), 1);
  QDEF(cTextEdit, "text",          RUBY_METHOD_FUNC((get_str<QTextEdit, &QTextEdit::toPlainText>)), 0);
  QDEF(cTextEdit, "setPlainText",  RUBY_METHOD_FUNC((set_str<QTextEdit, &QTextEdit::setPlainText>)), 1);
  QDEF(cTextEdit, "toPlainText",   RUBY_METHOD_FUNC((get_str<QTextEdit, &QTextEdit::toPlainText>)), 0);
  QDEF(cTextEdit, "setLineWrapMode", RUBY_METHOD_FUNC(textedit_set_wrap_mode), 1);
  rb_define_const(cTextEdit, "NoWrap",     INT2NUM((int)QTextEdit::NoWrap));
  rb_define_const(cTextEdit, "WidgetWidth", INT2NUM((int)QTextEdit::WidgetWidth));
  rb_define_const(cPlainTextEdit, "NoWrap",      INT2NUM((int)QPlainTextEdit::NoWrap));
  rb_define_const(cPlainTextEdit, "WidgetWidth", INT2NUM((int)QPlainTextEdit::WidgetWidth));

  cTimer = rb_define_class_under(mQt, "Timer", cQtObject);
  register_ctor(cTimer, ctor_plain<QTimer>);
  QDEF(cTimer, "start",         RUBY_METHOD_FUNC(timer_start), -1);
  QDEF(cTimer, "stop",          RUBY_METHOD_FUNC((call_void<QTimer, &QTimer::stop>)), 0);
  QDEF(cTimer, "setInterval",   RUBY_METHOD_FUNC((set_int<QTimer, &QTimer::setInterval>)), 1);
  QDEF(cTimer, "interval",      RUBY_METHOD_FUNC((get_int<QTimer, &QTimer::interval>)), 0);
  QDEF(cTimer, "setSingleShot", RUBY_METHOD_FUNC((set_bool<QTimer, &QTimer::setSingleShot>)), 1);
  QDEF(cTimer, "isActive",      RUBY_METHOD_FUNC((get_bool<QTimer, &QTimer::isActive>)), 0);
  // ---- Qt::Palette / Qt::Cursor ----------------------------------------
  cPalette = rb_define_class_under(mQt, "Palette", rb_cObject);
  rb_define_singleton_method(cPalette, "new", RUBY_METHOD_FUNC(palette_new), -1);
#define DEF_PAL(n) rb_define_const(cPalette, #n, INT2NUM((int)QPalette::n))
  DEF_PAL(Active); DEF_PAL(Inactive); DEF_PAL(Disabled);
  DEF_PAL(Window); DEF_PAL(WindowText); DEF_PAL(Base); DEF_PAL(Text);
  DEF_PAL(Button); DEF_PAL(ButtonText); DEF_PAL(Highlight);
#undef DEF_PAL
  QDEF(cWidget, "setPalette", RUBY_METHOD_FUNC(widget_set_palette), 1);

  cCursor = rb_define_class_under(mQt, "Cursor", rb_cObject);
  rb_define_singleton_method(cCursor, "new",    RUBY_METHOD_FUNC(cursor_new), 1);
  rb_define_singleton_method(cCursor, "pos",    RUBY_METHOD_FUNC(cursor_pos), 0);
  rb_define_singleton_method(cCursor, "setPos", RUBY_METHOD_FUNC(cursor_set_pos), 2);
  QDEF(cWidget, "setCursor", RUBY_METHOD_FUNC(widget_set_cursor), 1);

  // ---- scroll bar policy -----------------------------------------------
  rb_define_const(mQt, "ScrollBarAsNeeded",  INT2NUM((int)Qt::ScrollBarAsNeeded));
  rb_define_const(mQt, "ScrollBarAlwaysOff", INT2NUM((int)Qt::ScrollBarAlwaysOff));
  rb_define_const(mQt, "ScrollBarAlwaysOn",  INT2NUM((int)Qt::ScrollBarAlwaysOn));
  QDEF(cAbstractScrollArea, "setHorizontalScrollBarPolicy", RUBY_METHOD_FUNC(scroll_set_hpolicy), 1);
  QDEF(cAbstractScrollArea, "setVerticalScrollBarPolicy",   RUBY_METHOD_FUNC(scroll_set_vpolicy), 1);
  QDEF(cAbstractScrollArea, "setViewportMargins", RUBY_METHOD_FUNC(scrollarea_set_viewport_margins), 4);
  QDEF(cAbstractScrollArea, "viewport",           RUBY_METHOD_FUNC(scrollarea_viewport), 0);
  QDEF(cAbstractScrollArea, "verticalScrollBar",   RUBY_METHOD_FUNC(scrollarea_vscrollbar), 0);
  QDEF(cAbstractScrollArea, "horizontalScrollBar", RUBY_METHOD_FUNC(scrollarea_hscrollbar), 0);

  // ---- Qt::AbstractItemView constants -----------------------------------
#define DEF_AIV(n) rb_define_const(cAbstractItemView, #n, INT2NUM((int)QAbstractItemView::n))
  DEF_AIV(NoEditTriggers); DEF_AIV(CurrentChanged); DEF_AIV(DoubleClicked);
  DEF_AIV(SelectedClicked); DEF_AIV(AnyKeyPressed); DEF_AIV(AllEditTriggers);
  DEF_AIV(NoSelection); DEF_AIV(SingleSelection); DEF_AIV(MultiSelection);
  DEF_AIV(ExtendedSelection); DEF_AIV(ContiguousSelection);
  DEF_AIV(InternalMove); DEF_AIV(DragDrop);
#undef DEF_AIV

  // ---- Qt::TextCursor constants ------------------------------------------
  cTextCursor = rb_define_class_under(mQt, "TextCursor", rb_cObject);
#define DEF_TC(n) rb_define_const(cTextCursor, #n, INT2NUM((int)QTextCursor::n))
  DEF_TC(Start); DEF_TC(End); DEF_TC(StartOfLine); DEF_TC(EndOfLine);
  DEF_TC(Up); DEF_TC(Down); DEF_TC(PreviousCharacter); DEF_TC(NextCharacter);
  DEF_TC(MoveAnchor); DEF_TC(KeepAnchor);
#undef DEF_TC

  // ---- Qt::LineEdit echo modes -------------------------------------------
#define DEF_ECHO(n) rb_define_const(cLineEdit, #n, INT2NUM((int)QLineEdit::n))
  DEF_ECHO(Normal); DEF_ECHO(NoEcho); DEF_ECHO(Password); DEF_ECHO(PasswordEchoOnEdit);
#undef DEF_ECHO
  QDEF(cLineEdit, "setEchoMode",  RUBY_METHOD_FUNC(lineedit_set_echo), 1);
  QDEF(cLineEdit, "setAlignment", RUBY_METHOD_FUNC(lineedit_set_alignment), 1);

  // ---- Qt::InputDialog / Qt::Shortcut ------------------------------------
  cInputDialog = rb_define_class_under(mQt, "InputDialog", cDialog);
  QSDEF(cInputDialog, "getText",                        RUBY_METHOD_FUNC(inputdlg_get_text), -1);
  QSDEF(cInputDialog, "getDouble",                      RUBY_METHOD_FUNC(inputdlg_get_double), -1);

  cShortcut = rb_define_class_under(mQt, "Shortcut", cQtObject);
  register_ctor(cShortcut, ctor_shortcut);

  rb_define_const(mQt, "PLUGIN_PATH", rb_str_new2(""));
  // ---- painting value types --------------------------------------------
  QDEF(cColor, "==",   RUBY_METHOD_FUNC(color_eq), 1);
  QDEF(cColor, "eql?", RUBY_METHOD_FUNC(color_eq), 1);
  QDEF(cColor, "hash", RUBY_METHOD_FUNC(color_hash), 0);

  cPen = rb_define_class_under(mQt, "Pen", rb_cObject);
  rb_define_singleton_method(cPen, "new", RUBY_METHOD_FUNC(pen_new), -1);
  QDEF(cPen, "setColor", RUBY_METHOD_FUNC(pen_set_color), 1);
  QDEF(cPen, "setWidth", RUBY_METHOD_FUNC(pen_set_width), 1);
  QDEF(cPen, "setStyle", RUBY_METHOD_FUNC(pen_set_style), 1);
  QDEF(cPen, "color",    RUBY_METHOD_FUNC(pen_color), 0);

  cLinearGradient = rb_define_class_under(mQt, "LinearGradient", rb_cObject);
  rb_define_singleton_method(cLinearGradient, "new", RUBY_METHOD_FUNC(lingrad_new), -1);
  QDEF(cLinearGradient, "setCoordinateMode", RUBY_METHOD_FUNC(lingrad_set_coord_mode), 1);
  QDEF(cLinearGradient, "setColorAt", RUBY_METHOD_FUNC(lingrad_set_color_at), 2);

  cBrush = rb_define_class_under(mQt, "Brush", rb_cObject);
  rb_define_singleton_method(cBrush, "new", RUBY_METHOD_FUNC(brush_new), -1);
  QDEF(cBrush, "setColor", RUBY_METHOD_FUNC(brush_set_color), 1);
  QDEF(cBrush, "color",    RUBY_METHOD_FUNC(brush_color), 0);

  cFontMetrics = rb_define_class_under(mQt, "FontMetrics", rb_cObject);
  QDEF(cFontMetrics, "lineSpacing", RUBY_METHOD_FUNC(fm_line_spacing), 0);
  rb_define_singleton_method(cFontMetrics, "new", RUBY_METHOD_FUNC(fontmetrics_new), 1);
  QDEF(cFontMetrics, "boundingRect", RUBY_METHOD_FUNC(fm_bounding_rect), 1);
  QDEF(cFontMetrics, "width",   RUBY_METHOD_FUNC(fm_width), 1);
  QDEF(cFontMetrics, "horizontalAdvance", RUBY_METHOD_FUNC(fm_width), 1);
  QDEF(cFontMetrics, "height",  RUBY_METHOD_FUNC(fm_height), 0);
  QDEF(cFontMetrics, "ascent",  RUBY_METHOD_FUNC(fm_ascent), 0);
  QDEF(cFontMetrics, "descent", RUBY_METHOD_FUNC(fm_descent), 0);
  QDEF(cFontMetrics, "size",    RUBY_METHOD_FUNC(fm_size), 2);

  QDEF(cPalette, "setColor", RUBY_METHOD_FUNC(palette_set_color), -1);
  QDEF(cPalette, "setBrush", RUBY_METHOD_FUNC(palette_set_brush), -1);

  // ---- Qt::Painter -----------------------------------------------------
  VALUE cPainter = rb_define_class_under(mQt, "Painter", rb_cObject);
  cPainterKlass = cPainter;
  rb_define_alloc_func(cPainter, painter_alloc);
  QDEF(cPainter, "initialize",  RUBY_METHOD_FUNC(painter_initialize), -1);
  QDEF(cPainter, "begin",       RUBY_METHOD_FUNC(painter_begin), 1);
  QDEF(cPainter, "end",         RUBY_METHOD_FUNC(painter_end), 0);
  QDEF(cPainter, "isActive",    RUBY_METHOD_FUNC(painter_is_active), 0);
  QDEF(cPainter, "paintEngine", RUBY_METHOD_FUNC(painter_paint_engine), 0);
  QDEF(cPainter, "dispose",     RUBY_METHOD_FUNC(painter_dispose), 0);
  QDEF(cPainter, "save",        RUBY_METHOD_FUNC(painter_save), 0);
  QDEF(cPainter, "restore",     RUBY_METHOD_FUNC(painter_restore), 0);
  QDEF(cPainter, "setPen",      RUBY_METHOD_FUNC(painter_set_pen), 1);
  QDEF(cPainter, "setBrush",    RUBY_METHOD_FUNC(painter_set_brush), 1);
  QDEF(cPainter, "setFont",     RUBY_METHOD_FUNC(painter_set_font), 1);
  QDEF(cPainter, "setBackground",     RUBY_METHOD_FUNC(painter_set_background), 1);
  QDEF(cPainter, "setBackgroundMode", RUBY_METHOD_FUNC(painter_set_background_mode), 1);
  QDEF(cPainter, "setRenderHint",     RUBY_METHOD_FUNC(painter_set_render_hint), -1);
  QDEF(cPainter, "drawLine",    RUBY_METHOD_FUNC(painter_draw_line), 4);
  QDEF(cPainter, "drawRect",    RUBY_METHOD_FUNC(painter_draw_rect), 4);
  QDEF(cPainter, "drawEllipse", RUBY_METHOD_FUNC(painter_draw_ellipse), 4);
  QDEF(cPainter, "drawText",    RUBY_METHOD_FUNC(painter_draw_text), -1);
  QDEF(cPainter, "fillRect",    RUBY_METHOD_FUNC(painter_fill_rect), -1);
  QDEF(cPainter, "drawImage",   RUBY_METHOD_FUNC(painter_draw_image), 3);
#define DEF_RH(n) rb_define_const(cPainter, #n, INT2NUM((int)QPainter::n))
  DEF_RH(Antialiasing); DEF_RH(TextAntialiasing); DEF_RH(SmoothPixmapTransform);
#undef DEF_RH

  cPixmap = rb_define_class_under(mQt, "Pixmap", rb_cObject);
  rb_define_singleton_method(cPixmap, "new", RUBY_METHOD_FUNC(pixmap_new), -1);
  QDEF(cPixmap, "width",  RUBY_METHOD_FUNC(pixmap_width), 0);
  QDEF(cPixmap, "height", RUBY_METHOD_FUNC(pixmap_height), 0);
  QDEF(cPixmap, "isNull", RUBY_METHOD_FUNC(pixmap_is_null), 0);
  QDEF(cPixmap, "save",    RUBY_METHOD_FUNC(pixmap_save), 1);
  QDEF(cPixmap, "toImage", RUBY_METHOD_FUNC(pixmap_to_image), 0);
  QDEF(cWidget, "grab",   RUBY_METHOD_FUNC(widget_grab), 0);

  cIcon = rb_define_class_under(mQt, "Icon", rb_cObject);
  rb_define_singleton_method(cIcon, "new", RUBY_METHOD_FUNC(icon_new), -1);
  QDEF(cIcon, "addPixmap", RUBY_METHOD_FUNC(icon_add_pixmap), -1);
  QDEF(cIcon, "isNull", RUBY_METHOD_FUNC(icon_is_null), 0);
  QDEF(cIcon, "pixmap", RUBY_METHOD_FUNC(icon_pixmap), -1);

  cImage = rb_define_class_under(mQt, "Image", rb_cObject);
  rb_define_singleton_method(cImage, "new", RUBY_METHOD_FUNC(image_new), -1);
  QDEF(cImage, "width",  RUBY_METHOD_FUNC(image_width), 0);
  QDEF(cImage, "height",     RUBY_METHOD_FUNC(image_height), 0);
  QDEF(cImage, "pixelColor", RUBY_METHOD_FUNC(image_pixel_color), 2);
  QDEF(cImage, "save",       RUBY_METHOD_FUNC(image_save), 1);
  QDEF(cImage, "isNull",     RUBY_METHOD_FUNC(image_is_null), 0);

  cSettings = rb_define_class_under(mQt, "Settings", cQtObject);
  rb_define_alloc_func(cSettings, qtobj_alloc);
  QDEF(cSettings, "initialize", RUBY_METHOD_FUNC(settings_init), -1);
  QDEF(cSettings, "setValue", RUBY_METHOD_FUNC(settings_set_value), 2);
  QDEF(cSettings, "value",    RUBY_METHOD_FUNC(settings_value), 1);
  QDEF(cSettings, "contains", RUBY_METHOD_FUNC(settings_contains), 1);
  QDEF(cSettings, "setValue", RUBY_METHOD_FUNC(settings_set_value_variant), 2);

  cDesktopWidget = rb_define_class_under(mQt, "DesktopWidget", rb_cObject);
  QDEF(cDesktopWidget, "screen",         RUBY_METHOD_FUNC(desktop_self), 0);
  QDEF(cDesktopWidget, "width",          RUBY_METHOD_FUNC(desktop_width), 0);
  QDEF(cDesktopWidget, "height",         RUBY_METHOD_FUNC(desktop_height), 0);
  QDEF(cDesktopWidget, "availableGeometry", RUBY_METHOD_FUNC(desktop_available), -1);
  QDEF(cDesktopWidget, "screenGeometry",    RUBY_METHOD_FUNC(desktop_screen_geometry), -1);
  // ---- validators / completer -------------------------------------------
  VALUE cValidator = rb_define_class_under(mQt, "Validator", cQtObject);
  cIntValidator = rb_define_class_under(mQt, "IntValidator", cValidator);
  rb_define_alloc_func(cIntValidator, qtobj_alloc);
  QDEF(cIntValidator, "initialize", RUBY_METHOD_FUNC(intval_init), -1);
  cDoubleValidator = rb_define_class_under(mQt, "DoubleValidator", cValidator);
  rb_define_alloc_func(cDoubleValidator, qtobj_alloc);
  QDEF(cDoubleValidator, "initialize", RUBY_METHOD_FUNC(dblval_init), -1);
  rb_define_const(cDoubleValidator, "StandardNotation",   INT2NUM((int)QDoubleValidator::StandardNotation));
  rb_define_const(cDoubleValidator, "ScientificNotation", INT2NUM((int)QDoubleValidator::ScientificNotation));
  QDEF(cDoubleValidator, "setNotation", RUBY_METHOD_FUNC(dblval_set_notation), 1);
  QDEF(cDoubleValidator, "setBottom",   RUBY_METHOD_FUNC(dblval_set_bottom), 1);
  QDEF(cDoubleValidator, "setTop",      RUBY_METHOD_FUNC(dblval_set_top), 1);
  QDEF(cDoubleValidator, "setRange",    RUBY_METHOD_FUNC(dblval_set_range), 2);
  QDEF(cIntValidator,    "setBottom",   RUBY_METHOD_FUNC(intval_set_bottom), 1);
  QDEF(cIntValidator,    "setTop",      RUBY_METHOD_FUNC(intval_set_top), 1);
  QDEF(cIntValidator,    "setRange",    RUBY_METHOD_FUNC(intval_set_range), 2);
  QDEF(cLineEdit, "setValidator", RUBY_METHOD_FUNC(lineedit_set_validator), 1);

  cCompleter = rb_define_class_under(mQt, "Completer", cQtObject);
  rb_define_alloc_func(cCompleter, qtobj_alloc);
  QDEF(cCompleter, "initialize", RUBY_METHOD_FUNC(completer_init), -1);
  QDEF(cCompleter, "setModel",   RUBY_METHOD_FUNC(completer_set_model), 1);
  QDEF(cCompleter, "model",      RUBY_METHOD_FUNC(completer_model), 0);
  QDEF(cCompleter, "setWidget",  RUBY_METHOD_FUNC(completer_set_widget), 1);
  QDEF(cCompleter, "setCaseSensitivity", RUBY_METHOD_FUNC(completer_set_case_sensitivity), 1);
  QDEF(cLineEdit, "setCompleter", RUBY_METHOD_FUNC(lineedit_set_completer), 1);

  // ---- geometry ----------------------------------------------------------
  cRect = rb_define_class_under(mQt, "Rect", rb_cObject);
  rb_define_singleton_method(cRect, "new", RUBY_METHOD_FUNC(rect_new), 4);
  QDEF(cRect, "contains", RUBY_METHOD_FUNC(rect_contains), -1);
  QDEF(cRect, "x",        RUBY_METHOD_FUNC(rect_x), 0);
  QDEF(cRect, "y",        RUBY_METHOD_FUNC(rect_y), 0);
  QDEF(cRect, "left",     RUBY_METHOD_FUNC(rect_x), 0);
  QDEF(cRect, "top",      RUBY_METHOD_FUNC(rect_y), 0);
  QDEF(cRect, "right",    RUBY_METHOD_FUNC(rect_right), 0);
  QDEF(cRect, "bottom",   RUBY_METHOD_FUNC(rect_bottom), 0);
  QDEF(cRect, "setWidth", RUBY_METHOD_FUNC(rect_set_width), 1);
  QDEF(cRect, "isValid",  RUBY_METHOD_FUNC(rect_valid), 0);
  QDEF(cRect, "width",  RUBY_METHOD_FUNC(rect_w), 0);
  QDEF(cRect, "height", RUBY_METHOD_FUNC(rect_h), 0);
  QDEF(cRect, "translated", RUBY_METHOD_FUNC(rect_translated), 1);
  QDEF(cRect, "dispose",    RUBY_METHOD_FUNC(value_dispose_noop), 0);
  // Qt::Polygon was declared but never defined -- no class, no constructor.
  // Declared but never defined, like Polygon and ListWidgetItem were.
  cDate = rb_define_class_under(mQt, "Date", rb_cObject);
  rb_define_singleton_method(cDate, "new", RUBY_METHOD_FUNC(date_new), -1);
  QDEF(cDate, "year",    RUBY_METHOD_FUNC(date_year), 0);
  QDEF(cDate, "month",   RUBY_METHOD_FUNC(date_month), 0);
  QDEF(cDate, "day",     RUBY_METHOD_FUNC(date_day), 0);
  QDEF(cDate, "isValid", RUBY_METHOD_FUNC(date_valid), 0);

  cPolygon = rb_define_class_under(mQt, "Polygon", rb_cObject);
  rb_define_singleton_method(cPolygon, "new", RUBY_METHOD_FUNC(polygon_new), -1);
  QDEF(cPolygon, "setPoint", RUBY_METHOD_FUNC(polygon_set_point), 3);
  QDEF(cPolygon, "point",    RUBY_METHOD_FUNC(polygon_point), 1);
  QDEF(cPolygon, "size",     RUBY_METHOD_FUNC(polygon_size), 0);

  cPointF = rb_define_class_under(mQt, "PointF", rb_cObject);
  rb_define_singleton_method(cPointF, "new", RUBY_METHOD_FUNC(pointf_new), 2);
  QDEF(cPointF, "x", RUBY_METHOD_FUNC(pointf_x), 0);
  QDEF(cPointF, "y", RUBY_METHOD_FUNC(pointf_y), 0);

  // ---- model / view ------------------------------------------------------
  cModelIndex = rb_define_class_under(mQt, "ModelIndex", rb_cObject);
  QDEF(cModelIndex, "row",     RUBY_METHOD_FUNC(mi_row), 0);
  QDEF(cModelIndex, "column",  RUBY_METHOD_FUNC(mi_column), 0);
  QDEF(cModelIndex, "isValid", RUBY_METHOD_FUNC(mi_valid), 0);
  QDEF(cModelIndex, "valid?",  RUBY_METHOD_FUNC(mi_valid), 0);
  QDEF(cModelIndex, "data",    RUBY_METHOD_FUNC(mi_data), -1);
  QDEF(cModelIndex, "parent",  RUBY_METHOD_FUNC(mi_parent), 0);

  cAbstractItemModel = rb_define_class_under(mQt, "AbstractItemModel", cQtObject);
  VALUE cStringListModel = rb_define_class_under(mQt, "StringListModel", cAbstractItemModel);
  rb_define_alloc_func(cStringListModel, qtobj_alloc);
  QDEF(cStringListModel, "initialize", RUBY_METHOD_FUNC(slmodel_init), -1);
  VALUE cFileSystemModel = rb_define_class_under(mQt, "FileSystemModel", cAbstractItemModel);
  register_ctor(cFileSystemModel, ctor_plain<QFileSystemModel>);
  QDEF(cFileSystemModel, "setRootPath", RUBY_METHOD_FUNC(fsmodel_set_root_path), 1);
  QDEF(cFileSystemModel, "index",       RUBY_METHOD_FUNC(fsmodel_index), 1);
  QDEF(cFileSystemModel, "filePath",    RUBY_METHOD_FUNC(fsmodel_file_path), 1);

  QDEF(cAbstractItemView, "setSelectionMode", RUBY_METHOD_FUNC(aiv_set_selection_mode), 1);
  QDEF(cAbstractItemView, "setEditTriggers",  RUBY_METHOD_FUNC(aiv_set_edit_triggers), 1);
  QDEF(cAbstractItemView, "setModel",         RUBY_METHOD_FUNC(view_set_model), 1);
  QDEF(cAbstractItemView, "model",            RUBY_METHOD_FUNC(view_model), 0);
  QDEF(cAbstractItemView, "setRootIndex",     RUBY_METHOD_FUNC(view_set_root_index), 1);
  QDEF(cAbstractItemView, "setItemDelegate",  RUBY_METHOD_FUNC(view_set_item_delegate), 1);
  QDEF(cAbstractItemView, "setCurrentIndex",  RUBY_METHOD_FUNC(view_set_current_index), 1);
  QDEF(cAbstractItemView, "currentIndex",     RUBY_METHOD_FUNC(view_current_index), 0);
  QDEF(cAbstractItemView, "sizeHintForColumn", RUBY_METHOD_FUNC(view_size_hint_for_column), 1);
  QDEF(cAbstractItemView, "indexAt",          RUBY_METHOD_FUNC(view_index_at), 1);
  QDEF(cAbstractItemView, "scrollTo",         RUBY_METHOD_FUNC(view_scroll_to), 1);
  QDEF(cComboBox,         "setModel",         RUBY_METHOD_FUNC(view_set_model), 1);

  VALUE cListView = rb_define_class_under(mQt, "ListView", cAbstractItemView);
  register_ctor(cListView, ctor_plain<QListView>);
  VALUE cTreeView = rb_define_class_under(mQt, "TreeView", cAbstractItemView);
  register_ctor(cTreeView, ctor_plain<RubyForward<QTreeView> >);
  QDEF(cTreeView, "setColumnHidden", RUBY_METHOD_FUNC(view_set_column_hidden), 2);
  QDEF(cTreeView, "isColumnHidden",  RUBY_METHOD_FUNC(view_is_column_hidden), 1);
  QDEF(cTreeView, "expand",          RUBY_METHOD_FUNC(tree_expand), 1);
  QDEF(cTreeView, "collapse",        RUBY_METHOD_FUNC(tree_collapse), 1);

  cHeaderViewKlass = rb_define_class_under(mQt, "HeaderView", cWidget);
  QDEF(cHeaderViewKlass, "setResizeMode",        RUBY_METHOD_FUNC(header_set_resize_mode), -1);
  QDEF(cHeaderViewKlass, "setSectionResizeMode", RUBY_METHOD_FUNC(header_set_resize_mode), -1);
  QDEF(cHeaderViewKlass, "sectionResizeMode",    RUBY_METHOD_FUNC(header_section_resize_mode), 1);
#define DEF_HV(n) rb_define_const(cHeaderViewKlass, #n, INT2NUM((int)QHeaderView::n))
  DEF_HV(Interactive); DEF_HV(Fixed); DEF_HV(Stretch); DEF_HV(ResizeToContents);
#undef DEF_HV
  QDEF(cTableWidget, "editItem",        RUBY_METHOD_FUNC(tw_edit_item), 1);
  QDEF(cTableWidget, "setCurrentCell",  RUBY_METHOD_FUNC(tw_set_current_cell), 2);
  QDEF(cTableWidget, "verticalHeader",   RUBY_METHOD_FUNC(table_vertical_header), 0);
  QDEF(cTableWidget, "horizontalHeader", RUBY_METHOD_FUNC(table_horizontal_header), 0);

  VALUE cStyledItemDelegate = rb_define_class_under(mQt, "StyledItemDelegate", cQtObject);
  register_ctor(cStyledItemDelegate, ctor_plain<RubyItemDelegate>);   // virtuals -> Ruby
  QDEF(cStyledItemDelegate, "initStyleOption", RUBY_METHOD_FUNC(delegate_init_style_option), 2);
  QDEF(cStyledItemDelegate, "commitData",  RUBY_METHOD_FUNC(delegate_commit_data), 1);
  QDEF(cStyledItemDelegate, "closeEditor", RUBY_METHOD_FUNC(delegate_close_editor), -1);

  // ---- text --------------------------------------------------------------
  cTextDocument = rb_define_class_under(mQt, "TextDocument", cQtObject);
  cTextDocumentKlass = cTextDocument;
  register_ctor(cTextDocument, ctor_plain<QTextDocument>);
  QDEF(cTextDocument, "blockCount",     RUBY_METHOD_FUNC(doc_block_count), 0);
  QDEF(cTextDocument, "defaultFont",    RUBY_METHOD_FUNC(doc_default_font), 0);
  QDEF(cTextDocument, "setDefaultFont",     RUBY_METHOD_FUNC(doc_set_default_font), 1);
  QDEF(cTextDocument, "findBlockByNumber",  RUBY_METHOD_FUNC(doc_find_block_by_number), 1);
  QDEF(cTextDocument, "firstBlock",            RUBY_METHOD_FUNC(doc_first_block), 0);
  QDEF(cTextDocument, "size",                  RUBY_METHOD_FUNC(doc_size), 0);
  register_qt_class("QTextDocument", cTextDocument);   // so document() wraps specifically
  QDEF(cTextDocument, "findBlockByLineNumber", RUBY_METHOD_FUNC(doc_find_block_by_line), 1);
  QDEF(cTextDocument, "isModified",  RUBY_METHOD_FUNC(doc_is_modified), 0);
  QDEF(cTextDocument, "setModified", RUBY_METHOD_FUNC(doc_set_modified), -1);
  cTextCharFormat = rb_define_class_under(mQt, "TextCharFormat", rb_cObject);
  rb_define_singleton_method(cTextCharFormat, "new", RUBY_METHOD_FUNC(tcf_new), 0);
  QDEF(cTextCharFormat, "setForeground", RUBY_METHOD_FUNC(tcf_set_foreground), 1);
  QDEF(cTextCharFormat, "setFont",       RUBY_METHOD_FUNC(tcf_set_font), 1);
  QDEF(cTextCharFormat, "font",          RUBY_METHOD_FUNC(tcf_font), 0);
  QDEF(cTextCharFormat, "setFontWeight", RUBY_METHOD_FUNC(tcf_set_font_weight), 1);
  QDEF(cTextCharFormat, "setFontItalic", RUBY_METHOD_FUNC(tcf_set_font_italic), 1);
  QDEF(cTextCharFormat, "setProperty",   RUBY_METHOD_FUNC(tcf_set_property), 2);
  // QFont weight constants COSMOS passes to setFontWeight
  rb_define_const(cFont, "Normal", INT2NUM((int)QFont::Normal));
  rb_define_const(cFont, "Bold",   INT2NUM((int)QFont::Bold));
  cTextOption = rb_define_class_under(mQt, "TextOption", rb_cObject);
  rb_define_const(cTextOption, "NoWrap",     INT2NUM((int)QTextOption::NoWrap));
  rb_define_const(cTextOption, "WordWrap",   INT2NUM((int)QTextOption::WordWrap));
  rb_define_const(cTextOption, "WrapAnywhere", INT2NUM((int)QTextOption::WrapAnywhere));
  VALUE cSyntaxHighlighter = rb_define_class_under(mQt, "SyntaxHighlighter", cQtObject);
  register_ctor(cSyntaxHighlighter, ctor_highlighter);
  QDEF(cSyntaxHighlighter, "setFormat", RUBY_METHOD_FUNC(highlighter_set_format), 3);

  // ---- misc widgets / helpers --------------------------------------------
  VALUE cDialogButtonBox = rb_define_class_under(mQt, "DialogButtonBox", cWidget);
  register_ctor(cDialogButtonBox, ctor_plain<QDialogButtonBox>);
  QDEF(cDialogButtonBox, "addButton", RUBY_METHOD_FUNC(dbb_add_button), 1);
#define DEF_DBB(n) rb_define_const(cDialogButtonBox, #n, INT2NUM((int)QDialogButtonBox::n))
  DEF_DBB(Ok); DEF_DBB(Cancel); DEF_DBB(Yes); DEF_DBB(No); DEF_DBB(Close);
#undef DEF_DBB
  VALUE cActionGroup = rb_define_class_under(mQt, "ActionGroup", cQtObject);
  VALUE cCalendarWidget = rb_define_class_under(mQt, "CalendarWidget", cWidget);
  register_ctor(cCalendarWidget, ctor_plain<QCalendarWidget>);
  QDEF(cCalendarWidget, "selectedDate",            RUBY_METHOD_FUNC(cal_selected_date), 0);
  QDEF(cCalendarWidget, "setSelectedDate",         RUBY_METHOD_FUNC(cal_set_selected_date), 1);
  QDEF(cCalendarWidget, "setVerticalHeaderFormat", RUBY_METHOD_FUNC(cal_set_v_header_format), 1);
  VALUE cEventLoop = rb_define_class_under(mQt, "EventLoop", cQtObject);
  register_ctor(cEventLoop, ctor_plain<QEventLoop>);
  VALUE cMovie = rb_define_class_under(mQt, "Movie", cQtObject);
  // Snapshot of the QMimeData behind a drop: the real one dies with the event.
  cMimeDataCls = rb_define_class_under(mQt, "MimeData", rb_cObject);
  rb_define_attr(cMimeDataCls, "hasUrls", 1, 0);
  rb_define_attr(cMimeDataCls, "urls",    1, 0);
  rb_define_attr(cMimeDataCls, "text",    1, 0);
  rb_define_class_under(mQt, "Drag", cQtObject);
  rb_define_class_under(mQt, "SpacerItem", rb_cObject);
  rb_define_class_under(mQt, "Polygon", rb_cObject);
  VALUE cChar = rb_define_class_under(mQt, "Char", rb_cObject);
  rb_define_singleton_method(cChar, "new", RUBY_METHOD_FUNC(qchar_new), 1);
  QDEF(cChar, "to_s",    RUBY_METHOD_FUNC(qchar_to_s), 0);
  QDEF(cChar, "to_str",  RUBY_METHOD_FUNC(qchar_to_s), 0);
  QDEF(cChar, "unicode", RUBY_METHOD_FUNC(qchar_unicode), 0);
  // ---- event objects ----
  // Every forwarded Qt virtual now receives one of these instead of nil, and
  // accept/ignore on it is written back onto the real QEvent after dispatch.
  cEventCls = rb_define_class_under(mQt, "Event", rb_cObject);
  rb_define_attr(cEventCls, "type", 1, 0);
  QDEF(cEventCls, "accept",      RUBY_METHOD_FUNC(event_accept),       0);
  QDEF(cEventCls, "ignore",      RUBY_METHOD_FUNC(event_ignore),       0);
  QDEF(cEventCls, "isAccepted",  RUBY_METHOD_FUNC(event_is_accepted),  0);
  QDEF(cEventCls, "accepted?",   RUBY_METHOD_FUNC(event_is_accepted),  0);
  QDEF(cEventCls, "setAccepted", RUBY_METHOD_FUNC(event_set_accepted), 1);
  QDEF(cEventCls, "dispose",     RUBY_METHOD_FUNC(value_dispose_noop), 0);

  cKeyEventCls = rb_define_class_under(mQt, "KeyEvent", cEventCls);
  rb_define_attr(cKeyEventCls, "key", 1, 0);
  rb_define_attr(cKeyEventCls, "text", 1, 0);
  rb_define_attr(cKeyEventCls, "modifiers", 1, 0);

  cMouseEventCls = rb_define_class_under(mQt, "MouseEvent", cEventCls);
  rb_define_attr(cMouseEventCls, "x", 1, 0);
  rb_define_attr(cMouseEventCls, "y", 1, 0);
  rb_define_attr(cMouseEventCls, "button", 1, 0);
  rb_define_attr(cMouseEventCls, "buttons", 1, 0);
  rb_define_attr(cMouseEventCls, "modifiers", 1, 0);
  rb_define_attr(cMouseEventCls, "pos", 1, 0);

  cCloseEventCls  = rb_define_class_under(mQt, "CloseEvent",  cEventCls);
  cShowEventCls   = rb_define_class_under(mQt, "ShowEvent",   cEventCls);
  cResizeEventCls = rb_define_class_under(mQt, "ResizeEvent", cEventCls);
  cFocusEventCls  = rb_define_class_under(mQt, "FocusEvent",  cEventCls);
  cLeaveEventCls  = rb_define_class_under(mQt, "LeaveEvent",  cEventCls);

  cPaintEventCls = rb_define_class_under(mQt, "PaintEvent", cEventCls);
  rb_define_attr(cPaintEventCls, "rect", 1, 0);

  cWheelEventCls = rb_define_class_under(mQt, "WheelEvent", cEventCls);
  rb_define_attr(cWheelEventCls, "modifiers", 1, 0);
  QDEF(cWheelEventCls, "delta", RUBY_METHOD_FUNC(wheel_delta), 0);

  // Drag and drop: setAcceptDrops(true) already worked, but nothing forwarded
  // the drag virtuals and these classes did not exist, so dropping a script on
  // Script Runner or Config Editor silently did nothing.
  cDragEnterEventCls = rb_define_class_under(mQt, "DragEnterEvent", cEventCls);
  cDragMoveEventCls  = rb_define_class_under(mQt, "DragMoveEvent",  cEventCls);
  cDropEventCls      = rb_define_class_under(mQt, "DropEvent",      cEventCls);
  VALUE dnd[3] = { cDragEnterEventCls, cDragMoveEventCls, cDropEventCls };
  for (int i = 0; i < 3; i++) {
    rb_define_attr(dnd[i], "mimeData", 1, 0);
    QDEF(dnd[i], "acceptProposedAction", RUBY_METHOD_FUNC(event_accept_proposed), 0);
  }
  VALUE cStyleK = rb_define_class_under(mQt, "Style", cQtObject);
#define DEF_STYLE_PIX(n) rb_define_const(cStyleK, #n, INT2NUM((int)QStyle::n))
  DEF_STYLE_PIX(SP_MessageBoxCritical); DEF_STYLE_PIX(SP_MessageBoxInformation);
  DEF_STYLE_PIX(SP_MessageBoxQuestion); DEF_STYLE_PIX(SP_MessageBoxWarning);
  DEF_STYLE_PIX(SP_DialogOkButton); DEF_STYLE_PIX(SP_DialogCancelButton);
#undef DEF_STYLE_PIX
  QDEF(cStyleK, "standardIcon", RUBY_METHOD_FUNC(style_standard_icon), 1);
  QDEF(cStyleK, "drawControl",  RUBY_METHOD_FUNC(style_draw_control), 3);
  cStyleOptionButtonKlass = rb_define_class_under(mQt, "StyleOptionButton", rb_cObject);
  rb_define_singleton_method(cStyleOptionButtonKlass, "new", RUBY_METHOD_FUNC(sob_new), -1);
  QDEF(cStyleOptionButtonKlass, "rect",    RUBY_METHOD_FUNC(sob_rect), 0);
  QDEF(cStyleOptionButtonKlass, "rect=",   RUBY_METHOD_FUNC(sob_set_rect), 1);
  QDEF(cStyleOptionButtonKlass, "text=",   RUBY_METHOD_FUNC(sob_set_text), 1);
  QDEF(cStyleOptionButtonKlass, "dispose", RUBY_METHOD_FUNC(value_dispose_noop), 0);
  VALUE cSOVI = rb_define_class_under(mQt, "StyleOptionViewItem", rb_cObject);
  cStyleOptionViewItemKlass = cSOVI;
  rb_define_singleton_method(cSOVI, "new", RUBY_METHOD_FUNC(sovi_new), -1);
  QDEF(cSOVI, "rect",      RUBY_METHOD_FUNC(sovi_rect), 0);
  QDEF(cSOVI, "rect=",     RUBY_METHOD_FUNC(sovi_set_rect), 1);
  QDEF(cSOVI, "text",      RUBY_METHOD_FUNC(sovi_text), 0);
  QDEF(cSOVI, "text=",     RUBY_METHOD_FUNC(sovi_set_text), 1);
  QDEF(cSOVI, "features=", RUBY_METHOD_FUNC(sovi_set_features), 1);
  QDEF(cSOVI, "dispose",   RUBY_METHOD_FUNC(value_dispose_noop), 0);
  rb_define_const(cSOVI, "WrapText", INT2NUM((int)QStyleOptionViewItem::WrapText));
  // QStyleOptionViewItemV2/V3/V4 were removed in Qt5; COSMOS still names them.
  rb_define_const(mQt, "StyleOptionViewItemV2", cSOVI);
  rb_define_const(mQt, "StyleOptionViewItemV3", cSOVI);
  rb_define_const(mQt, "StyleOptionViewItemV4", cSOVI);
  rb_define_class_under(mQt, "ToolTip", cQtObject);
  VALUE cGradient = rb_define_class_under(mQt, "Gradient", rb_cObject);
#define DEF_GRAD(n) rb_define_const(cGradient, #n, INT2NUM((int)QGradient::n))
  DEF_GRAD(LogicalMode); DEF_GRAD(StretchToDeviceMode); DEF_GRAD(ObjectBoundingMode);
#undef DEF_GRAD
  rb_define_class_under(mQt, "RadialGradient", rb_cObject);
  rb_define_class_under(mQt, "ListWidgetItem", rb_cObject);
  // QSound was removed in Qt6 (QSoundEffect replaces it); COSMOS only plays
  // optional notification sounds, so a no-op keeps those call sites alive.
  rb_eval_string("module Qt; class Sound; def self.play(*); nil; end; "
                 "def self.isAvailable; false; end; end; end");
  // QGLWidget was removed in Qt6.
  VALUE cGLWidget = rb_define_class_under(mQt, "GLWidget", cWidget);
  register_ctor(cGLWidget, ctor_plain<RubyGLWidget>);   // GL virtuals -> Ruby
  QDEF(cGLWidget, "makeCurrent",     RUBY_METHOD_FUNC(glw_make_current), 0);
  QDEF(cGLWidget, "doneCurrent",     RUBY_METHOD_FUNC(glw_done_current), 0);
  QDEF(cGLWidget, "updateGL",        RUBY_METHOD_FUNC(glw_update_gl), 0);
  QDEF(cGLWidget, "isValid",         RUBY_METHOD_FUNC(glw_is_valid), 0);
  QDEF(cGLWidget, "grabFrameBuffer", RUBY_METHOD_FUNC(glw_grab_framebuffer), 0);
  // Qt4 swapped buffers manually; QOpenGLWidget does it automatically.
  rb_eval_string("module Qt; class GLWidget; def swapBuffers; nil; end; "
                 "def setAutoBufferSwap(*); nil; end; end; end");

  cUrl = rb_define_class_under(mQt, "Url", rb_cObject);
  QDEF(cUrl, "toLocalFile", RUBY_METHOD_FUNC(url_to_local_file), 0);
  rb_define_singleton_method(cUrl, "new", RUBY_METHOD_FUNC(url_new), 1);
  QDEF(cUrl, "toString", RUBY_METHOD_FUNC(url_to_s), 0);
  VALUE cDesktopServices = rb_define_class_under(mQt, "DesktopServices", rb_cObject);
  rb_define_singleton_method(cDesktopServices, "openUrl", RUBY_METHOD_FUNC(desktop_open_url), 1);

  // ---- remaining enum constants ------------------------------------------
  rb_define_const(mQt, "Key_F",  INT2NUM((int)Qt::Key_F));
#define DEF_FN(n) rb_define_const(mQt, "Key_F" #n, INT2NUM((int)Qt::Key_F##n))
  DEF_FN(1); DEF_FN(2); DEF_FN(3); DEF_FN(4); DEF_FN(5); DEF_FN(6);
  DEF_FN(7); DEF_FN(8); DEF_FN(9); DEF_FN(10); DEF_FN(11); DEF_FN(12);
#undef DEF_FN
  rb_define_const(mQt, "CTRL",   INT2NUM((int)Qt::ControlModifier));
  rb_define_const(mQt, "SHIFT",  INT2NUM((int)Qt::ShiftModifier));
  rb_define_const(mQt, "OpaqueMode", INT2NUM((int)Qt::OpaqueMode));
  rb_define_const(mQt, "AutoText",   INT2NUM((int)Qt::AutoText));
  VALUE cTextFormatK = rb_define_class_under(mQt, "TextFormat", rb_cObject);
  rb_define_const(cTextFormatK, "FullWidthSelection",
                  INT2NUM((int)QTextFormat::FullWidthSelection));
  // Qt4's ItemIsTristate was renamed ItemIsUserTristate in Qt6.
  rb_define_const(mQt, "ItemIsTristate", INT2NUM((int)Qt::ItemIsUserTristate));
  // ---- runtime batch ----------------------------------------------------
  cMovieKlass = rb_define_class_under(mQt, "Movie", cQtObject);
  rb_define_alloc_func(cMovieKlass, qtobj_alloc);
  QDEF(cMovieKlass, "initialize", RUBY_METHOD_FUNC(movie_init), -1);
  QDEF(cMovieKlass, "start",      RUBY_METHOD_FUNC(movie_start), 0);
  QDEF(cMovieKlass, "stop",       RUBY_METHOD_FUNC(movie_stop), 0);
  QDEF(cLabel, "setMovie",                RUBY_METHOD_FUNC(label_set_movie), 1);
  QDEF(cLabel, "setTextInteractionFlags", RUBY_METHOD_FUNC(label_set_text_interaction), 1);

  QDEF(cWidget, "setAcceptDrops",  RUBY_METHOD_FUNC(widget_set_accept_drops), 1);
  QDEF(cWidget, "addAction",       RUBY_METHOD_FUNC(widget_add_action), 1);
  QDEF(cMainWindow, "centralWidget", RUBY_METHOD_FUNC(mainwindow_central_widget), 0);
  QDEF(cSplitter, "setOrientation",  RUBY_METHOD_FUNC(splitter_set_orientation), 1);
  QDEF(cSplitter, "setSizes",        RUBY_METHOD_FUNC(splitter_set_sizes), 1);
  QDEF(cMenu,     "setText",         RUBY_METHOD_FUNC(menu_set_text), 1);
  QDEF(cMenu,     "addMenu",         RUBY_METHOD_FUNC(menu_add_menu), -1);
  QDEF(cMenu,     "setIcon",         RUBY_METHOD_FUNC(menu_set_icon), 1);
  QDEF(cMenu,     "addActions",      RUBY_METHOD_FUNC(menu_add_actions), 1);
  QDEF(cDialog,   "setModal",        RUBY_METHOD_FUNC(dialog_set_modal), 1);
  QDEF(cWidget, "setFocus",        RUBY_METHOD_FUNC(widget_set_focus), -1);
  QDEF(cWidget, "setWindowFlags",  RUBY_METHOD_FUNC(widget_set_window_flags), 1);
  QDEF(cWidget, "removeAction",    RUBY_METHOD_FUNC(widget_remove_action), 1);
  QDEF(cMenu,   "insertSeparator", RUBY_METHOD_FUNC(menu_insert_separator), 1);
  QDEF(cImage,  "bits",            RUBY_METHOD_FUNC(image_bits), 0);

  // Qt::TextCursor gets real behaviour (it was constants-only)
  cTextCursorKlass = cTextCursor;
  rb_define_singleton_method(cTextCursor, "new", RUBY_METHOD_FUNC(tc_new), -1);
  QDEF(cTextCursor, "movePosition", RUBY_METHOD_FUNC(tc_move_position), -1);
  QDEF(cTextCursor, "insertText",   RUBY_METHOD_FUNC(tc_insert_text), 1);
  QDEF(cTextCursor, "setPosition",  RUBY_METHOD_FUNC(tc_set_position), -1);
  QDEF(cTextCursor, "position",     RUBY_METHOD_FUNC(tc_position), 0);
  QDEF(cTextCursor, "blockNumber",  RUBY_METHOD_FUNC(tc_block_number), 0);
  QDEF(cTextCursor, "selectedText",       RUBY_METHOD_FUNC(tc_selected_text), 0);
  QDEF(cTextCursor, "positionInBlock",    RUBY_METHOD_FUNC(tc_position_in_block), 0);
  QDEF(cTextCursor, "anchor",             RUBY_METHOD_FUNC(tc_anchor), 0);
  QDEF(cTextCursor, "selectionStart",     RUBY_METHOD_FUNC(tc_selection_start), 0);
  QDEF(cTextCursor, "selectionEnd",       RUBY_METHOD_FUNC(tc_selection_end), 0);
  QDEF(cTextCursor, "selectedText",       RUBY_METHOD_FUNC(tc_selection_text), 0);
  QDEF(cTextCursor, "atBlockStart",       RUBY_METHOD_FUNC(tc_at_block_start), 0);
  QDEF(cTextCursor, "atBlockEnd",         RUBY_METHOD_FUNC(tc_at_block_end), 0);
  QDEF(cTextCursor, "deletePreviousChar", RUBY_METHOD_FUNC(tc_delete_prev_char), 0);
  QDEF(cTextCursor, "deleteChar",         RUBY_METHOD_FUNC(tc_delete_char), 0);
  QDEF(cTextCursor, "selectAll",          RUBY_METHOD_FUNC(tc_select_all), 0);
  QDEF(cTextCursor, "hasSelection",       RUBY_METHOD_FUNC(tc_has_selection), 0);
  QDEF(cTextCursor, "clearSelection",     RUBY_METHOD_FUNC(tc_clear_selection), 0);
  QDEF(cTextCursor, "removeSelectedText", RUBY_METHOD_FUNC(tc_remove_selected), 0);
  QDEF(cTextCursor, "beginEditBlock",     RUBY_METHOD_FUNC(tc_begin_edit), 0);
  QDEF(cTextCursor, "endEditBlock",       RUBY_METHOD_FUNC(tc_end_edit), 0);
  QDEF(cTextCursor, "atEnd",              RUBY_METHOD_FUNC(tc_at_end), 0);
  QDEF(cTextCursor, "atStart",            RUBY_METHOD_FUNC(tc_at_start), 0);
  QDEF(cTextCursor, "blockText",          RUBY_METHOD_FUNC(tc_block_text), 0);
  QDEF(cTextCursor, "block",              RUBY_METHOD_FUNC(tc_block), 0);

  cTextBlockKlass = rb_define_class_under(mQt, "TextBlock", rb_cObject);
  QDEF(cTextBlockKlass, "text",        RUBY_METHOD_FUNC(tb_text), 0);
  QDEF(cTextBlockKlass, "blockNumber", RUBY_METHOD_FUNC(tb_number), 0);
  QDEF(cTextBlockKlass, "isVisible",   RUBY_METHOD_FUNC(textblock_is_visible), 0);
  QDEF(cTextBlockKlass, "dispose",     RUBY_METHOD_FUNC(value_dispose_noop), 0);
  QDEF(cTextBlockKlass, "position",    RUBY_METHOD_FUNC(tb_position), 0);
  QDEF(cTextBlockKlass, "length",      RUBY_METHOD_FUNC(tb_length), 0);
  QDEF(cTextBlockKlass, "isValid",     RUBY_METHOD_FUNC(tb_valid), 0);
  QDEF(cTextBlockKlass, "next",        RUBY_METHOD_FUNC(tb_next), 0);
  QDEF(cTextBlockKlass, "previous",    RUBY_METHOD_FUNC(tb_previous), 0);
  QDEF(cTextBlockKlass, "setUserState",    RUBY_METHOD_FUNC(tb_set_user_state), 1);
  QDEF(cTextBlockKlass, "userState",       RUBY_METHOD_FUNC(tb_user_state), 0);
  QDEF(cTextBlockKlass, "firstLineNumber", RUBY_METHOD_FUNC(tb_first_line_number), 0);
  QDEF(cTextCursor, "select",       RUBY_METHOD_FUNC(tc_select), 1);
  rb_define_const(cTextCursor, "Document",    INT2NUM((int)QTextCursor::Document));
  rb_define_const(cTextCursor, "BlockUnderCursor", INT2NUM((int)QTextCursor::BlockUnderCursor));
  rb_define_const(cTextCursor, "LineUnderCursor",  INT2NUM((int)QTextCursor::LineUnderCursor));

  QDEF(cTextEdit,      "textCursor",          RUBY_METHOD_FUNC(te_text_cursor), 0);
  QDEF(cTextEdit,      "setTextCursor",       RUBY_METHOD_FUNC(te_set_text_cursor), 1);
  QDEF(cTextEdit,      "ensureCursorVisible", RUBY_METHOD_FUNC(te_ensure_cursor_visible), 0);
  QDEF(cTextEdit,      "setWordWrapMode",     RUBY_METHOD_FUNC(te_set_word_wrap_mode), 1);
  QDEF(cPlainTextEdit, "textCursor",          RUBY_METHOD_FUNC(te_text_cursor), 0);
  QDEF(cPlainTextEdit, "setTextCursor",       RUBY_METHOD_FUNC(te_set_text_cursor), 1);
  QDEF(cPlainTextEdit, "ensureCursorVisible", RUBY_METHOD_FUNC(te_ensure_cursor_visible), 0);
  QDEF(cPlainTextEdit, "setWordWrapMode",     RUBY_METHOD_FUNC(te_set_word_wrap_mode), 1);
  QDEF(cPlainTextEdit, "setMaximumBlockCount",RUBY_METHOD_FUNC(pte_set_max_blocks), 1);
  QDEF(cPlainTextEdit, "setLineWrapMode",     RUBY_METHOD_FUNC(pte_set_wrap_mode), 1);
  QDEF(cPlainTextEdit, "document",            RUBY_METHOD_FUNC(te_document), 0);
  QDEF(cTextEdit,      "document",            RUBY_METHOD_FUNC(te_document), 0);
  QDEF(cTextEdit,      "setExtraSelections",  RUBY_METHOD_FUNC(te_set_extra_selections), 1);
  QDEF(cPlainTextEdit, "setExtraSelections",  RUBY_METHOD_FUNC(te_set_extra_selections), 1);
  // QTextEdit::ExtraSelection {cursor, format}
  rb_eval_string(
    "module Qt\n"
    "  class TextEdit\n"
    "    class ExtraSelection\n"
    "      attr_accessor :cursor, :format\n"
    "      def initialize\n"
    "        @cursor = Qt::TextCursor.new\n"
    "        @format = Qt::TextCharFormat.new\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  class PlainTextEdit\n"
    "    ExtraSelection = Qt::TextEdit::ExtraSelection\n"
    "  end\n"
    "end\n");

  QDEF(cPushButton,  "setAutoDefault",   RUBY_METHOD_FUNC(button_set_auto_default), 1);
  QDEF(cListWidget,  "setCurrentRow",    RUBY_METHOD_FUNC(lw_set_current_row), 1);
  QDEF(cListWidget,  "currentRow",       RUBY_METHOD_FUNC(lw_current_row), 0);
  QDEF(cListWidget,  "setSortingEnabled",RUBY_METHOD_FUNC(lw_set_sorting_enabled), 1);
  QDEF(cTableWidget, "setCellWidget",    RUBY_METHOD_FUNC(tw_set_cell_widget), 3);
  QDEF(cTableWidget, "cellWidget",       RUBY_METHOD_FUNC(tw_cell_widget), 2);
  QDEF(cTableWidget, "rowAt",            RUBY_METHOD_FUNC(view_row_at), 1);
  QDEF(cTableWidget, "horizontalHeaderItem",   RUBY_METHOD_FUNC(tw_h_header_item), 1);
  QDEF(cTableWidget, "verticalHeaderItem",     RUBY_METHOD_FUNC(tw_v_header_item), 1);
  QDEF(cTableWidget, "setVerticalHeaderLabels", RUBY_METHOD_FUNC(tw_set_v_header_labels), 1);
  QDEF(cTableWidget, "setRowHidden",     RUBY_METHOD_FUNC(tw_set_row_hidden), 2);
  QDEF(cTableWidget, "selectRow",        RUBY_METHOD_FUNC(tw_select_row), 1);
  QDEF(cTableWidget, "rowHeight",        RUBY_METHOD_FUNC(tw_row_height), 1);
  QDEF(cTableWidget, "setSpan",          RUBY_METHOD_FUNC(tw_set_span), 4);
  QDEF(cTableWidget, "columnAt",         RUBY_METHOD_FUNC(view_column_at), 1);
  QDEF(cTableWidget, "removeCellWidget", RUBY_METHOD_FUNC(tw_remove_cell_widget), 2);
  QDEF(cTableWidget, "clearContents",    RUBY_METHOD_FUNC(tw_clear_contents), 0);
  QDEF(cTableWidget, "resizeRowsToContents", RUBY_METHOD_FUNC(table_resize_rows), 0);
  QDEF(cTableWidget, "setSortingEnabled",RUBY_METHOD_FUNC(tw_set_sorting_enabled), 1);
  QDEF(cTabWidget,   "removeTab",        RUBY_METHOD_FUNC(tab_remove_tab), 1);
  QDEF(cTabWidget,   "setMovable",       RUBY_METHOD_FUNC(tab_set_movable), 1);
  QDEF(cTabWidget,   "widget",           RUBY_METHOD_FUNC(tab_widget_at), 1);
  QDEF(cComboBox,    "findText",         RUBY_METHOD_FUNC(combo_find_text), 1);
  QDEF(cComboBox,    "setCompleter",     RUBY_METHOD_FUNC(combo_set_completer), 1);
#define DEF_SA(n) rb_define_const(cComboBox, #n, INT2NUM((int)QComboBox::n))
  DEF_SA(AdjustToContents); DEF_SA(AdjustToContentsOnFirstShow); DEF_SA(AdjustToMinimumContentsLengthWithIcon);
#undef DEF_SA
  QDEF(cAbstractItemView, "setDragDropMode", RUBY_METHOD_FUNC(aiv_set_drag_drop_mode), 1);
  QDEF(cHeaderViewKlass, "setStretchLastSection", RUBY_METHOD_FUNC(header_set_stretch_last), 1);
  QDEF(cHeaderViewKlass, "length", RUBY_METHOD_FUNC(header_length), 0);
  QDEF(cToolBar,     "setMovable",       RUBY_METHOD_FUNC(toolbar_set_movable), 1);
  QDEF(cLayout,      "removeWidget",     RUBY_METHOD_FUNC(layout_remove_widget), 1);
  QDEF(cCompleter,   "setCompletionPrefix", RUBY_METHOD_FUNC(completer_set_prefix), 1);
  QDEF(cPainter,     "drawPolygon",      RUBY_METHOD_FUNC(painter_draw_polygon), 1);

  // QKeySequence standard keys (Qt::KeySequence::New etc.)
#define DEF_SK(n) rb_define_const(cKeySequence, #n, INT2NUM((int)QKeySequence::n))
  DEF_SK(New); DEF_SK(Open); DEF_SK(Save); DEF_SK(SaveAs); DEF_SK(Close);
  DEF_SK(Quit); DEF_SK(Cut); DEF_SK(Copy); DEF_SK(Paste); DEF_SK(Undo);
  DEF_SK(Redo); DEF_SK(Find); DEF_SK(FindNext); DEF_SK(SelectAll); DEF_SK(Delete);
  DEF_SK(HelpContents); DEF_SK(Print); DEF_SK(ZoomIn); DEF_SK(ZoomOut);
#undef DEF_SK

  VALUE cActionGroupK = rb_const_get(mQt, rb_intern("ActionGroup"));
  register_ctor(cActionGroupK, ctor_action_group);
  QDEF(cActionGroupK, "addAction",    RUBY_METHOD_FUNC(actiongroup_add_action), 1);
  QDEF(cActionGroupK, "setExclusive", RUBY_METHOD_FUNC(actiongroup_set_exclusive), 1);
  QDEF(cActionGroupK, "actions",      RUBY_METHOD_FUNC(actiongroup_actions), 0);

  // ---- scoped enum constants COSMOS references --------------------------
  // Found by scanning every Qt::X::Y reference in lib/ against the binding.
  rb_define_const(cDialog, "Accepted", INT2NUM((int)QDialog::Accepted));
  rb_define_const(cDialog, "Rejected", INT2NUM((int)QDialog::Rejected));

#define DEF_MBI(n) rb_define_const(cMessageBox, #n, INT2NUM((int)QMessageBox::n))
  DEF_MBI(NoIcon); DEF_MBI(Information); DEF_MBI(Warning); DEF_MBI(Critical);
  DEF_MBI(Question); DEF_MBI(NoButton);
  DEF_MBI(AcceptRole); DEF_MBI(RejectRole); DEF_MBI(DestructiveRole);
  DEF_MBI(ActionRole); DEF_MBI(HelpRole); DEF_MBI(YesRole); DEF_MBI(NoRole);
  DEF_MBI(ResetRole); DEF_MBI(ApplyRole);
#undef DEF_MBI

#define DEF_EVT(n) rb_define_const(cEventK, #n, INT2NUM((int)QEvent::n))
  VALUE cEventK = rb_const_get(mQt, rb_intern("Event"));
  DEF_EVT(KeyPress); DEF_EVT(KeyRelease); DEF_EVT(MouseButtonPress);
  DEF_EVT(MouseButtonRelease); DEF_EVT(MouseMove); DEF_EVT(Close);
  DEF_EVT(Resize); DEF_EVT(Paint); DEF_EVT(FocusIn); DEF_EVT(FocusOut);
#undef DEF_EVT

  rb_define_const(cEventLoop, "AllEvents",        INT2NUM((int)QEventLoop::AllEvents));
  rb_define_const(cEventLoop, "ExcludeUserInputEvents",
                  INT2NUM((int)QEventLoop::ExcludeUserInputEvents));

#define DEF_FL(n) rb_define_const(cFormLayout, #n, INT2NUM((int)QFormLayout::n))
  DEF_FL(LabelRole); DEF_FL(FieldRole); DEF_FL(SpanningRole);
#undef DEF_FL

#define DEF_ICO(n) rb_define_const(cIcon, #n, INT2NUM((int)QIcon::n))
  DEF_ICO(Normal); DEF_ICO(Disabled); DEF_ICO(Active); DEF_ICO(Selected);
  DEF_ICO(On); DEF_ICO(Off);
#undef DEF_ICO

#define DEF_IMG(n) rb_define_const(cImage, #n, INT2NUM((int)QImage::n))
  DEF_IMG(Format_RGB32); DEF_IMG(Format_ARGB32); DEF_IMG(Format_ARGB32_Premultiplied);
  DEF_IMG(Format_RGB888); DEF_IMG(Format_Indexed8);
#undef DEF_IMG

  rb_define_const(cKeySequence, "NextChild",     INT2NUM((int)QKeySequence::NextChild));
  rb_define_const(cKeySequence, "PreviousChild", INT2NUM((int)QKeySequence::PreviousChild));

  rb_define_const(cListView, "Adjust", INT2NUM((int)QListView::Adjust));
  rb_define_const(cListView, "Fixed",  INT2NUM((int)QListView::Fixed));

#define DEF_TICK(n) rb_define_const(cSlider, #n, INT2NUM((int)QSlider::n))
  DEF_TICK(NoTicks); DEF_TICK(TicksAbove); DEF_TICK(TicksBelow); DEF_TICK(TicksBothSides);
#undef DEF_TICK

#define DEF_CE(n) rb_define_const(cStyleK, #n, INT2NUM((int)QStyle::n))
  DEF_CE(CE_PushButton); DEF_CE(CE_PushButtonLabel); DEF_CE(CE_CheckBox);
  DEF_CE(CE_ItemViewItem);
#undef DEF_CE

#define DEF_FIND(n) rb_define_const(cTextDocument, #n, INT2NUM((int)QTextDocument::n))
  DEF_FIND(FindBackward); DEF_FIND(FindCaseSensitively); DEF_FIND(FindWholeWords);
#undef DEF_FIND

  rb_define_const(cCalendarWidget, "NoVerticalHeader",
                  INT2NUM((int)QCalendarWidget::NoVerticalHeader));
  rb_define_const(cCalendarWidget, "NoHorizontalHeader",
                  INT2NUM((int)QCalendarWidget::NoHorizontalHeader));

  rb_define_singleton_method(cPixmap, "fromImage",  RUBY_METHOD_FUNC(pixmap_from_image), 1);
  rb_define_singleton_method(cPixmap, "grabWidget", RUBY_METHOD_FUNC(pixmap_grab_widget), -1);
  rb_define_singleton_method(cApplication, "startDragDistance",
                             RUBY_METHOD_FUNC(app_start_drag_distance), 0);

  // ---- Qt class name -> Ruby class (for findChildren typing) -------------
  register_qt_class("QWidget", cWidget);
  register_qt_class("QFrame", cFrame);
  register_qt_class("QLabel", cLabel);
  register_qt_class("QAbstractButton", cAbstractButton);
  register_qt_class("QPushButton", cPushButton);
  register_qt_class("QCheckBox", cCheckBox);
  register_qt_class("QRadioButton", cRadioButton);
  register_qt_class("QLineEdit", cLineEdit);
  register_qt_class("QComboBox", cComboBox);
  register_qt_class("QPlainTextEdit", cPlainTextEdit);
  register_qt_class("QTextEdit", cTextEdit);
  register_qt_class("QGroupBox", cGroupBox);
  register_qt_class("QDialog", cDialog);
  register_qt_class("QMainWindow", cMainWindow);
  register_qt_class("QTableWidget", cTableWidget);
  register_qt_class("QTreeWidget", cTreeWidget);
  register_qt_class("QListWidget", cListWidget);
  register_qt_class("QTabWidget", cTabWidget);
  register_qt_class("QSplitter", cSplitter);
  register_qt_class("QScrollArea", cScrollArea);
  register_qt_class("QMenu", cMenu);
  register_qt_class("QMenuBar", cMenuBar);
  register_qt_class("QToolBar", cToolBar);
  register_qt_class("QStatusBar", cStatusBar);
  register_qt_class("QProgressBar", cProgressBar);
  register_qt_class("QSlider", cSlider);
  register_qt_class("QSpinBox", cSpinBox);
  register_qt_class("QDoubleSpinBox", cDoubleSpinBox);
  register_qt_class("QAction", cAction);
  register_qt_class("QTimer", cTimer);

  // qtbindings compatibility shims (Enum, Boolean, RubyThreadFix, ...)
  rb_eval_string(COMPAT_RUBY);
}
