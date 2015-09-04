#ifndef RBX_OBJECTMEMORY_H
#define RBX_OBJECTMEMORY_H

#include "defines.hpp"
#include "type_info.hpp"
#include "object_position.hpp"
#include "oop.hpp"
#include "diagnostics.hpp"

#include "builtin/class.hpp"
#include "builtin/module.hpp"
#include "builtin/object.hpp"

#include "gc/code_manager.hpp"
#include "gc/finalize.hpp"
#include "gc/write_barrier.hpp"

#include "util/immix.hpp"
#include "util/thread.hpp"

#include "shared_state.hpp"

class TestObjectMemory; // So we can friend it properly
class TestVM; // So we can friend it properly

namespace rubinius {
  struct CallFrame;
  class BakerGC;
  class Configuration;
  class FinalizerThread;
  class GCData;
  class ImmixGC;
  class ImmixMarker;
  class InflatedHeaders;
  class Integer;
  class MarkSweepGC;
  class Thread;

  namespace diagnostics {
    class ObjectDiagnostics;
  }

  namespace gc {
    class Slab;
  }

  namespace capi {
    class Handle;
    class Handles;
    class GlobalHandle;
  }

  /**
   * ObjectMemory is the primary API that the rest of the VM uses to interact
   * with actions such as allocating objects, storing data in objects, and
   * performing garbage collection.
   *
   * It is currently split among 3 generations:
   *   - BakerGC:     handles young objects
   *   - ImmixGC:     handles mature objects
   *   - MarkSweepGC: handles large objects
   *
   * ObjectMemory also manages the memory used for CodeResources, which are
   * internal objects used for executing Ruby code. This includes MachineCode,
   * various JIT classes, and FFI data.
   *
   * Basic tasks:
   * - Allocate an object of a given class and number of fields. If the object
   *   is large, it's allocated in the large object space, otherwise in the
   *   young space.
   * - Detection of memory condition requiring collection of the young and
   *   mautre generations independently.
   */

  class ObjectMemory : public gc::WriteBarrier {
  private:
    utilities::thread::SpinLock allocation_lock_;
    utilities::thread::SpinLock inflation_lock_;

    /// BakerGC used for the young generation
    BakerGC* young_;

    /// MarkSweepGC used for the large object store
    MarkSweepGC* mark_sweep_;

    /// ImmixGC used for the mature generation
    ImmixGC* immix_;

    /// ImmixMarker thread used for the mature generation
    ImmixMarker* immix_marker_;

    /// Storage for all InflatedHeader instances.
    InflatedHeaders* inflated_headers_;

    /// Storage for C-API handle allocator, cached C-API handles
    /// and global handle locations.
    capi::Handles* capi_handles_;
    std::list<capi::Handle*> cached_capi_handles_;
    std::list<capi::GlobalHandle*> global_capi_handle_locations_;

    /// Garbage collector for CodeResource objects.
    CodeManager code_manager_;

    /// The current mark value used when marking objects.
    unsigned int mark_;

    /// Flag controlling whether garbage collections are allowed
    bool allow_gc_;
    /// Flag set when concurrent mature mark is requested
    bool mature_mark_concurrent_;
    /// Flag set when a mature GC is already in progress
    bool mature_gc_in_progress_;
    /// Flag set when requesting a young gen resize

    /// Size of slabs to be allocated to threads for lockless thread-local
    /// allocations.
    size_t slab_size_;

    /// Mutex used to manage lock contention
    utilities::thread::Mutex contention_lock_;

    /// Condition variable used to manage lock contention
    utilities::thread::Condition contention_var_;

    SharedState& shared_;

    diagnostics::ObjectDiagnostics* diagnostics_;

  public:
    /// Flag indicating whether a young collection should be performed soon
    bool collect_young_now;

    /// Flag indicating whether a full collection should be performed soon
    bool collect_mature_now;

    VM* vm_;
    /// Counter used for issuing object ids when #object_id is called on a
    /// Ruby object.
    size_t last_object_id;
    size_t last_snapshot_id;
    TypeInfo* type_info[(int)LastObjectType];

    /* Config variables */
    size_t large_object_threshold;

