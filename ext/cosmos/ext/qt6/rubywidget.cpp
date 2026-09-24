#include "rubywidget.h"
#include <QPaintEvent>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QFocusEvent>
#include <QPainter>
#include <QAbstractItemModel>

// Qt calls everything in this file from its event loop, which app.exec() runs
// with the GVL released. Every VALUE below -- the arguments as much as the
// call itself -- must therefore be built inside ruby_with_gvl. Constructing an
// argument first and acquiring the GVL afterwards allocates a Ruby object
// without the GVL: that corrupts the Ruby heap, and the [BUG] surfaces later
// on whichever thread allocates next (typically a COSMOS worker thread), which
// is what made the failure look unrelated to painting.

// Set for the duration of one virtual dispatch; see ruby_call_base_event.
static thread_local std::function<void()> t_base_event;
static thread_local QEvent *t_base_qevent = nullptr;

BaseEventScopeT::BaseEventScopeT(std::function<void()> f, QEvent *e)
    : saved(t_base_event), saved_event(t_base_qevent) {
  t_base_event = f;
  t_base_qevent = e;
}
BaseEventScopeT::~BaseEventScopeT() {
  t_base_event = saved;
  t_base_qevent = saved_event;
}

typedef BaseEventScopeT BaseEventScope;   // name used by the hand-written virtuals

// Returns the event the base ran on, so super can copy back what Qt did to
// it (see qt_base_event); nullptr if there was no base to run.
QEvent *ruby_call_base_event() {
  if (!t_base_event) return nullptr;
  std::function<void()> f = t_base_event;
  QEvent *e = t_base_qevent;
  t_base_event = nullptr;          // base runs at most once per dispatch
  f();
  return e;
}

// ---- RubyItemDelegate ------------------------------------------------------
QWidget *RubyItemDelegate::createEditor(QWidget *parent,
                                        const QStyleOptionViewItem &opt,
                                        const QModelIndex &idx) const {
  if (!ruby_overrides(this, m_overrides, "createEditor"))
    return QStyledItemDelegate::createEditor(parent, opt, idx);
  bool handled = false;
  QWidget *w = NULL;
  ruby_with_gvl([&] {
    VALUE args[3] = { ruby_wrap_qobject(parent), Qnil, ruby_wrap_model_index(idx) };
    VALUE r = ruby_event_call(const_cast<RubyItemDelegate *>(this),
                              "createEditor", 3, args, &handled);
    if (handled) w = ruby_unwrap_widget(r);
  });
  if (!handled || !w) return QStyledItemDelegate::createEditor(parent, opt, idx);
  return w;
}

void RubyItemDelegate::setEditorData(QWidget *editor, const QModelIndex &idx) const {
  if (!ruby_overrides(this, m_overrides, "setEditorData")) {
    QStyledItemDelegate::setEditorData(editor, idx);
    return;
  }
  bool handled = false;
  ruby_with_gvl([&] {
    VALUE args[2] = { ruby_wrap_qobject(editor), ruby_wrap_model_index(idx) };
    handled = ruby_event_dispatch_n(const_cast<RubyItemDelegate *>(this),
                                    "setEditorData", 2, args);
  });
  if (!handled) QStyledItemDelegate::setEditorData(editor, idx);
}

void RubyItemDelegate::setModelData(QWidget *editor, QAbstractItemModel *model,
                                    const QModelIndex &idx) const {
  if (!ruby_overrides(this, m_overrides, "setModelData")) {
    QStyledItemDelegate::setModelData(editor, model, idx);
    return;
  }
  bool handled = false;
  ruby_with_gvl([&] {
    VALUE args[3] = { ruby_wrap_qobject(editor), ruby_wrap_qobject(model),
                      ruby_wrap_model_index(idx) };
    handled = ruby_event_dispatch_n(const_cast<RubyItemDelegate *>(this),
                                    "setModelData", 3, args);
  });
  if (!handled) QStyledItemDelegate::setModelData(editor, model, idx);
}

void RubyItemDelegate::paint(QPainter *p, const QStyleOptionViewItem &opt,
                             const QModelIndex &idx) const {
  if (!ruby_overrides(this, m_overrides, "paint")) {
    QStyledItemDelegate::paint(p, opt, idx);
    return;
  }
  bool handled = false;
  ruby_with_gvl([&] {
    VALUE args[3] = { ruby_wrap_painter_borrowed(p),
                      ruby_wrap_style_option_view_item(&opt),
                      ruby_wrap_model_index(idx) };
    handled = ruby_event_dispatch_n(const_cast<RubyItemDelegate *>(this), "paint", 3, args);
  });
  if (!handled) QStyledItemDelegate::paint(p, opt, idx);
}

