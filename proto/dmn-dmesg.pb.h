// Generated by the protocol buffer compiler.  DO NOT EDIT!
// NO CHECKED-IN PROTOBUF GENCODE
// source: proto/dmn-dmesg.proto
// Protobuf C++ Version: 6.30.0-rc1

#ifndef proto_2fdmn_2ddmesg_2eproto_2epb_2eh
#define proto_2fdmn_2ddmesg_2eproto_2epb_2eh

#include <limits>
#include <string>
#include <type_traits>
#include <utility>

#include "google/protobuf/runtime_version.h"
#if PROTOBUF_VERSION != 6030000
#error "Protobuf C++ gencode is built with an incompatible version of"
#error "Protobuf C++ headers/runtime. See"
#error "https://protobuf.dev/support/cross-version-runtime-guarantee/#cpp"
#endif
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/arena.h"
#include "google/protobuf/arenastring.h"
#include "google/protobuf/generated_message_tctable_decl.h"
#include "google/protobuf/generated_message_util.h"
#include "google/protobuf/metadata_lite.h"
#include "google/protobuf/generated_message_reflection.h"
#include "google/protobuf/message.h"
#include "google/protobuf/message_lite.h"
#include "google/protobuf/repeated_field.h"  // IWYU pragma: export
#include "google/protobuf/extension_set.h"  // IWYU pragma: export
#include "google/protobuf/unknown_field_set.h"
#include "proto/dmn-dmesg-type.pb.h"
#include "proto/dmn-dmesg-body.pb.h"
#include "google/protobuf/timestamp.pb.h"
// @@protoc_insertion_point(includes)

// Must be included last.
#include "google/protobuf/port_def.inc"

#define PROTOBUF_INTERNAL_EXPORT_proto_2fdmn_2ddmesg_2eproto

namespace google {
namespace protobuf {
namespace internal {
template <typename T>
::absl::string_view GetAnyMessageName();
}  // namespace internal
}  // namespace protobuf
}  // namespace google

// Internal implementation detail -- do not use these members.
struct TableStruct_proto_2fdmn_2ddmesg_2eproto {
  static const ::uint32_t offsets[];
};
extern "C" {
extern const ::google::protobuf::internal::DescriptorTable descriptor_table_proto_2fdmn_2ddmesg_2eproto;
}  // extern "C"
namespace Dmn {
class DMesgPb;
struct DMesgPbDefaultTypeInternal;
extern DMesgPbDefaultTypeInternal _DMesgPb_default_instance_;
extern const ::google::protobuf::internal::ClassDataFull DMesgPb_class_data_;
}  // namespace Dmn
namespace google {
namespace protobuf {
}  // namespace protobuf
}  // namespace google

