#include "rubycallback.h"
#include "rubywidget.h"   // ruby_wrap_model_index
#include <ruby/thread.h>
#include <QCoreApplication>
#include <QThread>

// Whether THIS thread currently holds the GVL. Ruby already knows, so ask it.
// Tracking it locally did not work: "a Qt event loop released the GVL" was a
// process-global counter while "I already re-acquired it" was thread_local,
// so a background Ruby thread that natively held the GVL took the re-acquire
// path and aborted the process with
//   rb_thread_call_with_gvl: called by a thread which has GVL.
// Exported by libruby but not declared in a public header.
extern "C" int ruby_thread_has_gvl_p(void);

// Runs fn with the GVL held, acquiring it only if this thread doesn't have it.
void ruby_run_with_gvl(void *(*fn)(void *), void *arg) {
  // Qt can invoke us on threads Ruby has never seen (file-system watchers,
  // internal worker threads). Calling into Ruby from one of those -- with or
  // without rb_thread_call_with_gvl -- fails as
  // "pthread_mutex_lock: Invalid argument (EINVAL)". Skip instead.
  if (!ruby_native_thread_p()) return;

  if (ruby_thread_has_gvl_p()) fn(arg);
  else                          rb_thread_call_with_gvl(fn, arg);
}

// A Ruby exception raised inside a slot must never longjmp out through Qt's
// C++ stack frames -- that skips destructors and corrupts the stack, which
// surfaces later as a segfault on an unrelated thread. Contain it here and
// report it, the same discipline ev_invoke_with_gvl already follows.
static const char *g_error_context = "Qt slot";
static VALUE report_slot_error(VALUE err) {
  VALUE bt = rb_funcall(err, rb_intern("backtrace"), 0);
  VALUE msg = rb_sprintf("Qt %s raised %" PRIsVALUE ": %" PRIsVALUE "\n",
                         g_error_context,
                         rb_class_name(rb_obj_class(err)),
                         rb_funcall(err, rb_intern("message"), 0));
  rb_io_write(rb_stderr, msg);
  if (!NIL_P(bt)) {
    long n = RARRAY_LEN(bt) < 8 ? RARRAY_LEN(bt) : 8;
    for (long i = 0; i < n; i++)
      rb_io_write(rb_stderr, rb_sprintf("    from %" PRIsVALUE "\n", RARRAY_AREF(bt, i)));
  }
  // COSMOS reassigns $stderr into its own GUI output pane (qt_tool.rb
  // redirect_io), so contained errors never reach a log file and are easy to
  // miss. COSMOS_QT6_ERRLOG=<path> also appends them somewhere greppable.
  const char *errlog = getenv("COSMOS_QT6_ERRLOG");
  if (errlog) {
    FILE *f = fopen(errlog, "a");
    if (f) {
      fputs(StringValueCStr(msg), f);
      if (!NIL_P(bt)) {
        long n = RARRAY_LEN(bt) < 8 ? RARRAY_LEN(bt) : 8;
        for (long i = 0; i < n; i++) {
          VALUE line = rb_sprintf("    from %" PRIsVALUE "\n", RARRAY_AREF(bt, i));
          fputs(StringValueCStr(line), f);
        }
      }
      fclose(f);
    }
  }
  return Qnil;
}

// Reports a contained Ruby exception on stderr and clears it. Returns true
// when an exception was actually contained, so callers can fall back to Qt's
// default behaviour instead of pretending Ruby handled the event.
// Silently discarding here is what made broken Ruby virtuals render as blank
// widgets with no diagnostic at all.
static bool contain_error(int state);   // defined below

bool ruby_contain_error(int state, const char *context) {
  if (!state) return false;
  g_error_context = context ? context : "slot";
  bool contained = contain_error(state);
  g_error_context = "slot";
  return contained;
}

static bool contain_error(int state) {
  if (!state) return false;
  if (ruby_defer_exit_exception(state)) return true;   // ends the app instead
  VALUE err = rb_errinfo();
  rb_set_errinfo(Qnil);
  if (NIL_P(err)) return true;   // contained, just nothing printable
  int inner = 0;
  rb_protect(report_slot_error, err, &inner);
  if (inner) rb_set_errinfo(Qnil);   // reporting itself failed; drop it
  return true;
}

static VALUE do_call_proc(VALUE p) {
  return rb_funcall(p, rb_intern("call"), 0);
}
static void *call_proc(void *p) {
  int state = 0;
  rb_protect(do_call_proc, *(VALUE *)p, &state);
  contain_error(state);
  return NULL;
}