  public:
    static void memory_error(STATE);

    void set_vm(VM* vm) {
      vm_ = vm;
    }

    VM* vm() {
      return vm_;
    }

    ObjectMemory* memory() {
      return this;
    }

    unsigned int mark() const {
      return mark_;
    }

    const unsigned int* mark_address() const {
      return &mark_;
    }

    void rotate_mark() {
      mark_ = (mark_ == 2 ? 4 : 2);
    }

    bool can_gc() const {
      return allow_gc_;
    }

    void allow_gc() {
      allow_gc_ = true;
    }

    void inhibit_gc() {
      allow_gc_ = false;
    }

    FinalizerThread* finalizer_handler() const {
      return shared_.finalizer_handler();
    }

    InflatedHeaders* inflated_headers() const {
      return inflated_headers_;
    }

    capi::Handles* capi_handles() const {
      return capi_handles_;
    }

    ImmixMarker* immix_marker() const {
      return immix_marker_;
    }

    void set_immix_marker(ImmixMarker* immix_marker) {
      immix_marker_ = immix_marker;
    }

    capi::Handle* add_capi_handle(STATE, Object* obj);
    void make_capi_handle_cached(State*, capi::Handle* handle);

    std::list<capi::Handle*>* cached_capi_handles() {
      return &cached_capi_handles_;
    }

    std::list<capi::GlobalHandle*>* global_capi_handle_locations() {
      return &global_capi_handle_locations_;
    }

    void add_global_capi_handle_location(STATE, capi::Handle** loc, const char* file, int line);
    void del_global_capi_handle_location(STATE, capi::Handle** loc);

    ObjectArray* weak_refs_set();

  public:
    ObjectMemory(VM* state, SharedState& shared);
    ~ObjectMemory();

    void after_fork_child(STATE);

    inline void write_barrier(ObjectHeader* target, Fixnum* val) {
      /* No-op */
    }

    inline void write_barrier(ObjectHeader* target, Symbol* val) {
      /* No-op */
    }

    inline void write_barrier(ObjectHeader* target, ObjectHeader* val) {
      gc::WriteBarrier::write_barrier(target, val, mark_);
    }

    inline void write_barrier(ObjectHeader* target, Class* val) {
      gc::WriteBarrier::write_barrier(target, reinterpret_cast<Object*>(val), mark_);
    }

    // Object must be created in Immix or large object space.
    Object* new_object(STATE, native_int bytes);

    /* Allocate a new object in any space that will accommodate it based on
     * the following priority:
     *  1. SLAB (state-local allocation buffer, no locking needed)
     *  2. immix space (mature generation, lock needed)
     *  3. LOS (large object space, lock needed)
     *
     * The resulting object is UNINITIALIZED. The caller is responsible for
     * initializing all reference fields other than klass_ and ivars_.
     */
    Object* new_object(STATE, Class* klass, native_int bytes, object_type type) {
    allocate:
      Object* obj = state->vm()->local_slab().allocate(bytes).as<Object>();

      if(likely(obj)) {
        state->vm()->metrics().memory.young_objects++;
        state->vm()->metrics().memory.young_bytes += bytes;

        obj->init_header(YoungObjectZone, type);

        goto set_klass;
      }

      if(state->vm()->local_slab().empty_p()) {
        if(refill_slab(state, state->vm()->local_slab())) {
          goto allocate;
        } else {
          // TODO: set young collection
          state->vm()->metrics().gc.young_set++;
          collect_young_now = true;
          state->shared().gc_soon();
        }
      }

      if(likely(obj = new_object(state, bytes))) goto set_type;

      ObjectMemory::memory_error(state);
      return NULL;

    set_type:
      obj->set_obj_type(type);

    set_klass:
      obj->klass_ = klass;
      obj->ivars_ = cNil;

      if(obj->mature_object_p()) {
        write_barrier(obj, klass);
      }

#ifdef RBX_GC_STRESS
      state.shared().gc_soon();
#endif

      return obj;
    }

