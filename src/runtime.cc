// Copyright 2011 Google Inc. All Rights Reserved.

#include "runtime.h"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

#include "UniquePtr.h"
#include "class_linker.h"
#include "heap.h"
#include "intern_table.h"
#include "jni_internal.h"
#include "signal_catcher.h"
#include "thread.h"

// TODO: this drags in cutil/log.h, which conflicts with our logging.h.
#include "JniConstants.h"

namespace art {

Runtime* Runtime::instance_ = NULL;

Runtime::Runtime()
    : stack_size_(0),
      thread_list_(NULL),
      intern_table_(NULL),
      class_linker_(NULL),
      signal_catcher_(NULL),
      java_vm_(NULL),
      started_(false),
      vfprintf_(NULL),
      exit_(NULL),
      abort_(NULL) {
}

Runtime::~Runtime() {
  // TODO: use smart pointers instead. (we'll need the pimpl idiom.)
  delete class_linker_;
  Heap::Destroy();
  delete signal_catcher_;
  delete thread_list_;
  delete intern_table_;
  delete java_vm_;
  Thread::Shutdown();
  // TODO: acquire a static mutex on Runtime to avoid racing.
  CHECK(instance_ == NULL || instance_ == this);
  instance_ = NULL;
}

void Runtime::Abort(const char* file, int line) {
  // Get any pending output out of the way.
  fflush(NULL);

  // Many people have difficulty distinguish aborts from crashes,
  // so be explicit.
  LogMessage(file, line, ERROR, -1).stream() << "Runtime aborting...";

  // Perform any platform-specific pre-abort actions.
  PlatformAbort(file, line);

  // use abort hook if we have one
  if (Runtime::Current() != NULL && Runtime::Current()->abort_ != NULL) {
    Runtime::Current()->abort_();
    // notreached
  }

  // If we call abort(3) on a device, all threads in the process
  // receive SIGABRT.  debuggerd dumps the stack trace of the main
  // thread, whether or not that was the thread that failed.  By
  // stuffing a value into a bogus address, we cause a segmentation
  // fault in the current thread, and get a useful log from debuggerd.
  // We can also trivially tell the difference between a VM crash and
  // a deliberate abort by looking at the fault address.
  *reinterpret_cast<char*>(0xdeadd00d) = 38;
  abort();
  // notreached
}

void Runtime::CallExitHook(jint status) {
  if (exit_ != NULL) {
    ScopedThreadStateChange tsc(Thread::Current(), Thread::kNative);
    exit_(status);
    LOG(WARNING) << "Exit hook returned instead of exiting!";
  }
}

// Parse a string of the form /[0-9]+[kKmMgG]?/, which is used to specify
// memory sizes.  [kK] indicates kilobytes, [mM] megabytes, and
// [gG] gigabytes.
//
// "s" should point just past the "-Xm?" part of the string.
// "div" specifies a divisor, e.g. 1024 if the value must be a multiple
// of 1024.
//
// The spec says the -Xmx and -Xms options must be multiples of 1024.  It
// doesn't say anything about -Xss.
//
// Returns 0 (a useless size) if "s" is malformed or specifies a low or
// non-evenly-divisible value.
//
size_t ParseMemoryOption(const char *s, size_t div) {
  // strtoul accepts a leading [+-], which we don't want,
  // so make sure our string starts with a decimal digit.
  if (isdigit(*s)) {
    const char *s2;
    size_t val = strtoul(s, (char **)&s2, 10);
    if (s2 != s) {
      // s2 should be pointing just after the number.
      // If this is the end of the string, the user
      // has specified a number of bytes.  Otherwise,
      // there should be exactly one more character
      // that specifies a multiplier.
      if (*s2 != '\0') {
        // The remainder of the string is either a single multiplier
        // character, or nothing to indicate that the value is in
        // bytes.
        char c = *s2++;
        if (*s2 == '\0') {
          size_t mul;
          if (c == '\0') {
            mul = 1;
          } else if (c == 'k' || c == 'K') {
            mul = KB;
          } else if (c == 'm' || c == 'M') {
            mul = MB;
          } else if (c == 'g' || c == 'G') {
            mul = GB;
          } else {
            // Unknown multiplier character.
            return 0;
          }

          if (val <= std::numeric_limits<size_t>::max() / mul) {
            val *= mul;
          } else {
            // Clamp to a multiple of 1024.
            val = std::numeric_limits<size_t>::max() & ~(1024-1);
          }
        } else {
          // There's more than one character after the numeric part.
          return 0;
        }
      }
      // The man page says that a -Xm value must be a multiple of 1024.
      if (val % div == 0) {
        return val;
      }
    }
  }
  return 0;
}

void LoadJniLibrary(JavaVMExt* vm, const char* name) {
  // TODO: OS_SHARED_LIB_FORMAT_STR
  std::string mapped_name(StringPrintf("lib%s.so", name));
  std::string reason;
  if (!vm->LoadNativeLibrary(mapped_name, NULL, reason)) {
    LOG(FATAL) << "LoadNativeLibrary failed for \"" << mapped_name << "\": "
               << reason;
  }
}

void CreateClassPath(const char* class_path_cstr,
                     std::vector<const DexFile*>& class_path_vector) {
  CHECK(class_path_cstr != NULL);
  std::vector<std::string> parsed;
  Split(class_path_cstr, ':', parsed);
  for (size_t i = 0; i < parsed.size(); ++i) {
    const DexFile* dex_file = DexFile::Open(parsed[i]);
    if (dex_file != NULL) {
      class_path_vector.push_back(dex_file);
    }
  }
}

Runtime::ParsedOptions* Runtime::ParsedOptions::Create(const Options& options, bool ignore_unrecognized) {
  UniquePtr<ParsedOptions> parsed(new ParsedOptions());
  const char* boot_class_path = NULL;
  const char* class_path = NULL;
  parsed->boot_image_ = NULL;
#ifdef NDEBUG
  // -Xcheck:jni is off by default for regular builds...
  parsed->check_jni_ = false;
#else
  // ...but on by default in debug builds.
#if 0 // TODO: disabled for oatexec until the shorty's used by check_jni are managed heap allocated.
      // Instead we turn on -Xcheck_jni in common_test.
  parsed->check_jni_ = true;
#else
  parsed->check_jni_ = false;
#endif
#endif
  parsed->heap_initial_size_ = Heap::kInitialSize;
  parsed->heap_maximum_size_ = Heap::kMaximumSize;
  parsed->stack_size_ = Thread::kDefaultStackSize;

  parsed->hook_vfprintf_ = vfprintf;
  parsed->hook_exit_ = exit;
  parsed->hook_abort_ = abort;

  for (size_t i = 0; i < options.size(); ++i) {
    const StringPiece& option = options[i].first;
    if (option.starts_with("-Xbootclasspath:")) {
      boot_class_path = option.substr(strlen("-Xbootclasspath:")).data();
    } else if (option == "bootclasspath") {
      const void* dex_vector = options[i].second;
      const std::vector<const DexFile*>* v
          = reinterpret_cast<const std::vector<const DexFile*>*>(dex_vector);
      if (v == NULL) {
        if (ignore_unrecognized) {
          continue;
        }
        // TODO: usage
        LOG(FATAL) << "Failed to parse " << option;
        return NULL;
      }
      parsed->boot_class_path_ = *v;
    } else if (option == "-classpath" || option == "-cp") {
      // TODO: support -Djava.class.path
      i++;
      if (i == options.size()) {
        // TODO: usage
        LOG(FATAL) << "Missing required class path value for " << option;
        return NULL;
      }
      const StringPiece& value = options[i].first;
      class_path = value.data();
    } else if (option.starts_with("-Xbootimage:")) {
      // TODO: remove when intern_addr_ is removed, just use -Ximage:
      parsed->boot_image_ = option.substr(strlen("-Xbootimage:")).data();
    } else if (option.starts_with("-Ximage:")) {
      parsed->images_.push_back(option.substr(strlen("-Ximage:")).data());
    } else if (option.starts_with("-Xcheck:jni")) {
      parsed->check_jni_ = true;
    } else if (option.starts_with("-Xms")) {
      size_t size = ParseMemoryOption(option.substr(strlen("-Xms")).data(), 1024);
      if (size == 0) {
        if (ignore_unrecognized) {
          continue;
        }
        // TODO: usage
        LOG(FATAL) << "Failed to parse " << option;
        return NULL;
      }
      parsed->heap_initial_size_ = size;
    } else if (option.starts_with("-Xmx")) {
      size_t size = ParseMemoryOption(option.substr(strlen("-Xmx")).data(), 1024);
      if (size == 0) {
        if (ignore_unrecognized) {
          continue;
        }
        // TODO: usage
        LOG(FATAL) << "Failed to parse " << option;
        return NULL;
      }
      parsed->heap_maximum_size_ = size;
    } else if (option.starts_with("-Xss")) {
      size_t size = ParseMemoryOption(option.substr(strlen("-Xss")).data(), 1);
      if (size == 0) {
        if (ignore_unrecognized) {
          continue;
        }
        // TODO: usage
        LOG(FATAL) << "Failed to parse " << option;
        return NULL;
      }
      parsed->stack_size_ = size;
    } else if (option.starts_with("-D")) {
      parsed->properties_.push_back(option.substr(strlen("-D")).data());
    } else if (option.starts_with("-Xjnitrace:")) {
      parsed->jni_trace_ = option.substr(strlen("-Xjnitrace:")).data();
    } else if (option.starts_with("-verbose:")) {
      std::vector<std::string> verbose_options;
      Split(option.substr(strlen("-verbose:")).data(), ',', verbose_options);
      for (size_t i = 0; i < verbose_options.size(); ++i) {
        parsed->verbose_.insert(verbose_options[i]);
      }
    } else if (option == "vfprintf") {
      parsed->hook_vfprintf_ = reinterpret_cast<int (*)(FILE *, const char*, va_list)>(options[i].second);
    } else if (option == "exit") {
      parsed->hook_exit_ = reinterpret_cast<void(*)(jint)>(options[i].second);
    } else if (option == "abort") {
      parsed->hook_abort_ = reinterpret_cast<void(*)()>(options[i].second);
    } else {
      if (!ignore_unrecognized) {
        // TODO: print usage via vfprintf
        LOG(FATAL) << "Unrecognized option " << option;
        return NULL;
      }
    }
  }

  // consider it an error if both bootclasspath and -Xbootclasspath: are supplied.
  // TODO: remove bootclasspath which is only mostly just used by tests?
  if (!parsed->boot_class_path_.empty() && boot_class_path != NULL) {
    // TODO: usage
    LOG(FATAL) << "bootclasspath and -Xbootclasspath: are mutually exclusive options.";
    return NULL;
  }
  if (parsed->boot_class_path_.empty()) {
    if (boot_class_path == NULL) {
      boot_class_path = getenv("BOOTCLASSPATH");
      if (boot_class_path == NULL) {
        boot_class_path = "";
      }
    }
    CreateClassPath(boot_class_path, parsed->boot_class_path_);
  }

  if (class_path == NULL) {
    class_path = getenv("CLASSPATH");
    if (class_path == NULL) {
      class_path = "";
    }
  }
  CHECK_EQ(parsed->class_path_.size(), 0U);
  CreateClassPath(class_path, parsed->class_path_);

  return parsed.release();
}

Runtime* Runtime::Create(const Options& options, bool ignore_unrecognized) {
  // TODO: acquire a static mutex on Runtime to avoid racing.
  if (Runtime::instance_ != NULL) {
    return NULL;
  }
  instance_ = new Runtime;
  if (!instance_->Init(options, ignore_unrecognized)) {
    delete instance_;
    instance_ = NULL;
  }
  return instance_;
}

void Runtime::Start() {
  started_ = true;
  instance_->InitLibraries();
  instance_->signal_catcher_ = new SignalCatcher;
}

bool Runtime::IsStarted() {
  return started_;
}

bool Runtime::Init(const Options& raw_options, bool ignore_unrecognized) {
  CHECK_EQ(sysconf(_SC_PAGE_SIZE), kPageSize);

  UniquePtr<ParsedOptions> options(ParsedOptions::Create(raw_options, ignore_unrecognized));
  if (options.get() == NULL) {
    LOG(WARNING) << "Failed to parse options";
    return false;
  }
  vfprintf_ = options->hook_vfprintf_;
  exit_ = options->hook_exit_;
  abort_ = options->hook_abort_;

  stack_size_ = options->stack_size_;
  thread_list_ = ThreadList::Create();

  intern_table_ = new InternTable;

  if (!Heap::Init(options->heap_initial_size_,
                  options->heap_maximum_size_,
                  options->boot_image_,
                  options->images_)) {
    LOG(WARNING) << "Failed to create heap";
    return false;
  }

  BlockSignals();

  java_vm_ = new JavaVMExt(this, options.get());

  if (!Thread::Startup()) {
    LOG(WARNING) << "Failed to startup threads";
    return false;
  }

  thread_list_->Register(Thread::Attach(this, "main", false));

  class_linker_ = ClassLinker::Create(options->boot_class_path_,
                                      options->class_path_,
                                      intern_table_,
                                      Heap::GetBootSpace());

  return true;
}

void Runtime::InitLibraries() {
  Thread* self = Thread::Current();
  JNIEnv* env = self->GetJniEnv();

  // Must be in the kNative state for JNI-based method registration.
  ScopedThreadStateChange tsc(self, Thread::kNative);

  // First set up the native methods provided by the runtime itself.
  RegisterRuntimeNativeMethods(env);

  // Now set up libcore, which is just a JNI library with a JNI_OnLoad.
  // Most JNI libraries can just use System.loadLibrary, but you can't
  // if you're the library that implements System.loadLibrary!
  JniConstants::init(env);
  LoadJniLibrary(instance_->GetJavaVM(), "javacore");
}

void Runtime::RegisterRuntimeNativeMethods(JNIEnv* env) {
#define REGISTER(FN) extern void FN(JNIEnv*); FN(env)
  //REGISTER(register_dalvik_bytecode_OpcodeInfo);
  //REGISTER(register_dalvik_system_DexFile);
  //REGISTER(register_dalvik_system_VMDebug);
  //REGISTER(register_dalvik_system_VMRuntime);
  //REGISTER(register_dalvik_system_VMStack);
  //REGISTER(register_dalvik_system_Zygote);
  //REGISTER(register_java_lang_Class);
  REGISTER(register_java_lang_Object);
  REGISTER(register_java_lang_Runtime);
  REGISTER(register_java_lang_String);
  REGISTER(register_java_lang_System);
  //REGISTER(register_java_lang_Thread);
  //REGISTER(register_java_lang_Throwable);
  //REGISTER(register_java_lang_VMClassLoader);
  //REGISTER(register_java_lang_reflect_AccessibleObject);
  //REGISTER(register_java_lang_reflect_Array);
  //REGISTER(register_java_lang_reflect_Constructor);
  //REGISTER(register_java_lang_reflect_Field);
  //REGISTER(register_java_lang_reflect_Method);
  //REGISTER(register_java_lang_reflect_Proxy);
  REGISTER(register_java_util_concurrent_atomic_AtomicLong);
  //REGISTER(register_org_apache_harmony_dalvik_ddmc_DdmServer);
  //REGISTER(register_org_apache_harmony_dalvik_ddmc_DdmVmInternal);
  //REGISTER(register_sun_misc_Unsafe);
#undef REGISTER
}

void Runtime::DumpStatistics(std::ostream& os) {
  // TODO: dump other runtime statistics?
  os << "Loaded classes: " << class_linker_->NumLoadedClasses() << "\n";
  os << "Intern table size: " << GetInternTable()->Size() << "\n";
  // LOGV("VM stats: meth=%d ifld=%d sfld=%d linear=%d",
  //    gDvm.numDeclaredMethods,
  //    gDvm.numDeclaredInstFields,
  //    gDvm.numDeclaredStaticFields,
  //    gDvm.pBootLoaderAlloc->curOffset);
  // LOGI("GC precise methods: %d", dvmPointerSetGetCount(gDvm.preciseMethods));
  os << "\n";
}

void Runtime::BlockSignals() {
  sigset_t sigset;
  if (sigemptyset(&sigset) == -1) {
    PLOG(FATAL) << "sigemptyset failed";
  }
  if (sigaddset(&sigset, SIGPIPE) == -1) {
    PLOG(ERROR) << "sigaddset SIGPIPE failed";
  }
  // SIGQUIT is used to dump the runtime's state (including stack traces).
  if (sigaddset(&sigset, SIGQUIT) == -1) {
    PLOG(ERROR) << "sigaddset SIGQUIT failed";
  }
  // SIGUSR1 is used to initiate a heap dump.
  if (sigaddset(&sigset, SIGUSR1) == -1) {
    PLOG(ERROR) << "sigaddset SIGUSR1 failed";
  }
  CHECK_EQ(sigprocmask(SIG_BLOCK, &sigset, NULL), 0);
}

void Runtime::AttachCurrentThread(const char* name, JNIEnv** penv, bool as_daemon) {
  Thread* t = Thread::Attach(instance_, name, as_daemon);
  thread_list_->Register(t);
}

void Runtime::DetachCurrentThread() {
  thread_list_->Unregister();
}

void Runtime::VisitRoots(Heap::RootVisitor* visitor, void* arg) const {
  class_linker_->VisitRoots(visitor, arg);
  intern_table_->VisitRoots(visitor, arg);
  java_vm_->VisitRoots(visitor, arg);
  thread_list_->VisitRoots(visitor, arg);

  //(*visitor)(&gDvm.outOfMemoryObj, 0, ROOT_VM_INTERNAL, arg);
  //(*visitor)(&gDvm.internalErrorObj, 0, ROOT_VM_INTERNAL, arg);
  //(*visitor)(&gDvm.noClassDefFoundErrorObj, 0, ROOT_VM_INTERNAL, arg);
  UNIMPLEMENTED(WARNING) << "some roots not marked";
}

}  // namespace art
