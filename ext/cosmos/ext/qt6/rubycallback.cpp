#include "rubycallback.h"
#include "rubywidget.h"   // ruby_wrap_model_index
#include <ruby/thread.h>

int g_qt_gvl_released = 0;

// g_qt_gvl_released says "a Qt event loop on some thread released the GVL".
// It does NOT say whether THIS thread currently holds it. Without that
// distinction a nested callback (a Ruby slot that triggers another signal)
// calls rb_thread_call_with_gvl while already holding the GVL, which corrupts
// Ruby's thread state and crashes unrelated threads. Track it per-thread.
static thread_local bool t_reacquired_gvl = false;

// Runs fn with the GVL held, acquiring it only if this thread doesn't have it.
void ruby_run_with_gvl(void *(*fn)(void *), void *arg) {
  // Qt can invoke us on threads Ruby has never seen (file-system watchers,
  // internal worker threads). Calling into Ruby from one of those -- with or
  // without rb_thread_call_with_gvl -- fails as
  // "pthread_mutex_lock: Invalid argument (EINVAL)". Skip instead.
  if (!ruby_native_thread_p()) return;

  if (g_qt_gvl_released > 0 && !t_reacquired_gvl) {
    t_reacquired_gvl = true;
    rb_thread_call_with_gvl(fn, arg);
    t_reacquired_gvl = false;
  } else {
    fn(arg);
  }
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
GvlReleaseScope::GvlReleaseScope() {
  saved = t_reacquired_gvl;
  t_reacquired_gvl = false;
  g_qt_gvl_released++;
}
GvlReleaseScope::~GvlReleaseScope() {
  g_qt_gvl_released--;
  t_reacquired_gvl = saved;
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
void ruby_with_gvl(const std::function<void()> &fn) {
  ruby_run_with_gvl(run_std_fn, (void *)&fn);
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
