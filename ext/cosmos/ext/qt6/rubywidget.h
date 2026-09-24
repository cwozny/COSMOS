#ifndef RUBYWIDGET_H
#define RUBYWIDGET_H

#include <QWidget>
#include <QModelIndex>
#include <functional>
#include <cstring>
#include <atomic>
// The RubyForward template dereferences these, so full definitions are needed
// in the header rather than the forward declarations Qt would otherwise give.
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QFocusEvent>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
class QPainter;
class QAbstractItemModel;
#include <QMainWindow>
#include <QDialog>
#include <QOpenGLWidget>
#include <QStyledItemDelegate>

#ifndef Q_MOC_RUN
#  include <ruby.h>
#else
   typedef unsigned long VALUE;
#endif

// Runs fn with the GVL held (defined in rubycallback.cpp). Qt's event loop
// runs with the GVL released, so a callback must wrap its whole body -- the
// VALUEs it builds for arguments included -- in this.
void ruby_with_gvl(const std::function<void()> &fn);

// While a Qt virtual is being dispatched into Ruby, this holds a thunk that
// invokes the C++ base-class implementation. COSMOS overrides call `super`
// (25 event handlers do), and without something for super to reach that is a
// NoMethodError. Calling it runs Qt's default; it self-clears so a handler
// cannot invoke the base twice.
QEvent *ruby_call_base_event();

// RAII: installs the base-class thunk for the duration of one dispatch, so a
// Ruby override's `super` can run Qt's default. Restores the previous thunk,
// so nested dispatch is safe.
struct BaseEventScopeT {
  std::function<void()> saved;
  QEvent *saved_event;
  // e: the real event the base runs on, reported back to super (see
  // ruby_call_base_event).
  BaseEventScopeT(std::function<void()> f, QEvent *e);
  ~BaseEventScopeT();
};

// Which of an object's forwarded virtuals Ruby overrides, cached so that an
// event with no override runs Qt's default without the GVL. Every event used
// to take it -- each paint, resize and show of every Ruby-created Qt::Label,
// since the binding's own pass-through (qt_base_event, there so `super`
// works) made every widget look overridden -- and with a busy Ruby thread
// each acquisition waits out that thread's time slice. lib/Qt.rb invalidates
// every cache when an override can have appeared or gone (a method defined or
// removed on a Qt class or object; a module included, prepended or extended
// into one), and the next event recomputes under the GVL.
struct RubyOverrides {
  std::atomic<unsigned long> epoch{0};   // 0: not computed yet
  std::atomic<unsigned> mask{0};
};
// False when obj's Ruby object does not override `method`: the caller runs
// Qt's default and never takes the GVL. True otherwise, and for a name not
// tracked here, when ruby_event_dispatch decides as before.
bool ruby_overrides(const QObject *obj, RubyOverrides &cache, const char *method);

// Invokes a Ruby-side override of a Qt virtual, if the wrapper defines one.
// Returns true when Ruby handled the event, false to fall through to Qt's
// default implementation. Defined in cosmos_qt6.cpp, where the object map and
// the GVL helpers live.
bool ruby_event_dispatch(QObject *obj, const char *method, VALUE arg);
bool ruby_event_dispatch_n(QObject *obj, const char *method, int argc, VALUE *argv);
VALUE ruby_event_call(QObject *obj, const char *method, int argc, VALUE *argv, bool *handled);
VALUE ruby_wrap_model_index(const QModelIndex &idx);
VALUE ruby_wrap_style_option_view_item(const void *opt);
VALUE ruby_wrap_painter_borrowed(QPainter *p);
VALUE ruby_wrap_qobject(QObject *o);
QWidget *ruby_unwrap_widget(VALUE v);
// Which snapshot class a forwarded event should be built as.
enum RubyEventKind {
  RUBY_EV_PLAIN = 0, RUBY_EV_CLOSE, RUBY_EV_SHOW, RUBY_EV_RESIZE,
  RUBY_EV_FOCUS, RUBY_EV_LEAVE, RUBY_EV_DRAGENTER, RUBY_EV_DRAGMOVE, RUBY_EV_DROP
};

// Qt events reach Ruby as snapshots, never as pointers -- the QEvent is gone
// once dispatch returns, so a handler that stored one would hold freed memory.
// accept/ignore is read back off the snapshot and applied to the real event.
VALUE ruby_make_plain_event(int kind, int type);
VALUE ruby_make_paint_event(int x, int y, int w, int h, int type);
VALUE ruby_make_wheel_event(int dx, int dy, int mods, int type);
VALUE ruby_make_key_event(int key, const char *text, int mods, int type);
VALUE ruby_make_mouse_event(int x, int y, int button, int buttons, int mods, int type);
VALUE ruby_make_drop_event(int kind, const void *mime, int type);
bool  ruby_event_accepted(VALUE ev);
bool  ruby_event_proposed(VALUE ev);

