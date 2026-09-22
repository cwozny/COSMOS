#ifndef RUBYWIDGET_H
#define RUBYWIDGET_H

#include <QWidget>
#include <QModelIndex>
#include <functional>
#include <cstring>
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
void ruby_call_base_event();

// RAII: installs the base-class thunk for the duration of one dispatch, so a
// Ruby override's `super` can run Qt's default. Restores the previous thunk,
// so nested dispatch is safe.
struct BaseEventScopeT {
  std::function<void()> saved;
  explicit BaseEventScopeT(std::function<void()> f);
  ~BaseEventScopeT();
};

// Invokes a Ruby-side override of a Qt virtual, if the wrapper defines one.
// Returns true when Ruby handled the event, false to fall through to Qt's
// default implementation. Defined in cosmos_qt6.cpp, where the object map and
// the GVL helpers live.
bool ruby_event_dispatch(QObject *obj, const char *method, VALUE arg);
bool ruby_event_dispatch_n(QObject *obj, const char *method, int argc, VALUE *argv);
VALUE ruby_event_call(QObject *obj, const char *method, int argc, VALUE *argv, bool *handled);
VALUE ruby_wrap_model_index(const QModelIndex &idx);
VALUE ruby_wrap_painter_borrowed(QPainter *p);
VALUE ruby_wrap_qobject(QObject *o);
QWidget *ruby_unwrap_widget(VALUE v);
VALUE ruby_make_key_event(int key);
VALUE ruby_make_mouse_event(int x, int y, int button);

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
};

class RubyWidget : public QWidget {
  Q_OBJECT
public:
  explicit RubyWidget(QWidget *parent = nullptr) : QWidget(parent) {}

protected:
  // Helpers so every callback body runs inside a single ruby_with_gvl.
  bool dispatch_plain(const char *method);
  bool dispatch_mouse(const char *method, QMouseEvent *e);

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
};

class RubyDialog : public QDialog {
  Q_OBJECT
public:
  explicit RubyDialog(QWidget *parent = nullptr) : QDialog(parent) {}

protected:
  void closeEvent(QCloseEvent *e) override;
  void showEvent(QShowEvent *e) override;
  void resizeEvent(QResizeEvent *e) override;
  void keyPressEvent(QKeyEvent *e) override;
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
  bool fwd_plain(const char *method) {
    bool handled = false;
    ruby_with_gvl([&] { handled = ruby_event_dispatch(this, method, Qnil); });
    return handled;
  }

  void paintEvent(QPaintEvent *e) override {
    BaseEventScopeT scope([this, e] { Base::paintEvent(e); });
    if (!fwd_plain("paintEvent")) Base::paintEvent(e);
  }
  void resizeEvent(QResizeEvent *e) override {
    BaseEventScopeT scope([this, e] { Base::resizeEvent(e); });
    if (!fwd_plain("resizeEvent")) Base::resizeEvent(e);
  }
  void closeEvent(QCloseEvent *e) override {
    BaseEventScopeT scope([this, e] { Base::closeEvent(e); });
    if (!fwd_plain("closeEvent")) Base::closeEvent(e);
  }
  void showEvent(QShowEvent *e) override {
    BaseEventScopeT scope([this, e] { Base::showEvent(e); });
    if (!fwd_plain("showEvent")) Base::showEvent(e);
  }
  void wheelEvent(QWheelEvent *e) override {
    BaseEventScopeT scope([this, e] { Base::wheelEvent(e); });
    if (!fwd_plain("wheelEvent")) Base::wheelEvent(e);
  }
  void leaveEvent(QEvent *e) override {
    BaseEventScopeT scope([this, e] { Base::leaveEvent(e); });
    if (!fwd_plain("leaveEvent")) Base::leaveEvent(e);
  }
  void focusInEvent(QFocusEvent *e) override {
    BaseEventScopeT scope([this, e] { Base::focusInEvent(e); });
    if (!fwd_plain("focusInEvent")) Base::focusInEvent(e);
  }
  void focusOutEvent(QFocusEvent *e) override {
    BaseEventScopeT scope([this, e] { Base::focusOutEvent(e); });
    if (!fwd_plain("focusOutEvent")) Base::focusOutEvent(e);
  }
  void keyPressEvent(QKeyEvent *e) override {
    BaseEventScopeT scope([this, e] { Base::keyPressEvent(e); });
    bool handled = false;
    const int key = e->key();
    ruby_with_gvl([&] {
      handled = ruby_event_dispatch(this, "keyPressEvent", ruby_make_key_event(key));
    });
    if (!handled) Base::keyPressEvent(e);
  }
  void mousePressEvent(QMouseEvent *e) override   { fwd_mouse("mousePressEvent", e); }
  void mouseMoveEvent(QMouseEvent *e) override    { fwd_mouse("mouseMoveEvent", e); }
  void mouseReleaseEvent(QMouseEvent *e) override { fwd_mouse("mouseReleaseEvent", e); }

private:
  void fwd_mouse(const char *method, QMouseEvent *e) {
    BaseEventScopeT scope([this, method, e] { base_mouse(method, e); });
    bool handled = false;
    const int x = (int)e->position().x();
    const int y = (int)e->position().y();
    const int b = (int)e->button();
    ruby_with_gvl([&] {
      handled = ruby_event_dispatch(this, method, ruby_make_mouse_event(x, y, b));
    });
    if (!handled) base_mouse(method, e);
  }
  void base_mouse(const char *method, QMouseEvent *e) {
    if (!strcmp(method, "mousePressEvent"))        Base::mousePressEvent(e);
    else if (!strcmp(method, "mouseMoveEvent"))    Base::mouseMoveEvent(e);
    else                                            Base::mouseReleaseEvent(e);
  }
};

#endif
