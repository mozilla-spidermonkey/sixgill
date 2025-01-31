#include <utility>
#include <type_traits>

#include <new>  // for placement new
#include <type_traits>
#include <utility>

#define ANNOTATE(tagname) __attribute__((annotate(tagname)))

struct Cell { std::size_t f; } ANNOTATE("GC Thing");

struct Nothing {};

template <class T>
class Maybe;

namespace detail {

template <typename T>
struct MaybeStorage {
  using NonConstT = std::remove_const_t<T>;

  union Union {
    Union() {}
    constexpr explicit Union(const T& aVal) : val{aVal} {}
    template <typename U,
              typename = std::enable_if_t<std::is_move_constructible<U>::value>>
    constexpr explicit Union(U&& aVal) : val{std::forward<U>(aVal)} {}

    ~Union() {}

    NonConstT val;
    char dummy;
  } mStorage;
  char mIsSome = false;  // not bool -- guarantees minimal space consumption

  MaybeStorage() = default;
  explicit MaybeStorage(const T& aVal) : mStorage{aVal}, mIsSome{true} {}
  explicit MaybeStorage(T&& aVal) : mStorage{std::move(aVal)}, mIsSome{true} {}

  template <typename... Args>
  explicit MaybeStorage(Args&&... aArgs) : mIsSome{true} {
    ::new (&mStorage.val) T(std::forward<Args>(aArgs)...);
  }

  // Copy and move operations are no-ops, since copying is moving is implemented
  // by Maybe_CopyMove_Enabler.

  MaybeStorage(const MaybeStorage&) {}
  MaybeStorage& operator=(const MaybeStorage&) { return *this; }
  MaybeStorage(MaybeStorage&&) {}
  MaybeStorage& operator=(MaybeStorage&&) { return *this; }

  ~MaybeStorage() {
    if (mIsSome) {
      mStorage.val.T::~T();
    }
  }
};

}  // namespace detail

template <typename T, typename U = typename std::remove_cv<
                          typename std::remove_reference<T>::type>::type>
constexpr Maybe<U> Some(T&& aValue);

template <class T>
class Maybe
  : private detail::MaybeStorage<T> {
  template <typename U, typename V>
  friend constexpr Maybe<V> Some(U&& aValue);

  struct SomeGuard {};

  template <typename U>
  constexpr Maybe(U&& aValue, SomeGuard)
      : detail::MaybeStorage<T>{std::forward<U>(aValue)} {}

  using detail::MaybeStorage<T>::mIsSome;
  using detail::MaybeStorage<T>::mStorage;

 public:
  using ValueType = T;

  constexpr Maybe() = default;

  constexpr Maybe(Nothing) : Maybe{} {}

  template <typename... Args>
  constexpr explicit Maybe(Args&&... aArgs)
      : detail::MaybeStorage<T>{std::forward<Args>(aArgs)...} {}

  /**
   * Maybe<T> can be copy-constructed from a Maybe<U> if T is constructible from
   * a const U&.
   */
  template <typename U,
            typename = std::enable_if_t<std::is_constructible_v<T, const U&>>>
  Maybe(const Maybe<U>& aOther) {
    if (aOther.isSome()) {
      emplace(*aOther);
    }
  }

  /**
   * Maybe<T> can be move-constructed from a Maybe<U> if T is constructible from
   * a U&&.
   */
  template <typename U,
            typename = std::enable_if_t<std::is_constructible_v<T, U&&>>>
  Maybe(Maybe<U>&& aOther) {
    if (aOther.isSome()) {
      emplace(std::move(*aOther));
      aOther.reset();
    }
  }

  template <typename U,
            typename = std::enable_if_t<std::is_constructible_v<T, const U&>>>
  Maybe& operator=(const Maybe<U>& aOther) {
    if (aOther.isSome()) {
      if (mIsSome) {
        ref() = aOther.ref();
      } else {
        emplace(*aOther);
      }
    } else {
      reset();
    }
    return *this;
  }

  template <typename U,
            typename = std::enable_if_t<std::is_constructible_v<T, U&&>>>
  Maybe& operator=(Maybe<U>&& aOther) {
    if (aOther.isSome()) {
      if (mIsSome) {
        ref() = std::move(aOther.ref());
      } else {
        emplace(std::move(*aOther));
      }
      aOther.reset();
    } else {
      reset();
    }

    return *this;
  }

  constexpr Maybe& operator=(Nothing) {
    reset();
    return *this;
  }

  /* Methods that check whether this Maybe contains a value */
  constexpr explicit operator bool() const { return isSome(); }
  constexpr bool isSome() const { return mIsSome; }
  constexpr bool isNothing() const { return !mIsSome; }

  /* Returns the contents of this Maybe<T> by value. Unsafe unless |isSome()|.
   */
  constexpr T value() const;

  /**
   * Move the contents of this Maybe<T> out of internal storage and return it
   * without calling the destructor. The internal storage is also reset to
   * avoid multiple calls. Unsafe unless |isSome()|.
   */
  T extract() {
    T v = std::move(mStorage.val);
    reset();
    return v;
  }

  /* Returns the contents of this Maybe<T> by pointer. Unsafe unless |isSome()|.
   */
  T* ptr();
  constexpr const T* ptr() const;

  constexpr T* operator->();
  constexpr const T* operator->() const;

  /* Returns the contents of this Maybe<T> by ref. Unsafe unless |isSome()|. */
  constexpr T& ref();
  constexpr const T& ref() const;

  constexpr T& operator*();
  constexpr const T& operator*() const;

  /* If |isSome()|, empties this Maybe and destroys its contents. */
  constexpr void reset() {
    if (isSome()) {
      if constexpr (!std::is_trivially_destructible_v<T>) {
        ref().T::~T();
      }
      mIsSome = false;
    }
  }

  /*
   * Constructs a T value in-place in this empty Maybe<T>'s storage. The
   * arguments to |emplace()| are the parameters to T's constructor.
   */
  template <typename... Args>
  constexpr void emplace(Args&&... aArgs);

  template <typename U>
  constexpr std::enable_if_t<std::is_same_v<T, U> &&
                             std::is_copy_constructible_v<U> &&
                             !std::is_move_constructible_v<U>>
  emplace(U&& aArgs) {
    emplace(aArgs);
  }
};

static Maybe<int> intTest;
static Maybe<double> doubleTest;

static Maybe<Cell*> cellPtrTest;
struct CellStruct { Cell* cell; bool dummy; };
static Maybe<CellStruct> cellStructTest;
struct NonCellStruct { void* pointer; bool dummy; };
static Maybe<NonCellStruct> nonCellStructTest;

int checksize(std::size_t sss) {
  return sss;
}