namespace Dmn {

// ===================================================================


// -------------------------------------------------------------------

class DMesgPb final : public ::google::protobuf::Message
/* @@protoc_insertion_point(class_definition:Dmn.DMesgPb) */ {
 public:
  inline DMesgPb() : DMesgPb(nullptr) {}
  ~DMesgPb() PROTOBUF_FINAL;

#if defined(PROTOBUF_CUSTOM_VTABLE)
  void operator delete(DMesgPb* PROTOBUF_NONNULL msg, std::destroying_delete_t) {
    SharedDtor(*msg);
    ::google::protobuf::internal::SizedDelete(msg, sizeof(DMesgPb));
  }
#endif

  template <typename = void>
  explicit PROTOBUF_CONSTEXPR DMesgPb(::google::protobuf::internal::ConstantInitialized);

  inline DMesgPb(const DMesgPb& from) : DMesgPb(nullptr, from) {}
  inline DMesgPb(DMesgPb&& from) noexcept
      : DMesgPb(nullptr, std::move(from)) {}
  inline DMesgPb& operator=(const DMesgPb& from) {
    CopyFrom(from);
    return *this;
  }
  inline DMesgPb& operator=(DMesgPb&& from) noexcept {
    if (this == &from) return *this;
    if (::google::protobuf::internal::CanMoveWithInternalSwap(GetArena(), from.GetArena())) {
      InternalSwap(&from);
    } else {
      CopyFrom(from);
    }
    return *this;
  }

  inline const ::google::protobuf::UnknownFieldSet& unknown_fields() const
      ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return _internal_metadata_.unknown_fields<::google::protobuf::UnknownFieldSet>(::google::protobuf::UnknownFieldSet::default_instance);
  }
  inline ::google::protobuf::UnknownFieldSet* PROTOBUF_NONNULL mutable_unknown_fields()
      ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return _internal_metadata_.mutable_unknown_fields<::google::protobuf::UnknownFieldSet>();
  }

  static const ::google::protobuf::Descriptor* PROTOBUF_NONNULL descriptor() {
    return GetDescriptor();
  }
  static const ::google::protobuf::Descriptor* PROTOBUF_NONNULL GetDescriptor() {
    return default_instance().GetMetadata().descriptor;
  }
  static const ::google::protobuf::Reflection* PROTOBUF_NONNULL GetReflection() {
    return default_instance().GetMetadata().reflection;
  }
  static const DMesgPb& default_instance() {
    return *reinterpret_cast<const DMesgPb*>(
        &_DMesgPb_default_instance_);
  }
  static constexpr int kIndexInFileMessages = 0;
  friend void swap(DMesgPb& a, DMesgPb& b) { a.Swap(&b); }
  inline void Swap(DMesgPb* PROTOBUF_NONNULL other) {
    if (other == this) return;
    if (::google::protobuf::internal::CanUseInternalSwap(GetArena(), other->GetArena())) {
      InternalSwap(other);
    } else {
      ::google::protobuf::internal::GenericSwap(this, other);
    }
  }
  void UnsafeArenaSwap(DMesgPb* PROTOBUF_NONNULL other) {
    if (other == this) return;
    ABSL_DCHECK(GetArena() == other->GetArena());
    InternalSwap(other);
  }

  // implements Message ----------------------------------------------

  DMesgPb* PROTOBUF_NONNULL New(::google::protobuf::Arena* PROTOBUF_NULLABLE arena = nullptr) const {
    return ::google::protobuf::Message::DefaultConstruct<DMesgPb>(arena);
  }
  using ::google::protobuf::Message::CopyFrom;
  void CopyFrom(const DMesgPb& from);
  using ::google::protobuf::Message::MergeFrom;
  void MergeFrom(const DMesgPb& from) { DMesgPb::MergeImpl(*this, from); }

  private:
  static void MergeImpl(::google::protobuf::MessageLite& to_msg,
                        const ::google::protobuf::MessageLite& from_msg);

  public:
  bool IsInitialized() const {
    return true;
  }
  ABSL_ATTRIBUTE_REINITIALIZES void Clear() PROTOBUF_FINAL;
  #if defined(PROTOBUF_CUSTOM_VTABLE)
  private:
  static ::size_t ByteSizeLong(const ::google::protobuf::MessageLite& msg);
  static ::uint8_t* PROTOBUF_NONNULL _InternalSerialize(
      const ::google::protobuf::MessageLite& msg, ::uint8_t* PROTOBUF_NONNULL target,
      ::google::protobuf::io::EpsCopyOutputStream* PROTOBUF_NONNULL stream);

  public:
  ::size_t ByteSizeLong() const { return ByteSizeLong(*this); }
  ::uint8_t* PROTOBUF_NONNULL _InternalSerialize(
      ::uint8_t* PROTOBUF_NONNULL target,
      ::google::protobuf::io::EpsCopyOutputStream* PROTOBUF_NONNULL stream) const {
    return _InternalSerialize(*this, target, stream);
  }
  #else   // PROTOBUF_CUSTOM_VTABLE
  ::size_t ByteSizeLong() const final;
  ::uint8_t* PROTOBUF_NONNULL _InternalSerialize(
      ::uint8_t* PROTOBUF_NONNULL target,
      ::google::protobuf::io::EpsCopyOutputStream* PROTOBUF_NONNULL stream) const final;
  #endif  // PROTOBUF_CUSTOM_VTABLE
  int GetCachedSize() const { return _impl_._cached_size_.Get(); }

  private:
  void SharedCtor(::google::protobuf::Arena* PROTOBUF_NULLABLE arena);
  static void SharedDtor(MessageLite& self);
  void InternalSwap(DMesgPb* PROTOBUF_NONNULL other);
 private:
  template <typename T>
  friend ::absl::string_view(::google::protobuf::internal::GetAnyMessageName)();
  static ::absl::string_view FullMessageName() { return "Dmn.DMesgPb"; }

 protected:
  explicit DMesgPb(::google::protobuf::Arena* PROTOBUF_NULLABLE arena);
  DMesgPb(::google::protobuf::Arena* PROTOBUF_NULLABLE arena, const DMesgPb& from);
  DMesgPb(
      ::google::protobuf::Arena* PROTOBUF_NULLABLE arena, DMesgPb&& from) noexcept
      : DMesgPb(arena) {
    *this = ::std::move(from);
  }
  const ::google::protobuf::internal::ClassData* PROTOBUF_NONNULL GetClassData() const PROTOBUF_FINAL;
  static void* PROTOBUF_NONNULL PlacementNew_(
      const void* PROTOBUF_NONNULL, void* PROTOBUF_NONNULL mem,
      ::google::protobuf::Arena* PROTOBUF_NULLABLE arena);
  static constexpr auto InternalNewImpl_();

 public:
  static constexpr auto InternalGenerateClassData_();

  ::google::protobuf::Metadata GetMetadata() const;
  // nested types ----------------------------------------------------

  // accessors -------------------------------------------------------
  enum : int {
    kTopicFieldNumber = 2,
    kSourceIdentifierFieldNumber = 4,
    kTimestampFieldNumber = 1,
    kBodyFieldNumber = 10,
    kRunningCounterFieldNumber = 3,
    kTypeFieldNumber = 5,
  };
  // string topic = 2;
  void clear_topic() ;
  const std::string& topic() const;
  template <typename Arg_ = const std::string&, typename... Args_>
  void set_topic(Arg_&& arg, Args_... args);
  std::string* PROTOBUF_NONNULL mutable_topic();
  [[nodiscard]] std::string* PROTOBUF_NULLABLE release_topic();
  void set_allocated_topic(std::string* PROTOBUF_NULLABLE value);

  private:
  const std::string& _internal_topic() const;
  PROTOBUF_ALWAYS_INLINE void _internal_set_topic(const std::string& value);
  std::string* PROTOBUF_NONNULL _internal_mutable_topic();

  public:
  // string sourceIdentifier = 4;
  void clear_sourceidentifier() ;
  const std::string& sourceidentifier() const;
  template <typename Arg_ = const std::string&, typename... Args_>
  void set_sourceidentifier(Arg_&& arg, Args_... args);
  std::string* PROTOBUF_NONNULL mutable_sourceidentifier();
  [[nodiscard]] std::string* PROTOBUF_NULLABLE release_sourceidentifier();
  void set_allocated_sourceidentifier(std::string* PROTOBUF_NULLABLE value);

  private:
  const std::string& _internal_sourceidentifier() const;
  PROTOBUF_ALWAYS_INLINE void _internal_set_sourceidentifier(const std::string& value);
  std::string* PROTOBUF_NONNULL _internal_mutable_sourceidentifier();

  public:
  // .google.protobuf.Timestamp timestamp = 1;
  bool has_timestamp() const;
  void clear_timestamp() ;
  const ::google::protobuf::Timestamp& timestamp() const;
  [[nodiscard]] ::google::protobuf::Timestamp* PROTOBUF_NULLABLE release_timestamp();
  ::google::protobuf::Timestamp* PROTOBUF_NONNULL mutable_timestamp();
  void set_allocated_timestamp(::google::protobuf::Timestamp* PROTOBUF_NULLABLE value);
  void unsafe_arena_set_allocated_timestamp(::google::protobuf::Timestamp* PROTOBUF_NULLABLE value);
  ::google::protobuf::Timestamp* PROTOBUF_NULLABLE unsafe_arena_release_timestamp();

  private:
  const ::google::protobuf::Timestamp& _internal_timestamp() const;
  ::google::protobuf::Timestamp* PROTOBUF_NONNULL _internal_mutable_timestamp();

  public:
  // .Dmn.DMesgBodyPb body = 10;
  bool has_body() const;
  void clear_body() ;
  const ::Dmn::DMesgBodyPb& body() const;
  [[nodiscard]] ::Dmn::DMesgBodyPb* PROTOBUF_NULLABLE release_body();
  ::Dmn::DMesgBodyPb* PROTOBUF_NONNULL mutable_body();
  void set_allocated_body(::Dmn::DMesgBodyPb* PROTOBUF_NULLABLE value);
  void unsafe_arena_set_allocated_body(::Dmn::DMesgBodyPb* PROTOBUF_NULLABLE value);
  ::Dmn::DMesgBodyPb* PROTOBUF_NULLABLE unsafe_arena_release_body();

  private:
  const ::Dmn::DMesgBodyPb& _internal_body() const;
  ::Dmn::DMesgBodyPb* PROTOBUF_NONNULL _internal_mutable_body();

  public:
  // uint64 runningCounter = 3;
  void clear_runningcounter() ;
  ::uint64_t runningcounter() const;
  void set_runningcounter(::uint64_t value);

  private:
  ::uint64_t _internal_runningcounter() const;
  void _internal_set_runningcounter(::uint64_t value);

  public:
  // .Dmn.DMesgTypePb type = 5;
  void clear_type() ;
  ::Dmn::DMesgTypePb type() const;
  void set_type(::Dmn::DMesgTypePb value);

  private:
  ::Dmn::DMesgTypePb _internal_type() const;
  void _internal_set_type(::Dmn::DMesgTypePb value);

  public:
  // @@protoc_insertion_point(class_scope:Dmn.DMesgPb)
 private:
  class _Internal;
  friend class ::google::protobuf::internal::TcParser;
  static const ::google::protobuf::internal::TcParseTable<3, 6,
                                   2, 41,
                                   2>
      _table_;

  friend class ::google::protobuf::MessageLite;
  friend class ::google::protobuf::Arena;
  template <typename T>
  friend class ::google::protobuf::Arena::InternalHelper;
  using InternalArenaConstructable_ = void;
  using DestructorSkippable_ = void;
  struct Impl_ {
    inline explicit constexpr Impl_(::google::protobuf::internal::ConstantInitialized) noexcept;
    inline explicit Impl_(
        ::google::protobuf::internal::InternalVisibility visibility,
        ::google::protobuf::Arena* PROTOBUF_NULLABLE arena);
    inline explicit Impl_(
        ::google::protobuf::internal::InternalVisibility visibility,
        ::google::protobuf::Arena* PROTOBUF_NULLABLE arena, const Impl_& from,
        const DMesgPb& from_msg);
    ::google::protobuf::internal::HasBits<1> _has_bits_;
    ::google::protobuf::internal::CachedSize _cached_size_;
    ::google::protobuf::internal::ArenaStringPtr topic_;
    ::google::protobuf::internal::ArenaStringPtr sourceidentifier_;
    ::google::protobuf::Timestamp* PROTOBUF_NULLABLE timestamp_;
    ::Dmn::DMesgBodyPb* PROTOBUF_NULLABLE body_;
    ::uint64_t runningcounter_;
    int type_;
    PROTOBUF_TSAN_DECLARE_MEMBER
  };
  union { Impl_ _impl_; };
  friend struct ::TableStruct_proto_2fdmn_2ddmesg_2eproto;
};