struct ProcCall { VALUE proc; int argc; VALUE argv[1]; };

static VALUE do_call_proc_args(VALUE p) {
  ProcCall *c = (ProcCall *)p;
  return rb_funcallv(c->proc, rb_intern("call"), c->argc, c->argv);
}
static void *call_proc_args(void *p) {
  int state = 0;
  rb_protect(do_call_proc_args, (VALUE)p, &state);
  contain_error(state);
  return NULL;
}

static void *run_std_fn(void *p) {
  (*(const std::function<void()> *)p)();
  return NULL;
}
static void *run_std_fn_v(void *p) { return run_std_fn(p); }
void ruby_with_gvl(const std::function<void()> &fn) {
  ruby_run_with_gvl(run_std_fn, (void *)&fn);
}

// ---- Ruby interrupts while a Qt event loop runs ----------------------------
// An exit-class exception (SystemExit, Interrupt, SignalException) raised
// where Ruby cannot unwind -- inside the event loop -- waits here for every
// loop to return; ruby_raise_pending_exit raises it in Ruby. The first wins.
static VALUE g_pending_exit = Qnil;

static bool is_exit_exception(VALUE err) {
  return rb_obj_is_kind_of(err, rb_eSystemExit) || rb_obj_is_kind_of(err, rb_eSignal);
}
// GVL held. QCoreApplication::exit ends every event loop on this thread, the
// modal ones nested in app.exec included, so each exec returns in turn.
static void defer_exit(VALUE err) {
  static bool registered = false;
  if (!registered) { rb_gc_register_address(&g_pending_exit); registered = true; }
  if (NIL_P(g_pending_exit)) g_pending_exit = err;
  if (QCoreApplication::instance()) QCoreApplication::exit(0);
}
void ruby_raise_pending_exit(void) {
  VALUE err = g_pending_exit;
  if (NIL_P(err)) return;
  g_pending_exit = Qnil;
  rb_exc_raise(err);
}

bool ruby_defer_exit_exception(int state) {
  if (!state) return false;
  VALUE err = rb_errinfo();
  if (NIL_P(err) || !is_exit_exception(err)) return false;
  rb_set_errinfo(Qnil);
  defer_exit(err);
  return true;
}

static VALUE check_ints(VALUE) { rb_thread_check_ints(); return Qnil; }
// Runs pending trap handlers, or raises the pending signal's default
// exception, on the GUI thread with the GVL held.
static void *service_interrupts(void *) {
  int state = 0;
  rb_protect(check_ints, Qnil, &state);
  ruby_contain_error(state, "signal handler");   // defers exit-class exceptions
  return NULL;
}
// The unblocking function Ruby calls, from another thread, to interrupt the
// thread running a Qt event loop. RUBY_UBF_IO only interrupted syscalls with
// EINTR, which Qt's dispatcher retries, so SIGINT/SIGTERM to an idle tool or
// an open dialog were never serviced. Post the servicing into the loop.
static void wake_event_loop(void *) {
  if (QCoreApplication *app = QCoreApplication::instance())
    QMetaObject::invokeMethod(app, [] { ruby_run_with_gvl(service_interrupts, NULL); },
                              Qt::QueuedConnection);
}

// Runs a blocking Qt call with the GVL released so Ruby's other threads keep
// getting scheduled. Every nested modal loop must go through this: a
// QMessageBox that holds the GVL gives CmdTlmServer's interface, telemetry
// and logging threads zero slices for as long as the dialog is on screen.
void ruby_without_gvl(const std::function<void()> &fn) {
  if (!ruby_native_thread_p() || !ruby_thread_has_gvl_p()) { fn(); return; }
  QCoreApplication *app = QCoreApplication::instance();
  bool gui = app && QThread::currentThread() == app->thread();
  rb_thread_call_without_gvl(run_std_fn_v, (void *)&fn,
                             gui ? wake_event_loop : RUBY_UBF_IO, NULL);
}

void ruby_invoke_proc(VALUE proc) {
  if (proc == Qnil) return;
  ruby_run_with_gvl(call_proc, &proc);
}

