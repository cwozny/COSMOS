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
    VALUE args[3] = { ruby_wrap_painter_borrowed(p), Qnil, ruby_wrap_model_index(idx) };
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

// ---- RubyWidget ------------------------------------------------------------
// Dispatches a no-argument event; `handled` stays false when the Ruby object
// has no override (or the thread is unknown to Ruby), so Qt's default runs.
bool RubyWidget::dispatch_plain(const char *method) {
  bool handled = false;
  ruby_with_gvl([&] { handled = ruby_event_dispatch(this, method, Qnil); });
  return handled;
}

void RubyWidget::paintEvent(QPaintEvent *e) {
  BaseEventScope scope([this, e] { QWidget::paintEvent(e); });
  if (!dispatch_plain("paintEvent")) QWidget::paintEvent(e);
}
void RubyWidget::resizeEvent(QResizeEvent *e) {
  BaseEventScope scope([this, e] { QWidget::resizeEvent(e); });
  if (!dispatch_plain("resizeEvent")) QWidget::resizeEvent(e);
}
void RubyWidget::leaveEvent(QEvent *e) {
  BaseEventScope scope([this, e] { QWidget::leaveEvent(e); });
  if (!dispatch_plain("leaveEvent")) QWidget::leaveEvent(e);
}
void RubyWidget::focusInEvent(QFocusEvent *e) {
  BaseEventScope scope([this, e] { QWidget::focusInEvent(e); });
  if (!dispatch_plain("focusInEvent")) QWidget::focusInEvent(e);
}
void RubyWidget::focusOutEvent(QFocusEvent *e) {
  BaseEventScope scope([this, e] { QWidget::focusOutEvent(e); });
  if (!dispatch_plain("focusOutEvent")) QWidget::focusOutEvent(e);
}

bool RubyWidget::dispatch_mouse(const char *method, QMouseEvent *e) {
  bool handled = false;
  const int x = (int)e->position().x();
  const int y = (int)e->position().y();
  const int b = (int)e->button();
  ruby_with_gvl([&] {
    handled = ruby_event_dispatch(this, method, ruby_make_mouse_event(x, y, b));
  });
  return handled;
}

void RubyWidget::mousePressEvent(QMouseEvent *e) {
  BaseEventScope scope([this, e] { QWidget::mousePressEvent(e); });
  if (!dispatch_mouse("mousePressEvent", e)) QWidget::mousePressEvent(e);
}
void RubyWidget::mouseMoveEvent(QMouseEvent *e) {
  BaseEventScope scope([this, e] { QWidget::mouseMoveEvent(e); });
  if (!dispatch_mouse("mouseMoveEvent", e)) QWidget::mouseMoveEvent(e);
}
void RubyWidget::mouseReleaseEvent(QMouseEvent *e) {
  BaseEventScope scope([this, e] { QWidget::mouseReleaseEvent(e); });
  if (!dispatch_mouse("mouseReleaseEvent", e)) QWidget::mouseReleaseEvent(e);
}
void RubyWidget::keyPressEvent(QKeyEvent *e) {
  BaseEventScope scope([this, e] { QWidget::keyPressEvent(e); });
  bool handled = false;
  const int key = e->key();
  ruby_with_gvl([&] {
    handled = ruby_event_dispatch(this, "keyPressEvent", ruby_make_key_event(key));
  });
  if (!handled) QWidget::keyPressEvent(e);
}

void RubyWidget::closeEvent(QCloseEvent *e) {
  BaseEventScope scope([this, e] { QWidget::closeEvent(e); });
  if (!dispatch_plain("closeEvent")) QWidget::closeEvent(e);
}
void RubyWidget::wheelEvent(QWheelEvent *e) {
  BaseEventScope scope([this, e] { QWidget::wheelEvent(e); });
  if (!dispatch_plain("wheelEvent")) QWidget::wheelEvent(e);
}
void RubyWidget::showEvent(QShowEvent *e) {
  BaseEventScope scope([this, e] { QWidget::showEvent(e); });
  if (!dispatch_plain("showEvent")) QWidget::showEvent(e);
}

// ---- RubyMainWindow / RubyDialog -------------------------------------------
// Same contract as RubyWidget: build every VALUE inside ruby_with_gvl, and
// fall through to Qt when the Ruby object has no override.
static bool dispatch_for(QObject *self, const char *method) {
  bool handled = false;
  ruby_with_gvl([&] { handled = ruby_event_dispatch(self, method, Qnil); });
  return handled;
}

void RubyMainWindow::closeEvent(QCloseEvent *e) {
  BaseEventScope scope([this, e] { QMainWindow::closeEvent(e); });
  if (!dispatch_for(this, "closeEvent")) QMainWindow::closeEvent(e);
}
void RubyMainWindow::showEvent(QShowEvent *e) {
  BaseEventScope scope([this, e] { QMainWindow::showEvent(e); });
  if (!dispatch_for(this, "showEvent")) QMainWindow::showEvent(e);
}
void RubyMainWindow::resizeEvent(QResizeEvent *e) {
  BaseEventScope scope([this, e] { QMainWindow::resizeEvent(e); });
  if (!dispatch_for(this, "resizeEvent")) QMainWindow::resizeEvent(e);
}
void RubyMainWindow::keyPressEvent(QKeyEvent *e) {
  BaseEventScope scope([this, e] { QMainWindow::keyPressEvent(e); });
  bool handled = false;
  const int key = e->key();
  ruby_with_gvl([&] { handled = ruby_event_dispatch(this, "keyPressEvent", ruby_make_key_event(key)); });
  if (!handled) QMainWindow::keyPressEvent(e);
}

void RubyDialog::closeEvent(QCloseEvent *e) {
  BaseEventScope scope([this, e] { QDialog::closeEvent(e); });
  if (!dispatch_for(this, "closeEvent")) QDialog::closeEvent(e);
}
void RubyDialog::showEvent(QShowEvent *e) {
  BaseEventScope scope([this, e] { QDialog::showEvent(e); });
  if (!dispatch_for(this, "showEvent")) QDialog::showEvent(e);
}
void RubyDialog::resizeEvent(QResizeEvent *e) {
  BaseEventScope scope([this, e] { QDialog::resizeEvent(e); });
  if (!dispatch_for(this, "resizeEvent")) QDialog::resizeEvent(e);
}
void RubyDialog::keyPressEvent(QKeyEvent *e) {
  BaseEventScope scope([this, e] { QDialog::keyPressEvent(e); });
  bool handled = false;
  const int key = e->key();
  ruby_with_gvl([&] { handled = ruby_event_dispatch(this, "keyPressEvent", ruby_make_key_event(key)); });
  if (!handled) QDialog::keyPressEvent(e);
}