// ---- RubyGLWidget ----------------------------------------------------------
void RubyGLWidget::initializeGL() {
  if (!ruby_overrides(this, m_overrides, "initializeGL")) { QOpenGLWidget::initializeGL(); return; }
  bool handled = false;
  ruby_with_gvl([&] { handled = ruby_event_dispatch_n(this, "initializeGL", 0, NULL); });
  if (!handled) QOpenGLWidget::initializeGL();
}
void RubyGLWidget::resizeGL(int w, int h) {
  if (!ruby_overrides(this, m_overrides, "resizeGL")) { QOpenGLWidget::resizeGL(w, h); return; }
  bool handled = false;
  ruby_with_gvl([&] {
    VALUE args[2] = { INT2NUM(w), INT2NUM(h) };
    handled = ruby_event_dispatch_n(this, "resizeGL", 2, args);
  });
  if (!handled) QOpenGLWidget::resizeGL(w, h);
}
void RubyGLWidget::paintGL() {
  if (!ruby_overrides(this, m_overrides, "paintGL")) { QOpenGLWidget::paintGL(); return; }
  bool handled = false;
  ruby_with_gvl([&] { handled = ruby_event_dispatch_n(this, "paintGL", 0, NULL); });
  if (!handled) QOpenGLWidget::paintGL();
}

// ---- shared dispatch -------------------------------------------------------
// Every VALUE is built inside ruby_with_gvl: building arguments first and
// acquiring the GVL afterwards corrupts the Ruby heap, and the [BUG] then
// lands on whatever thread allocates next, far from the real cause.
// `handled` stays false when the Ruby object has no override, so Qt's default
// runs. When true, whatever the handler did with accept/ignore is written
// back onto the real QEvent -- that is what lets event.ignore() in a
// closeEvent actually cancel the close.
// Each first asks ruby_overrides, so an event nothing overrides never takes
// the GVL.
static bool dispatch_ev(QObject *self, RubyOverrides &ov, const char *method, int kind, QEvent *e) {
  if (!ruby_overrides(self, ov, method)) return false;
  bool handled = false, accepted = true;
  const int type = e ? (int)e->type() : 0;
  ruby_with_gvl([&] {
    VALUE ev = ruby_make_plain_event(kind, type);
    handled  = ruby_event_dispatch(self, method, ev);
    accepted = ruby_event_accepted(ev);
  });
  if (handled && e) e->setAccepted(accepted);
  return handled;
}

static bool dispatch_paint(QObject *self, RubyOverrides &ov, QPaintEvent *e) {
  if (!ruby_overrides(self, ov, "paintEvent")) return false;
  bool handled = false;
  const QRect r = e->rect();
  const int type = (int)e->type();
  ruby_with_gvl([&] {
    handled = ruby_event_dispatch(
      self, "paintEvent",
      ruby_make_paint_event(r.x(), r.y(), r.width(), r.height(), type));
  });
  return handled;
}

static bool dispatch_wheel(QObject *self, RubyOverrides &ov, QWheelEvent *e) {
  if (!ruby_overrides(self, ov, "wheelEvent")) return false;
  bool handled = false, accepted = true;
  const int dx = e->angleDelta().x(), dy = e->angleDelta().y();
  const int mods = (int)e->modifiers(), type = (int)e->type();
  ruby_with_gvl([&] {
    VALUE ev = ruby_make_wheel_event(dx, dy, mods, type);
    handled  = ruby_event_dispatch(self, "wheelEvent", ev);
    accepted = ruby_event_accepted(ev);
  });
  if (handled) e->setAccepted(accepted);
  return handled;
}

static bool dispatch_key(QObject *self, RubyOverrides &ov, QKeyEvent *e) {
  if (!ruby_overrides(self, ov, "keyPressEvent")) return false;
  bool handled = false, accepted = true;
  const int key = e->key(), mods = (int)e->modifiers(), type = (int)e->type();
  const QByteArray txt = e->text().toUtf8();
  ruby_with_gvl([&] {
    VALUE ev = ruby_make_key_event(key, txt.constData(), mods, type);
    handled  = ruby_event_dispatch(self, "keyPressEvent", ev);
    accepted = ruby_event_accepted(ev);
  });
  if (handled) e->setAccepted(accepted);
  return handled;
}