extern const ::google::protobuf::internal::ClassDataFull DMesgPb_class_data_;

// ===================================================================




// ===================================================================


#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#endif  // __GNUC__
// -------------------------------------------------------------------

// DMesgPb

// .google.protobuf.Timestamp timestamp = 1;
inline bool DMesgPb::has_timestamp() const {
  bool value = (_impl_._has_bits_[0] & 0x00000004u) != 0;
  PROTOBUF_ASSUME(!value || _impl_.timestamp_ != nullptr);
  return value;
}
inline const ::google::protobuf::Timestamp& DMesgPb::_internal_timestamp() const {
  ::google::protobuf::internal::TSanRead(&_impl_);
  const ::google::protobuf::Timestamp* p = _impl_.timestamp_;
  return p != nullptr ? *p : reinterpret_cast<const ::google::protobuf::Timestamp&>(::google::protobuf::_Timestamp_default_instance_);
}
inline const ::google::protobuf::Timestamp& DMesgPb::timestamp() const ABSL_ATTRIBUTE_LIFETIME_BOUND {
  // @@protoc_insertion_point(field_get:Dmn.DMesgPb.timestamp)
  return _internal_timestamp();
}
inline void DMesgPb::unsafe_arena_set_allocated_timestamp(
    ::google::protobuf::Timestamp* PROTOBUF_NULLABLE value) {
  ::google::protobuf::internal::TSanWrite(&_impl_);
  if (GetArena() == nullptr) {
    delete reinterpret_cast<::google::protobuf::MessageLite*>(_impl_.timestamp_);
  }
  _impl_.timestamp_ = reinterpret_cast<::google::protobuf::Timestamp*>(value);
  if (value != nullptr) {
    _impl_._has_bits_[0] |= 0x00000004u;
  } else {
    _impl_._has_bits_[0] &= ~0x00000004u;
  }
  // @@protoc_insertion_point(field_unsafe_arena_set_allocated:Dmn.DMesgPb.timestamp)
}
inline ::google::protobuf::Timestamp* PROTOBUF_NULLABLE DMesgPb::release_timestamp() {
  ::google::protobuf::internal::TSanWrite(&_impl_);

  _impl_._has_bits_[0] &= ~0x00000004u;
  ::google::protobuf::Timestamp* released = _impl_.timestamp_;
  _impl_.timestamp_ = nullptr;
  if (::google::protobuf::internal::DebugHardenForceCopyInRelease()) {
    auto* old = reinterpret_cast<::google::protobuf::MessageLite*>(released);
    released = ::google::protobuf::internal::DuplicateIfNonNull(released);
    if (GetArena() == nullptr) {
      delete old;
    }
  } else {
    if (GetArena() != nullptr) {
      released = ::google::protobuf::internal::DuplicateIfNonNull(released);
    }
  }
  return released;
}
inline ::google::protobuf::Timestamp* PROTOBUF_NULLABLE DMesgPb::unsafe_arena_release_timestamp() {
  ::google::protobuf::internal::TSanWrite(&_impl_);
  // @@protoc_insertion_point(field_release:Dmn.DMesgPb.timestamp)

  _impl_._has_bits_[0] &= ~0x00000004u;
  ::google::protobuf::Timestamp* temp = _impl_.timestamp_;
  _impl_.timestamp_ = nullptr;
  return temp;
}
inline ::google::protobuf::Timestamp* PROTOBUF_NONNULL DMesgPb::_internal_mutable_timestamp() {
  ::google::protobuf::internal::TSanWrite(&_impl_);
  if (_impl_.timestamp_ == nullptr) {
    auto* p = ::google::protobuf::Message::DefaultConstruct<::google::protobuf::Timestamp>(GetArena());
    _impl_.timestamp_ = reinterpret_cast<::google::protobuf::Timestamp*>(p);
  }
  return _impl_.timestamp_;
}
inline ::google::protobuf::Timestamp* PROTOBUF_NONNULL DMesgPb::mutable_timestamp()
    ABSL_ATTRIBUTE_LIFETIME_BOUND {
  _impl_._has_bits_[0] |= 0x00000004u;
  ::google::protobuf::Timestamp* _msg = _internal_mutable_timestamp();
  // @@protoc_insertion_point(field_mutable:Dmn.DMesgPb.timestamp)
  return _msg;
}
inline void DMesgPb::set_allocated_timestamp(::google::protobuf::Timestamp* PROTOBUF_NULLABLE value) {
  ::google::protobuf::Arena* message_arena = GetArena();
  ::google::protobuf::internal::TSanWrite(&_impl_);
  if (message_arena == nullptr) {
    delete reinterpret_cast<::google::protobuf::MessageLite*>(_impl_.timestamp_);
  }

  if (value != nullptr) {
    ::google::protobuf::Arena* submessage_arena = reinterpret_cast<::google::protobuf::Message*>(value)->GetArena();
    if (message_arena != submessage_arena) {
      value = ::google::protobuf::internal::GetOwnedMessage(message_arena, value, submessage_arena);
    }
    _impl_._has_bits_[0] |= 0x00000004u;
  } else {
    _impl_._has_bits_[0] &= ~0x00000004u;
  }

  _impl_.timestamp_ = reinterpret_cast<::google::protobuf::Timestamp*>(value);
  // @@protoc_insertion_point(field_set_allocated:Dmn.DMesgPb.timestamp)
}

