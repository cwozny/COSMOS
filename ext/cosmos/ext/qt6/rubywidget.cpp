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

BaseEventScopeT::BaseEventScopeT(std::function<void()> f) : saved(t_base_event) {
  t_base_event = f;
}
BaseEventScopeT::~BaseEventScopeT() { t_base_event = saved; }

typedef BaseEventScopeT BaseEventScope;   // name used by the hand-written virtuals

void ruby_call_base_event() {
  if (!t_base_event) return;
  std::function<void()> f = t_base_event;
  t_base_event = nullptr;          // base runs at most once per dispatch
  f();
}

// ---- RubyItemDelegate ------------------------------------------------------
QWidget *RubyItemDelegate::createEditor(QWidget *parent,
                                        const QStyleOptionViewItem &opt,
                                        const QModelIndex &idx) const {
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
  bool handled = false;
  ruby_with_gvl([&] { handled = ruby_event_dispatch_n(this, "initializeGL", 0, NULL); });
  if (!handled) QOpenGLWidget::initializeGL();
}
void RubyGLWidget::resizeGL(int w, int h) {
  bool handled = false;
  ruby_with_gvl([&] {
    VALUE args[2] = { INT2NUM(w), INT2NUM(h) };
    handled = ruby_event_dispatch_n(this, "resizeGL", 2, args);
  });
  if (!handled) QOpenGLWidget::resizeGL(w, h);
}
void RubyGLWidget::paintGL() {
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
static bool dispatch_ev(QObject *self, const char *method, int kind, QEvent *e) {
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

static bool dispatch_paint(QObject *self, QPaintEvent *e) {
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

static bool dispatch_wheel(QObject *self, QWheelEvent *e) {
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

static bool dispatch_key(QObject *self, QKeyEvent *e) {
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

static bool dispatch_mouse_ev(QObject *self, const char *method, QMouseEvent *e) {
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

static bool dispatch_dnd(QObject *self, const char *method, int kind, QDropEvent *e) {
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
    BaseEventScope scope([this, e] { Base::Name(e); });       \
    if (!dispatch_ev(this, #Name, Kind, e)) Base::Name(e);    \
  }
#define FWD_DND(Cls, Base)                                                     \
  void Cls::dragEnterEvent(QDragEnterEvent *e) {                               \
    BaseEventScope scope([this, e] { Base::dragEnterEvent(e); });              \
    if (!dispatch_dnd(this, "dragEnterEvent", RUBY_EV_DRAGENTER, e))           \
      Base::dragEnterEvent(e);                                                 \
  }                                                                            \
  void Cls::dragMoveEvent(QDragMoveEvent *e) {                                 \
    BaseEventScope scope([this, e] { Base::dragMoveEvent(e); });               \
    if (!dispatch_dnd(this, "dragMoveEvent", RUBY_EV_DRAGMOVE, e))             \
      Base::dragMoveEvent(e);                                                  \
  }                                                                            \
  void Cls::dropEvent(QDropEvent *e) {                                         \
    BaseEventScope scope([this, e] { Base::dropEvent(e); });                   \
    if (!dispatch_dnd(this, "dropEvent", RUBY_EV_DROP, e)) Base::dropEvent(e); \
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
  BaseEventScope scope([this, e] { QWidget::paintEvent(e); });
  if (!dispatch_paint(this, e)) QWidget::paintEvent(e);
}
void RubyWidget::wheelEvent(QWheelEvent *e) {
  BaseEventScope scope([this, e] { QWidget::wheelEvent(e); });
  if (!dispatch_wheel(this, e)) QWidget::wheelEvent(e);
}
void RubyWidget::keyPressEvent(QKeyEvent *e) {
  BaseEventScope scope([this, e] { QWidget::keyPressEvent(e); });
  if (!dispatch_key(this, e)) QWidget::keyPressEvent(e);
}
void RubyWidget::mousePressEvent(QMouseEvent *e) {
  BaseEventScope scope([this, e] { QWidget::mousePressEvent(e); });
  if (!dispatch_mouse_ev(this, "mousePressEvent", e)) QWidget::mousePressEvent(e);
}
void RubyWidget::mouseMoveEvent(QMouseEvent *e) {
  BaseEventScope scope([this, e] { QWidget::mouseMoveEvent(e); });
  if (!dispatch_mouse_ev(this, "mouseMoveEvent", e)) QWidget::mouseMoveEvent(e);
}
void RubyWidget::mouseReleaseEvent(QMouseEvent *e) {
  BaseEventScope scope([this, e] { QWidget::mouseReleaseEvent(e); });
  if (!dispatch_mouse_ev(this, "mouseReleaseEvent", e)) QWidget::mouseReleaseEvent(e);
}

// ---- RubyMainWindow / RubyDialog -------------------------------------------
// 13 of COSMOS's 19 closeEvent overrides are on QtTool < Qt::MainWindow, so
// these carry most of the "are you sure?" prompts.
FWD_PLAIN(RubyMainWindow, QMainWindow, closeEvent,  RUBY_EV_CLOSE,  QCloseEvent)
FWD_PLAIN(RubyMainWindow, QMainWindow, showEvent,   RUBY_EV_SHOW,   QShowEvent)
FWD_PLAIN(RubyMainWindow, QMainWindow, resizeEvent, RUBY_EV_RESIZE, QResizeEvent)
FWD_DND(RubyMainWindow, QMainWindow)

void RubyMainWindow::keyPressEvent(QKeyEvent *e) {
  BaseEventScope scope([this, e] { QMainWindow::keyPressEvent(e); });
  if (!dispatch_key(this, e)) QMainWindow::keyPressEvent(e);
}

FWD_PLAIN(RubyDialog, QDialog, closeEvent,  RUBY_EV_CLOSE,  QCloseEvent)
FWD_PLAIN(RubyDialog, QDialog, showEvent,   RUBY_EV_SHOW,   QShowEvent)
FWD_PLAIN(RubyDialog, QDialog, resizeEvent, RUBY_EV_RESIZE, QResizeEvent)

void RubyDialog::keyPressEvent(QKeyEvent *e) {
  BaseEventScope scope([this, e] { QDialog::keyPressEvent(e); });
  if (!dispatch_key(this, e)) QDialog::keyPressEvent(e);
}

// ---- RubyGLWidget input ----------------------------------------------------
void RubyGLWidget::mousePressEvent(QMouseEvent *e) {
  BaseEventScope scope([this, e] { QOpenGLWidget::mousePressEvent(e); });
  if (!dispatch_mouse_ev(this, "mousePressEvent", e)) QOpenGLWidget::mousePressEvent(e);
}
void RubyGLWidget::mouseReleaseEvent(QMouseEvent *e) {
  BaseEventScope scope([this, e] { QOpenGLWidget::mouseReleaseEvent(e); });
  if (!dispatch_mouse_ev(this, "mouseReleaseEvent", e)) QOpenGLWidget::mouseReleaseEvent(e);
}
void RubyGLWidget::mouseMoveEvent(QMouseEvent *e) {
  BaseEventScope scope([this, e] { QOpenGLWidget::mouseMoveEvent(e); });
  if (!dispatch_mouse_ev(this, "mouseMoveEvent", e)) QOpenGLWidget::mouseMoveEvent(e);
}
void RubyGLWidget::wheelEvent(QWheelEvent *e) {
  BaseEventScope scope([this, e] { QOpenGLWidget::wheelEvent(e); });
  if (!dispatch_wheel(this, e)) QOpenGLWidget::wheelEvent(e);
}