    /* Allocate a new, pinned, object in any space that will accommodate it
     * based on the following priority:
     *  1. immix space (mature generation, lock needed)
     *  2. LOS (large object space, lock needed)
     *
     * The resulting object is UNINITIALIZED. The caller is responsible for
     * initializing all reference fields other than klass_ and ivars_.
     */
    Object* new_object_pinned(STATE, Class* klass, native_int bytes, object_type type) {
      Object* obj = new_object(state, bytes);

      if(unlikely(!obj)) {
        ObjectMemory::memory_error(state);
        return NULL;
      }

      obj->set_pinned();
      obj->set_obj_type(type);

      obj->klass_ = klass;
      obj->ivars_ = cNil;

      write_barrier(obj, klass);

#ifdef RBX_GC_STRESS
      state.shared().gc_soon();
#endif

      return obj;
    }

    template <class T>
      T* new_object(STATE, Class* klass, native_int bytes, object_type type) {
        T* obj = new_object(state, klass, bytes, type);
        T::initialize(state, obj, bytes, type);

        return obj;
      }

    template <class T>
      T* new_object(STATE, Class *klass) {
        T* obj = static_cast<T*>(new_object(state, klass, sizeof(T), T::type));
        T::initialize(state, obj);

        return obj;
      }

    template <class T>
      T* new_object(STATE, Class *klass, native_int bytes) {
        return static_cast<T*>(new_object(state, klass, bytes, T::type));
      }

    template <class T>
      T* new_bytes(STATE, Class* klass, native_int bytes) {
        bytes = ObjectHeader::align(sizeof(T) + bytes);
        T* obj = static_cast<T*>(new_object(state, klass, bytes, T::type));

        obj->set_full_size(bytes);

        return obj;
      }

    template <class T>
      T* new_fields(STATE, Class* klass, native_int fields) {
        native_int bytes = sizeof(T) + (fields * sizeof(Object*));
        T* obj = static_cast<T*>(new_object(state, klass, bytes, T::type));

        obj->set_full_size(bytes);

        return obj;
      }

    template <class T>
      T* new_object_pinned(STATE, Class *klass) {
        T* obj = static_cast<T*>(new_object_pinned(state, klass, sizeof(T), T::type));
        T::initialize(state, obj);

        return obj;
      }

    template <class T>
      T* new_bytes_pinned(STATE, Class* klass, native_int bytes) {
        bytes = ObjectHeader::align(sizeof(T) + bytes);
        T* obj = static_cast<T*>(new_object_pinned(state, klass, bytes, T::type));

        obj->set_full_size(bytes);

        return obj;
      }

    template <class T>
      T* new_fields_pinned(STATE, Class* klass, native_int fields) {
        native_int bytes = sizeof(T) + (fields * sizeof(Object*));
        T* obj = static_cast<T*>(new_object_pinned(state, klass, bytes, T::type));

        obj->set_full_size(bytes);

        return obj;
      }

    // New classes.
    template <class T>
      Class* new_class(STATE, Class* super) {
        T* klass =
          static_cast<T*>(new_object(state, G(klass), sizeof(T), T::type));
        T::initialize(state, klass, super);

        return klass;
      }

    template <class T>
      Class* new_class(STATE, Module* under, const char* name) {
        return new_class<T>(state, G(object), under, name);
      }

    template <class T>
      T* new_class(STATE, Class* super, Module* under, const char* name) {
        return new_class<T>(state, super, under, state->symbol(name));
      }

    template <class T>
      T* new_class(STATE, Class* super, Module* under, Symbol* name) {
        T* klass =
          static_cast<T*>(new_object(state, G(klass), sizeof(T), T::type));
        T::initialize(state, klass, super, under, name);

        return klass;
      }

    template <class S, class R>
      Class* new_class(STATE, Module* under, const char* name) {
        return new_class<S, R>(state, G(object), under, name);
      }

    template <class S, class R>
      Class* new_class(STATE, Class* super, const char* name) {
        return new_class<S, R>(state, super, G(object), name);
      }

    template <class S, class R>
      Class* new_class(STATE, const char* name) {
        return new_class<S, R>(state, G(object), G(object), name);
      }

    template <class S, class R>
      S* new_class(STATE, Class* super, Module* under, const char* name) {
        return new_class<S, R>(state, super, under, state->symbol(name));
      }