// QWidget subclass that forwards the virtuals COSMOS overrides in Ruby.
// Without this, a Ruby `def paintEvent` is simply never called and nothing
// draws -- which is what lib/cosmos/gui/line_graph depends on.
// QOpenGLWidget subclass forwarding the GL virtuals COSMOS overrides in
// lib/cosmos/gui/opengl/gl_viewer.rb. QGLWidget was removed in Qt6; the GL
// calls themselves come from the opengl-bindings gem, so Qt only has to
// supply a current context at the right moments.
class RubyGLWidget : public QOpenGLWidget {
  Q_OBJECT
public:
  explicit RubyGLWidget(QWidget *parent = nullptr) : QOpenGLWidget(parent) {}
protected:
  void initializeGL() override;
  void resizeGL(int w, int h) override;
  void paintGL() override;
  // gl_viewer.rb:590-658 overrides these for rotate / pan / zoom / pick;
  // QOpenGLWidget's own virtuals were the only ones forwarded, so 3D input
  // in Telemetry Viewer's GL screens and OpenGL Builder did nothing.
  void mousePressEvent(QMouseEvent *e) override;
  void mouseReleaseEvent(QMouseEvent *e) override;
  void mouseMoveEvent(QMouseEvent *e) override;
  void wheelEvent(QWheelEvent *e) override;


private:
  mutable RubyOverrides m_overrides;
};

// QStyledItemDelegate forwarding the virtuals COSMOS overrides in
// cmd_sender/cmd_param_table_item_delegate.rb and table_manager.rb.
class RubyItemDelegate : public QStyledItemDelegate {
  Q_OBJECT
public:
  explicit RubyItemDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

  QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &opt,
                        const QModelIndex &idx) const override;
  void setEditorData(QWidget *editor, const QModelIndex &idx) const override;
  void setModelData(QWidget *editor, QAbstractItemModel *model,
                    const QModelIndex &idx) const override;
  void paint(QPainter *p, const QStyleOptionViewItem &opt,
             const QModelIndex &idx) const override;


private:
  mutable RubyOverrides m_overrides;
};

class RubyWidget : public QWidget {
  Q_OBJECT
public:
  explicit RubyWidget(QWidget *parent = nullptr) : QWidget(parent) {}

protected:
  void paintEvent(QPaintEvent *e) override;
  void resizeEvent(QResizeEvent *e) override;
  void mousePressEvent(QMouseEvent *e) override;
  void mouseMoveEvent(QMouseEvent *e) override;
  void mouseReleaseEvent(QMouseEvent *e) override;
  void keyPressEvent(QKeyEvent *e) override;
  void leaveEvent(QEvent *e) override;
  void focusInEvent(QFocusEvent *e) override;
  void focusOutEvent(QFocusEvent *e) override;
  void closeEvent(QCloseEvent *e) override;
  void wheelEvent(QWheelEvent *e) override;
  void showEvent(QShowEvent *e) override;
  void dragEnterEvent(QDragEnterEvent *e) override;
  void dragMoveEvent(QDragMoveEvent *e) override;
  void dropEvent(QDropEvent *e) override;


private:
  mutable RubyOverrides m_overrides;
};

// Ruby overrides of Qt virtuals only reach Ruby through a C++ subclass that
// forwards them, and previously only Qt::Widget had one. 19 COSMOS files
// override closeEvent -- 13 of them on Qt::MainWindow (QtTool and friends) and
// 3 on Qt::Dialog -- so without these the tools never save config or stop
// their threads on window close. Same dispatch helpers as RubyWidget.
class RubyMainWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit RubyMainWindow(QWidget *parent = nullptr) : QMainWindow(parent) {}

protected:
  void closeEvent(QCloseEvent *e) override;
  void showEvent(QShowEvent *e) override;
  void resizeEvent(QResizeEvent *e) override;
  void keyPressEvent(QKeyEvent *e) override;
  void dragEnterEvent(QDragEnterEvent *e) override;
  void dragMoveEvent(QDragMoveEvent *e) override;
  void dropEvent(QDropEvent *e) override;


private:
  mutable RubyOverrides m_overrides;
};