static bool dispatch_mouse_ev(QObject *self, RubyOverrides &ov, const char *method, QMouseEvent *e) {
  if (!ruby_overrides(self, ov, method)) return false;
  bool handled = false, accepted = true;
  const int x = (int)e->position().x(), y = (int)e->position().y();
  const int b = (int)e->button(), bs = (int)e->buttons();
  const int mods = (int)e->modifiers(), type = (int)e->type();
  ruby_with_gvl([&] {
    VALUE ev = ruby_make_mouse_event(x, y, b, bs, mods, type);
    handled  = ruby_event_dispatch(self, method, ev);
    accepted = ruby_event_accepted(ev);
  });
  if (handled) e->setAccepted(accepted);
  return handled;
}

static bool dispatch_dnd(QObject *self, RubyOverrides &ov, const char *method, int kind, QDropEvent *e) {
  if (!ruby_overrides(self, ov, method)) return false;
  bool handled = false, accepted = true, proposed = false;
  const QMimeData *md = e->mimeData();
  const int type = (int)e->type();
  ruby_with_gvl([&] {
    VALUE ev = ruby_make_drop_event(kind, md, type);
    handled  = ruby_event_dispatch(self, method, ev);
    accepted = ruby_event_accepted(ev);
    proposed = ruby_event_proposed(ev);
  });
  if (!handled) return false;
  if (proposed) e->acceptProposedAction();
  else          e->setAccepted(accepted);
  return true;
}