// ---- RubyGenericSlot -------------------------------------------------------
// Qt hands qt_metacall an argument array: a[0] is the return slot (unused for
// signals), a[1..n] are the parameters. Every VALUE must be built with the GVL
// held, so the whole conversion happens inside ruby_run_with_gvl.
namespace {
struct GenericCall {
  VALUE proc;
  int argc;
  VALUE argv[10];
};
VALUE do_generic_call(VALUE p) {
  GenericCall *c = (GenericCall *)p;
  return rb_funcallv(c->proc, rb_intern("call"), c->argc, c->argv);
}
struct RawSignalArgs {
  VALUE proc;
  const QList<int> *types;
  void **a;
};
void *generic_with_gvl(void *p) {
  RawSignalArgs *r = (RawSignalArgs *)p;
  GenericCall c;
  c.proc = r->proc;
  c.argc = 0;
  const int n = r->types->size() < 10 ? r->types->size() : 10;
  for (int i = 0; i < n; i++)
    c.argv[c.argc++] = ruby_value_from_meta(r->types->at(i), r->a[i + 1]);
  int state = 0;
  rb_protect(do_generic_call, (VALUE)&c, &state);
  ruby_contain_error(state, "slot");
  return NULL;
}
}  // namespace

int RubyGenericSlot::qt_metacall(QMetaObject::Call c, int id, void **a) {
  id = QObject::qt_metacall(c, id, a);
  if (id < 0 || c != QMetaObject::InvokeMetaMethod) return id;
  if (id == 0) {
    if (m_proc != Qnil) {
      RawSignalArgs r;
      r.proc = m_proc;
      r.types = &m_types;
      r.a = a;
      ruby_run_with_gvl(generic_with_gvl, &r);
    }
    return -1;
  }
  return id - 1;
}

void RubyCallback::dispatch(int argc, VALUE *argv) {
  if (m_proc == Qnil) return;
  ProcCall c;
  c.proc = m_proc;
  c.argc = argc;
  if (argc > 0) c.argv[0] = argv[0];
  ruby_run_with_gvl(call_proc_args, &c);
}

// The signal argument is carried across as plain C++ data and turned into a
// VALUE inside the GVL. Building it in invoke_str/invoke_idx first -- as this
// used to -- allocates a Ruby object from Qt's event loop with the GVL
// released, which corrupts the Ruby heap; the resulting [BUG] lands later on
// whichever thread allocates next, not here.
struct ArgCall {
  VALUE proc;
  enum Kind { Int, Bool, Str, Idx } kind;
  int ival;
  bool bval;
  QString sval;
  QModelIndex mval;
};

static VALUE do_arg_call(VALUE p) {
  ArgCall *c = (ArgCall *)p;
  VALUE a = Qnil;
  switch (c->kind) {
    case ArgCall::Int:  a = INT2NUM(c->ival); break;
    case ArgCall::Bool: a = c->bval ? Qtrue : Qfalse; break;
    case ArgCall::Str:  a = rb_str_new2(c->sval.toUtf8().constData()); break;
    case ArgCall::Idx:  a = ruby_wrap_model_index(c->mval); break;
  }
  return rb_funcallv(c->proc, rb_intern("call"), 1, &a);
}
static void *arg_call_with_gvl(void *p) {
  int state = 0;
  rb_protect(do_arg_call, (VALUE)p, &state);
  contain_error(state);
  return NULL;
}
static void dispatch_arg(ArgCall &c) {
  if (c.proc == Qnil) return;
  ruby_run_with_gvl(arg_call_with_gvl, &c);
}

void RubyCallback::invoke_int(int v) {
  ArgCall c; c.proc = m_proc; c.kind = ArgCall::Int;  c.ival = v; dispatch_arg(c);
}
void RubyCallback::invoke_bool(bool v) {
  ArgCall c; c.proc = m_proc; c.kind = ArgCall::Bool; c.bval = v; dispatch_arg(c);
}
void RubyCallback::invoke_str(const QString &v) {
  ArgCall c; c.proc = m_proc; c.kind = ArgCall::Str;  c.sval = v; dispatch_arg(c);
}
void RubyCallback::invoke_idx(const QModelIndex &v) {
  ArgCall c; c.proc = m_proc; c.kind = ArgCall::Idx;  c.mval = v; dispatch_arg(c);
}

void RubyCallback::invoke() {
  if (m_proc == Qnil) return;
  VALUE proc = m_proc;
  // Re-acquire the GVL if a Qt event loop released it and we don't already
  // hold it -- the crux of the Ruby/Qt threading problem.
  ruby_run_with_gvl(call_proc, &proc);
}