class RubyDialog : public QDialog {
  Q_OBJECT
public:
  explicit RubyDialog(QWidget *parent = nullptr) : QDialog(parent) {}
  // QDialog's close handling and Esc call this virtual; see rubywidget.cpp.
  void reject() override;

protected:
  void closeEvent(QCloseEvent *e) override;
  void showEvent(QShowEvent *e) override;
  void resizeEvent(QResizeEvent *e) override;
  void keyPressEvent(QKeyEvent *e) override;


private:
  mutable RubyOverrides m_overrides;
};

// Event forwarding for every other Qt class COSMOS subclasses with an event
// override. Only Widget/MainWindow/Dialog/GLWidget/StyledItemDelegate had a
// forwarding subclass, so overrides on Qt::LineEdit, Qt::ComboBox,
// Qt::ListWidget, Qt::TreeView, Qt::Label, Qt::Frame, Qt::TextEdit and
// Qt::PlainTextEdit silently never fired -- e.g. the search box's arrow-key
// navigation and focus-out hiding (full_text_search_line_edit.rb:45,54).
//
// A template, not Q_OBJECT subclasses: these need virtual overrides only, no
// meta-object of their own, and moc cannot process templates anyway.
template <typename Base>
class RubyForward : public Base {
public:
  template <typename... Args>
  explicit RubyForward(Args &&... args) : Base(std::forward<Args>(args)...) {}

protected:
  // Dispatch with a real event object and apply the handler's accept/ignore
  // back onto the QEvent. Passing Qnil here made `event.ignore()` raise
  // NoMethodError on nil; the raise was swallowed, so a closeEvent override
  // could not cancel a close and 16 COSMOS handlers were silently no-ops.
  bool fwd_ev(const char *method, int kind, QEvent *e) {
    if (!ruby_overrides(this, m_overrides, method)) return false;
    bool handled = false, accepted = true;
    const int type = e ? (int)e->type() : 0;
    ruby_with_gvl([&] {
      VALUE ev = ruby_make_plain_event(kind, type);
      handled  = ruby_event_dispatch(this, method, ev);
      accepted = ruby_event_accepted(ev);
    });
    if (handled && e) e->setAccepted(accepted);
    return handled;
  }

  void paintEvent(QPaintEvent *e) override {
    if (!ruby_overrides(this, m_overrides, "paintEvent")) { Base::paintEvent(e); return; }
    BaseEventScopeT scope([this, e] { Base::paintEvent(e); }, e);
    bool handled = false;
    const QRect r = e->rect();
    const int type = (int)e->type();
    ruby_with_gvl([&] {
      handled = ruby_event_dispatch(
        this, "paintEvent",
        ruby_make_paint_event(r.x(), r.y(), r.width(), r.height(), type));
    });
    if (!handled) Base::paintEvent(e);
  }
  void resizeEvent(QResizeEvent *e) override {
    BaseEventScopeT scope([this, e] { Base::resizeEvent(e); }, e);
    if (!fwd_ev("resizeEvent", RUBY_EV_RESIZE, e)) Base::resizeEvent(e);
  }
  void closeEvent(QCloseEvent *e) override {
    BaseEventScopeT scope([this, e] { Base::closeEvent(e); }, e);
    if (!fwd_ev("closeEvent", RUBY_EV_CLOSE, e)) Base::closeEvent(e);
  }
  void showEvent(QShowEvent *e) override {
    BaseEventScopeT scope([this, e] { Base::showEvent(e); }, e);
    if (!fwd_ev("showEvent", RUBY_EV_SHOW, e)) Base::showEvent(e);
  }
  void leaveEvent(QEvent *e) override {
    BaseEventScopeT scope([this, e] { Base::leaveEvent(e); }, e);
    if (!fwd_ev("leaveEvent", RUBY_EV_LEAVE, e)) Base::leaveEvent(e);
  }
  void focusInEvent(QFocusEvent *e) override {
    BaseEventScopeT scope([this, e] { Base::focusInEvent(e); }, e);
    if (!fwd_ev("focusInEvent", RUBY_EV_FOCUS, e)) Base::focusInEvent(e);
  }
  void focusOutEvent(QFocusEvent *e) override {
    BaseEventScopeT scope([this, e] { Base::focusOutEvent(e); }, e);
    if (!fwd_ev("focusOutEvent", RUBY_EV_FOCUS, e)) Base::focusOutEvent(e);
  }
  // event.ignore() in a wheelEvent means "let the parent scroll instead"
  // (cmd_tlm_server_gui.rb:90), so a handled event must NOT fall through to
  // the base implementation, which would accept and absorb the wheel.
  void wheelEvent(QWheelEvent *e) override {
    if (!ruby_overrides(this, m_overrides, "wheelEvent")) { Base::wheelEvent(e); return; }
    BaseEventScopeT scope([this, e] { Base::wheelEvent(e); }, e);
    bool handled = false, accepted = true;
    const int dx = e->angleDelta().x(), dy = e->angleDelta().y();
    const int mods = (int)e->modifiers(), type = (int)e->type();
    ruby_with_gvl([&] {
      VALUE ev = ruby_make_wheel_event(dx, dy, mods, type);
      handled  = ruby_event_dispatch(this, "wheelEvent", ev);
      accepted = ruby_event_accepted(ev);
    });
    if (handled) e->setAccepted(accepted);
    else         Base::wheelEvent(e);
  }
  void keyPressEvent(QKeyEvent *e) override {
    if (!ruby_overrides(this, m_overrides, "keyPressEvent")) { Base::keyPressEvent(e); return; }
    BaseEventScopeT scope([this, e] { Base::keyPressEvent(e); }, e);
    bool handled = false, accepted = true;
    const int key = e->key(), mods = (int)e->modifiers(), type = (int)e->type();
    const QByteArray txt = e->text().toUtf8();
    ruby_with_gvl([&] {
      VALUE ev = ruby_make_key_event(key, txt.constData(), mods, type);
      handled  = ruby_event_dispatch(this, "keyPressEvent", ev);
      accepted = ruby_event_accepted(ev);
    });
    if (handled) e->setAccepted(accepted);
    else         Base::keyPressEvent(e);
  }
  void mousePressEvent(QMouseEvent *e) override   { fwd_mouse("mousePressEvent", e); }
  void mouseMoveEvent(QMouseEvent *e) override    { fwd_mouse("mouseMoveEvent", e); }
  void mouseReleaseEvent(QMouseEvent *e) override { fwd_mouse("mouseReleaseEvent", e); }