#define FWD_PLAIN(Cls, Base, Name, Kind, EvT)                 \
  void Cls::Name(EvT *e) {                                    \
    BaseEventScope scope([this, e] { Base::Name(e); }, e);       \
    if (!dispatch_ev(this, m_overrides, #Name, Kind, e)) Base::Name(e);    \
  }
#define FWD_DND(Cls, Base)                                                     \
  void Cls::dragEnterEvent(QDragEnterEvent *e) {                               \
    BaseEventScope scope([this, e] { Base::dragEnterEvent(e); }, e);              \
    if (!dispatch_dnd(this, m_overrides, "dragEnterEvent", RUBY_EV_DRAGENTER, e))           \
      Base::dragEnterEvent(e);                                                 \
  }                                                                            \
  void Cls::dragMoveEvent(QDragMoveEvent *e) {                                 \
    BaseEventScope scope([this, e] { Base::dragMoveEvent(e); }, e);               \
    if (!dispatch_dnd(this, m_overrides, "dragMoveEvent", RUBY_EV_DRAGMOVE, e))             \
      Base::dragMoveEvent(e);                                                  \
  }                                                                            \
  void Cls::dropEvent(QDropEvent *e) {                                         \
    BaseEventScope scope([this, e] { Base::dropEvent(e); }, e);                   \
    if (!dispatch_dnd(this, m_overrides, "dropEvent", RUBY_EV_DROP, e)) Base::dropEvent(e); \
  }

// ---- RubyWidget ------------------------------------------------------------
FWD_PLAIN(RubyWidget, QWidget, resizeEvent,   RUBY_EV_RESIZE, QResizeEvent)
FWD_PLAIN(RubyWidget, QWidget, leaveEvent,    RUBY_EV_LEAVE,  QEvent)
FWD_PLAIN(RubyWidget, QWidget, focusInEvent,  RUBY_EV_FOCUS,  QFocusEvent)
FWD_PLAIN(RubyWidget, QWidget, focusOutEvent, RUBY_EV_FOCUS,  QFocusEvent)
FWD_PLAIN(RubyWidget, QWidget, closeEvent,    RUBY_EV_CLOSE,  QCloseEvent)
FWD_PLAIN(RubyWidget, QWidget, showEvent,     RUBY_EV_SHOW,   QShowEvent)
FWD_DND(RubyWidget, QWidget)

void RubyWidget::paintEvent(QPaintEvent *e) {
  BaseEventScope scope([this, e] { QWidget::paintEvent(e); }, e);
  if (!dispatch_paint(this, m_overrides, e)) QWidget::paintEvent(e);
}
void RubyWidget::wheelEvent(QWheelEvent *e) {
  BaseEventScope scope([this, e] { QWidget::wheelEvent(e); }, e);
  if (!dispatch_wheel(this, m_overrides, e)) QWidget::wheelEvent(e);
}
void RubyWidget::keyPressEvent(QKeyEvent *e) {
  BaseEventScope scope([this, e] { QWidget::keyPressEvent(e); }, e);
  if (!dispatch_key(this, m_overrides, e)) QWidget::keyPressEvent(e);
}
void RubyWidget::mousePressEvent(QMouseEvent *e) {
  BaseEventScope scope([this, e] { QWidget::mousePressEvent(e); }, e);
  if (!dispatch_mouse_ev(this, m_overrides, "mousePressEvent", e)) QWidget::mousePressEvent(e);
}
void RubyWidget::mouseMoveEvent(QMouseEvent *e) {
  BaseEventScope scope([this, e] { QWidget::mouseMoveEvent(e); }, e);
  if (!dispatch_mouse_ev(this, m_overrides, "mouseMoveEvent", e)) QWidget::mouseMoveEvent(e);
}
void RubyWidget::mouseReleaseEvent(QMouseEvent *e) {
  BaseEventScope scope([this, e] { QWidget::mouseReleaseEvent(e); }, e);
  if (!dispatch_mouse_ev(this, m_overrides, "mouseReleaseEvent", e)) QWidget::mouseReleaseEvent(e);
}

// ---- RubyMainWindow / RubyDialog -------------------------------------------
// 13 of COSMOS's 19 closeEvent overrides are on QtTool < Qt::MainWindow, so
// these carry most of the "are you sure?" prompts.
FWD_PLAIN(RubyMainWindow, QMainWindow, closeEvent,  RUBY_EV_CLOSE,  QCloseEvent)
FWD_PLAIN(RubyMainWindow, QMainWindow, showEvent,   RUBY_EV_SHOW,   QShowEvent)
FWD_PLAIN(RubyMainWindow, QMainWindow, resizeEvent, RUBY_EV_RESIZE, QResizeEvent)
FWD_DND(RubyMainWindow, QMainWindow)

void RubyMainWindow::keyPressEvent(QKeyEvent *e) {
  BaseEventScope scope([this, e] { QMainWindow::keyPressEvent(e); }, e);
  if (!dispatch_key(this, m_overrides, e)) QMainWindow::keyPressEvent(e);
}

FWD_PLAIN(RubyDialog, QDialog, closeEvent,  RUBY_EV_CLOSE,  QCloseEvent)
FWD_PLAIN(RubyDialog, QDialog, showEvent,   RUBY_EV_SHOW,   QShowEvent)
FWD_PLAIN(RubyDialog, QDialog, resizeEvent, RUBY_EV_RESIZE, QResizeEvent)

void RubyDialog::keyPressEvent(QKeyEvent *e) {
  BaseEventScope scope([this, e] { QDialog::keyPressEvent(e); }, e);
  if (!dispatch_key(this, m_overrides, e)) QDialog::keyPressEvent(e);
}

// ---- RubyDialog::reject ------------------------------------------------------
// The close button (QDialog::closeEvent) and Esc (QDialog::keyPressEvent)
// reach reject() here, never through Ruby, so a Ruby reject override --
// ScriptRunnerDialog refusing to close mid-script, the raw dialogs stopping
// their timers -- never ran. Forward it; the Ruby-visible reject (and so an
// override's super) calls QDialog::reject() directly, so this cannot recurse.
// Public methods only, as qtbindings' own override lookup was (Qt.cpp:319).
void RubyDialog::reject() {
  if (!ruby_overrides(this, m_overrides, "reject")) { QDialog::reject(); return; }
  bool handled = false;
  ruby_with_gvl([&] { handled = ruby_event_dispatch_n(this, "reject", 0, NULL); });
  if (!handled) QDialog::reject();
}

// ---- RubyGLWidget input ----------------------------------------------------
void RubyGLWidget::mousePressEvent(QMouseEvent *e) {
  BaseEventScope scope([this, e] { QOpenGLWidget::mousePressEvent(e); }, e);
  if (!dispatch_mouse_ev(this, m_overrides, "mousePressEvent", e)) QOpenGLWidget::mousePressEvent(e);
}
void RubyGLWidget::mouseReleaseEvent(QMouseEvent *e) {
  BaseEventScope scope([this, e] { QOpenGLWidget::mouseReleaseEvent(e); }, e);
  if (!dispatch_mouse_ev(this, m_overrides, "mouseReleaseEvent", e)) QOpenGLWidget::mouseReleaseEvent(e);
}
void RubyGLWidget::mouseMoveEvent(QMouseEvent *e) {
  BaseEventScope scope([this, e] { QOpenGLWidget::mouseMoveEvent(e); }, e);
  if (!dispatch_mouse_ev(this, m_overrides, "mouseMoveEvent", e)) QOpenGLWidget::mouseMoveEvent(e);
}
void RubyGLWidget::wheelEvent(QWheelEvent *e) {
  BaseEventScope scope([this, e] { QOpenGLWidget::wheelEvent(e); }, e);
  if (!dispatch_wheel(this, m_overrides, e)) QOpenGLWidget::wheelEvent(e);
}