// string topic = 2;
inline void DMesgPb::clear_topic() {
  ::google::protobuf::internal::TSanWrite(&_impl_);
  _impl_.topic_.ClearToEmpty();
  _impl_._has_bits_[0] &= ~0x00000001u;
}
inline const std::string& DMesgPb::topic() const
    ABSL_ATTRIBUTE_LIFETIME_BOUND {
  // @@protoc_insertion_point(field_get:Dmn.DMesgPb.topic)
  return _internal_topic();
}
template <typename Arg_, typename... Args_>
PROTOBUF_ALWAYS_INLINE void DMesgPb::set_topic(Arg_&& arg, Args_... args) {
  ::google::protobuf::internal::TSanWrite(&_impl_);
  _impl_._has_bits_[0] |= 0x00000001u;
  _impl_.topic_.Set(static_cast<Arg_&&>(arg), args..., GetArena());
  // @@protoc_insertion_point(field_set:Dmn.DMesgPb.topic)
}
inline std::string* PROTOBUF_NONNULL DMesgPb::mutable_topic()
    ABSL_ATTRIBUTE_LIFETIME_BOUND {
  std::string* _s = _internal_mutable_topic();
  // @@protoc_insertion_point(field_mutable:Dmn.DMesgPb.topic)
  return _s;
}
inline const std::string& DMesgPb::_internal_topic() const {
  ::google::protobuf::internal::TSanRead(&_impl_);
  return _impl_.topic_.Get();
}
inline void DMesgPb::_internal_set_topic(const std::string& value) {
  ::google::protobuf::internal::TSanWrite(&_impl_);
  _impl_._has_bits_[0] |= 0x00000001u;
  _impl_.topic_.Set(value, GetArena());
}
inline std::string* PROTOBUF_NONNULL DMesgPb::_internal_mutable_topic() {
  ::google::protobuf::internal::TSanWrite(&_impl_);
  _impl_._has_bits_[0] |= 0x00000001u;
  return _impl_.topic_.Mutable( GetArena());
}
inline std::string* PROTOBUF_NULLABLE DMesgPb::release_topic() {
  ::google::protobuf::internal::TSanWrite(&_impl_);
  // @@protoc_insertion_point(field_release:Dmn.DMesgPb.topic)
  if ((_impl_._has_bits_[0] & 0x00000001u) == 0) {
    return nullptr;
  }
  _impl_._has_bits_[0] &= ~0x00000001u;
  auto* released = _impl_.topic_.Release();
  if (::google::protobuf::internal::DebugHardenForceCopyDefaultString()) {
    _impl_.topic_.Set("", GetArena());
  }
  return released;
}
inline void DMesgPb::set_allocated_topic(std::string* PROTOBUF_NULLABLE value) {
  ::google::protobuf::internal::TSanWrite(&_impl_);
  if (value != nullptr) {
    _impl_._has_bits_[0] |= 0x00000001u;
  } else {
    _impl_._has_bits_[0] &= ~0x00000001u;
  }
  _impl_.topic_.SetAllocated(value, GetArena());
  if (::google::protobuf::internal::DebugHardenForceCopyDefaultString() && _impl_.topic_.IsDefault()) {
    _impl_.topic_.Set("", GetArena());
  }
  // @@protoc_insertion_point(field_set_allocated:Dmn.DMesgPb.topic)
}