  // setAcceptDrops(true) already worked, so these widgets advertised drop
  // support while the Ruby overrides were never reached.
  void dragEnterEvent(QDragEnterEvent *e) override {
    BaseEventScopeT scope([this, e] { Base::dragEnterEvent(e); }, e);
    if (!fwd_dnd("dragEnterEvent", RUBY_EV_DRAGENTER, e)) Base::dragEnterEvent(e);
  }
  void dragMoveEvent(QDragMoveEvent *e) override {
    BaseEventScopeT scope([this, e] { Base::dragMoveEvent(e); }, e);
    if (!fwd_dnd("dragMoveEvent", RUBY_EV_DRAGMOVE, e)) Base::dragMoveEvent(e);
  }
  void dropEvent(QDropEvent *e) override {
    BaseEventScopeT scope([this, e] { Base::dropEvent(e); }, e);
    if (!fwd_dnd("dropEvent", RUBY_EV_DROP, e)) Base::dropEvent(e);
  }

private:
  bool fwd_dnd(const char *method, int kind, QDropEvent *e) {
    if (!ruby_overrides(this, m_overrides, method)) return false;
    bool handled = false, accepted = true, proposed = false;
    const QMimeData *md = e->mimeData();
    const int type = (int)e->type();
    ruby_with_gvl([&] {
      VALUE ev = ruby_make_drop_event(kind, md, type);
      handled  = ruby_event_dispatch(this, method, ev);
      accepted = ruby_event_accepted(ev);
      proposed = ruby_event_proposed(ev);
    });
    if (!handled) return false;
    if (proposed) e->acceptProposedAction();
    else          e->setAccepted(accepted);
    return true;
  }

  void fwd_mouse(const char *method, QMouseEvent *e) {
    if (!ruby_overrides(this, m_overrides, method)) { base_mouse(method, e); return; }
    BaseEventScopeT scope([this, method, e] { base_mouse(method, e); }, e);
    bool handled = false, accepted = true;
    const int x = (int)e->position().x(), y = (int)e->position().y();
    const int b = (int)e->button(), bs = (int)e->buttons();
    const int mods = (int)e->modifiers(), type = (int)e->type();
    ruby_with_gvl([&] {
      VALUE ev = ruby_make_mouse_event(x, y, b, bs, mods, type);
      handled  = ruby_event_dispatch(this, method, ev);
      accepted = ruby_event_accepted(ev);
    });
    if (handled) e->setAccepted(accepted);
    else         base_mouse(method, e);
  }
  void base_mouse(const char *method, QMouseEvent *e) {
    if (!strcmp(method, "mousePressEvent"))        Base::mousePressEvent(e);
    else if (!strcmp(method, "mouseMoveEvent"))    Base::mouseMoveEvent(e);
    else                                            Base::mouseReleaseEvent(e);
  }

  mutable RubyOverrides m_overrides;
};

#endif