    template <class S, class R>
      S* new_class(STATE, Class* super, Module* under, Symbol* name) {
        S* klass =
          static_cast<S*>(new_object(state, G(klass), sizeof(S), S::type));
        S::initialize(state, klass, super, under, name, R::type);

        return klass;
      }

    // New modules.
    template <class T>
      T* new_module(STATE) {
        return new_module<T>(state, G(module));
      }

    template <class T>
      T* new_module(STATE, Class* super) {
        return state->memory()->new_object<T>(state, super);
      }

    template <class T>
      T* new_module(STATE, Module* under, const char* name) {
        return new_module<T>(state, G(module), under, name);
      }

    template <class T>
      T* new_module(STATE, Class* super, Module* under, const char* name) {
        T *mod = static_cast<T*>(state->memory()->new_object(
              state, super, sizeof(T), T::type));
        T::initialize(state, mod, under, name);

        return mod;
      }

    template <class T>
      T* new_module(STATE, const char* name) {
        return new_module<T>(state, G(module), G(object), name);
      }

    TypeInfo* find_type_info(Object* obj);
    Object* promote_object(Object* obj);

    bool refill_slab(STATE, gc::Slab& slab);

    void assign_object_id(STATE, Object* obj);
    bool inflate_lock_count_overflow(STATE, ObjectHeader* obj, int count);
    LockStatus contend_for_lock(STATE, CallFrame* call_frame, ObjectHeader* obj, size_t us, bool interrupt);
    void release_contention(STATE, CallFrame* call_frame);
    bool inflate_and_lock(STATE, ObjectHeader* obj);
    bool inflate_for_contention(STATE, ObjectHeader* obj);

    bool valid_object_p(Object* obj);
    void add_type_info(TypeInfo* ti);

    void add_code_resource(STATE, CodeResource* cr);
    void memstats();

    void validate_handles(capi::Handles* handles);
    void prune_handles(capi::Handles* handles, std::list<capi::Handle*>* cached, BakerGC* young);
    void clear_fiber_marks(ThreadList* threads);

    ObjectPosition validate_object(Object* obj);

    void collect_maybe(STATE);

    void needs_finalization(Object* obj, FinalizerFunction func,
        FinalizeObject::FinalizeKind kind = FinalizeObject::eManaged);
    void set_ruby_finalizer(Object* obj, Object* finalizer);

    InflatedHeader* inflate_header(STATE, ObjectHeader* obj);
    void inflate_for_id(STATE, ObjectHeader* obj, uint32_t id);
    void inflate_for_handle(STATE, ObjectHeader* obj, capi::Handle* handle);

    /// This only has one use! Don't use it!
    Object* allocate_object_raw(size_t bytes);
    void collect_mature_finish(STATE, GCData* data);

    // TODO: generalize when fixing safe points.
    String* new_string_certain(STATE, Class* klass);

    bool mature_gc_in_progress() {
      return mature_gc_in_progress_;
    }

    void clear_mature_mark_in_progress() {
      mature_gc_in_progress_ = false;
    }

    diagnostics::ObjectDiagnostics* diagnostics() {
      return diagnostics_;
    }

    immix::MarkStack& mature_mark_stack();

  private:
    Object* allocate_object(size_t bytes);
    Object* allocate_object_mature(size_t bytes);

    void collect_young(STATE, GCData* data);
    void collect_mature(STATE, GCData* data);

  public:
    friend class ::TestObjectMemory;
    friend class ::TestVM;


    /**
     * Object used to prevent garbage collections from running for a short
     * period while the memory is scanned, e.g. to find referrers to an
     * object or take a snapshot of the heap. Typically, an instance of
     * this class is created at the start of a method that requires the
     * heap to be stable. When the method ends, the object goes out of
     * scope and is destroyed, re-enabling garbage collections.
     */

    class GCInhibit {
      ObjectMemory* om_;

    public:
      GCInhibit(ObjectMemory* om)
        : om_(om)
      {
        om->inhibit_gc();
      }

      GCInhibit(STATE)
        : om_(state->memory())
      {
        om_->inhibit_gc();
      }

      ~GCInhibit() {
        om_->allow_gc();
      }
    };
  };
};

#endif