// uint64 runningCounter = 3;
inline void DMesgPb::clear_runningcounter() {
  ::google::protobuf::internal::TSanWrite(&_impl_);
  _impl_.runningcounter_ = ::uint64_t{0u};
  _impl_._has_bits_[0] &= ~0x00000010u;
}
inline ::uint64_t DMesgPb::runningcounter() const {
  // @@protoc_insertion_point(field_get:Dmn.DMesgPb.runningCounter)
  return _internal_runningcounter();
}
inline void DMesgPb::set_runningcounter(::uint64_t value) {
  _internal_set_runningcounter(value);
  _impl_._has_bits_[0] |= 0x00000010u;
  // @@protoc_insertion_point(field_set:Dmn.DMesgPb.runningCounter)
}
inline ::uint64_t DMesgPb::_internal_runningcounter() const {
  ::google::protobuf::internal::TSanRead(&_impl_);
  return _impl_.runningcounter_;
}
inline void DMesgPb::_internal_set_runningcounter(::uint64_t value) {
  ::google::protobuf::internal::TSanWrite(&_impl_);
  _impl_.runningcounter_ = value;
}

// string sourceIdentifier = 4;
inline void DMesgPb::clear_sourceidentifier() {
  ::google::protobuf::internal::TSanWrite(&_impl_);
  _impl_.sourceidentifier_.ClearToEmpty();
  _impl_._has_bits_[0] &= ~0x00000002u;
}
inline const std::string& DMesgPb::sourceidentifier() const
    ABSL_ATTRIBUTE_LIFETIME_BOUND {
  // @@protoc_insertion_point(field_get:Dmn.DMesgPb.sourceIdentifier)
  return _internal_sourceidentifier();
}
template <typename Arg_, typename... Args_>
PROTOBUF_ALWAYS_INLINE void DMesgPb::set_sourceidentifier(Arg_&& arg, Args_... args) {
  ::google::protobuf::internal::TSanWrite(&_impl_);
  _impl_._has_bits_[0] |= 0x00000002u;
  _impl_.sourceidentifier_.Set(static_cast<Arg_&&>(arg), args..., GetArena());
  // @@protoc_insertion_point(field_set:Dmn.DMesgPb.sourceIdentifier)
}
inline std::string* PROTOBUF_NONNULL DMesgPb::mutable_sourceidentifier()
    ABSL_ATTRIBUTE_LIFETIME_BOUND {
  std::string* _s = _internal_mutable_sourceidentifier();
  // @@protoc_insertion_point(field_mutable:Dmn.DMesgPb.sourceIdentifier)
  return _s;
}
inline const std::string& DMesgPb::_internal_sourceidentifier() const {
  ::google::protobuf::internal::TSanRead(&_impl_);
  return _impl_.sourceidentifier_.Get();
}
inline void DMesgPb::_internal_set_sourceidentifier(const std::string& value) {
  ::google::protobuf::internal::TSanWrite(&_impl_);
  _impl_._has_bits_[0] |= 0x00000002u;
  _impl_.sourceidentifier_.Set(value, GetArena());
}
inline std::string* PROTOBUF_NONNULL DMesgPb::_internal_mutable_sourceidentifier() {
  ::google::protobuf::internal::TSanWrite(&_impl_);
  _impl_._has_bits_[0] |= 0x00000002u;
  return _impl_.sourceidentifier_.Mutable( GetArena());
}
inline std::string* PROTOBUF_NULLABLE DMesgPb::release_sourceidentifier() {
  ::google::protobuf::internal::TSanWrite(&_impl_);
  // @@protoc_insertion_point(field_release:Dmn.DMesgPb.sourceIdentifier)
  if ((_impl_._has_bits_[0] & 0x00000002u) == 0) {
    return nullptr;
  }
  _impl_._has_bits_[0] &= ~0x00000002u;
  auto* released = _impl_.sourceidentifier_.Release();
  if (::google::protobuf::internal::DebugHardenForceCopyDefaultString()) {
    _impl_.sourceidentifier_.Set("", GetArena());
  }
  return released;
}
inline void DMesgPb::set_allocated_sourceidentifier(std::string* PROTOBUF_NULLABLE value) {
  ::google::protobuf::internal::TSanWrite(&_impl_);
  if (value != nullptr) {
    _impl_._has_bits_[0] |= 0x00000002u;
  } else {
    _impl_._has_bits_[0] &= ~0x00000002u;
  }
  _impl_.sourceidentifier_.SetAllocated(value, GetArena());
  if (::google::protobuf::internal::DebugHardenForceCopyDefaultString() && _impl_.sourceidentifier_.IsDefault()) {
    _impl_.sourceidentifier_.Set("", GetArena());
  }
  // @@protoc_insertion_point(field_set_allocated:Dmn.DMesgPb.sourceIdentifier)
}

// .Dmn.DMesgTypePb type = 5;
inline void DMesgPb::clear_type() {
  ::google::protobuf::internal::TSanWrite(&_impl_);
  _impl_.type_ = 0;
  _impl_._has_bits_[0] &= ~0x00000020u;
}
inline ::Dmn::DMesgTypePb DMesgPb::type() const {
  // @@protoc_insertion_point(field_get:Dmn.DMesgPb.type)
  return _internal_type();
}
inline void DMesgPb::set_type(::Dmn::DMesgTypePb value) {
  _internal_set_type(value);
  _impl_._has_bits_[0] |= 0x00000020u;
  // @@protoc_insertion_point(field_set:Dmn.DMesgPb.type)
}
inline ::Dmn::DMesgTypePb DMesgPb::_internal_type() const {
  ::google::protobuf::internal::TSanRead(&_impl_);
  return static_cast<::Dmn::DMesgTypePb>(_impl_.type_);
}
inline void DMesgPb::_internal_set_type(::Dmn::DMesgTypePb value) {
  ::google::protobuf::internal::TSanWrite(&_impl_);
  _impl_.type_ = value;
}

// .Dmn.DMesgBodyPb body = 10;
inline bool DMesgPb::has_body() const {
  bool value = (_impl_._has_bits_[0] & 0x00000008u) != 0;
  PROTOBUF_ASSUME(!value || _impl_.body_ != nullptr);
  return value;
}
inline const ::Dmn::DMesgBodyPb& DMesgPb::_internal_body() const {
  ::google::protobuf::internal::TSanRead(&_impl_);
  const ::Dmn::DMesgBodyPb* p = _impl_.body_;
  return p != nullptr ? *p : reinterpret_cast<const ::Dmn::DMesgBodyPb&>(::Dmn::_DMesgBodyPb_default_instance_);
}
inline const ::Dmn::DMesgBodyPb& DMesgPb::body() const ABSL_ATTRIBUTE_LIFETIME_BOUND {
  // @@protoc_insertion_point(field_get:Dmn.DMesgPb.body)
  return _internal_body();
}
inline void DMesgPb::unsafe_arena_set_allocated_body(
    ::Dmn::DMesgBodyPb* PROTOBUF_NULLABLE value) {
  ::google::protobuf::internal::TSanWrite(&_impl_);
  if (GetArena() == nullptr) {
    delete reinterpret_cast<::google::protobuf::MessageLite*>(_impl_.body_);
  }
  _impl_.body_ = reinterpret_cast<::Dmn::DMesgBodyPb*>(value);
  if (value != nullptr) {
    _impl_._has_bits_[0] |= 0x00000008u;
  } else {
    _impl_._has_bits_[0] &= ~0x00000008u;
  }
  // @@protoc_insertion_point(field_unsafe_arena_set_allocated:Dmn.DMesgPb.body)
}
inline ::Dmn::DMesgBodyPb* PROTOBUF_NULLABLE DMesgPb::release_body() {
  ::google::protobuf::internal::TSanWrite(&_impl_);

  _impl_._has_bits_[0] &= ~0x00000008u;
  ::Dmn::DMesgBodyPb* released = _impl_.body_;
  _impl_.body_ = nullptr;
  if (::google::protobuf::internal::DebugHardenForceCopyInRelease()) {
    auto* old = reinterpret_cast<::google::protobuf::MessageLite*>(released);
    released = ::google::protobuf::internal::DuplicateIfNonNull(released);
    if (GetArena() == nullptr) {
      delete old;
    }
  } else {
    if (GetArena() != nullptr) {
      released = ::google::protobuf::internal::DuplicateIfNonNull(released);
    }
  }
  return released;
}
inline ::Dmn::DMesgBodyPb* PROTOBUF_NULLABLE DMesgPb::unsafe_arena_release_body() {
  ::google::protobuf::internal::TSanWrite(&_impl_);
  // @@protoc_insertion_point(field_release:Dmn.DMesgPb.body)

  _impl_._has_bits_[0] &= ~0x00000008u;
  ::Dmn::DMesgBodyPb* temp = _impl_.body_;
  _impl_.body_ = nullptr;
  return temp;
}
inline ::Dmn::DMesgBodyPb* PROTOBUF_NONNULL DMesgPb::_internal_mutable_body() {
  ::google::protobuf::internal::TSanWrite(&_impl_);
  if (_impl_.body_ == nullptr) {
    auto* p = ::google::protobuf::Message::DefaultConstruct<::Dmn::DMesgBodyPb>(GetArena());
    _impl_.body_ = reinterpret_cast<::Dmn::DMesgBodyPb*>(p);
  }
  return _impl_.body_;
}
inline ::Dmn::DMesgBodyPb* PROTOBUF_NONNULL DMesgPb::mutable_body()
    ABSL_ATTRIBUTE_LIFETIME_BOUND {
  _impl_._has_bits_[0] |= 0x00000008u;
  ::Dmn::DMesgBodyPb* _msg = _internal_mutable_body();
  // @@protoc_insertion_point(field_mutable:Dmn.DMesgPb.body)
  return _msg;
}
inline void DMesgPb::set_allocated_body(::Dmn::DMesgBodyPb* PROTOBUF_NULLABLE value) {
  ::google::protobuf::Arena* message_arena = GetArena();
  ::google::protobuf::internal::TSanWrite(&_impl_);
  if (message_arena == nullptr) {
    delete reinterpret_cast<::google::protobuf::MessageLite*>(_impl_.body_);
  }

  if (value != nullptr) {
    ::google::protobuf::Arena* submessage_arena = reinterpret_cast<::google::protobuf::Message*>(value)->GetArena();
    if (message_arena != submessage_arena) {
      value = ::google::protobuf::internal::GetOwnedMessage(message_arena, value, submessage_arena);
    }
    _impl_._has_bits_[0] |= 0x00000008u;
  } else {
    _impl_._has_bits_[0] &= ~0x00000008u;
  }

  _impl_.body_ = reinterpret_cast<::Dmn::DMesgBodyPb*>(value);
  // @@protoc_insertion_point(field_set_allocated:Dmn.DMesgPb.body)
}

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif  // __GNUC__

// @@protoc_insertion_point(namespace_scope)
}  // namespace Dmn


// @@protoc_insertion_point(global_scope)

#include "google/protobuf/port_undef.inc"

#endif  // proto_2fdmn_2ddmesg_2eproto_2epb_2eh
