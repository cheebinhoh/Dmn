// Generated by the protocol buffer compiler.  DO NOT EDIT!
// NO CHECKED-IN PROTOBUF GENCODE
// source: proto/dmn-dmesg.proto
// Protobuf C++ Version: 6.30.0-rc1

#include "proto/dmn-dmesg.pb.h"

#include <algorithm>
#include <type_traits>
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/generated_message_tctable_impl.h"
#include "google/protobuf/extension_set.h"
#include "google/protobuf/generated_message_util.h"
#include "google/protobuf/wire_format_lite.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/generated_message_reflection.h"
#include "google/protobuf/reflection_ops.h"
#include "google/protobuf/wire_format.h"
// @@protoc_insertion_point(includes)

// Must be included last.
#include "google/protobuf/port_def.inc"
PROTOBUF_PRAGMA_INIT_SEG
namespace _pb = ::google::protobuf;
namespace _pbi = ::google::protobuf::internal;
namespace _fl = ::google::protobuf::internal::field_layout;
namespace Dmn {

inline constexpr DMesgPb::Impl_::Impl_(
    ::_pbi::ConstantInitialized) noexcept
      : _cached_size_{0},
        topic_(
            &::google::protobuf::internal::fixed_address_empty_string,
            ::_pbi::ConstantInitialized()),
        sourceidentifier_(
            &::google::protobuf::internal::fixed_address_empty_string,
            ::_pbi::ConstantInitialized()),
        timestamp_{nullptr},
        body_{nullptr},
        runningcounter_{::uint64_t{0u}},
        type_{static_cast< ::Dmn::DMesgTypePb >(0)} {}

template <typename>
PROTOBUF_CONSTEXPR DMesgPb::DMesgPb(::_pbi::ConstantInitialized)
#if defined(PROTOBUF_CUSTOM_VTABLE)
    : ::google::protobuf::Message(DMesgPb_class_data_.base()),
#else   // PROTOBUF_CUSTOM_VTABLE
    : ::google::protobuf::Message(),
#endif  // PROTOBUF_CUSTOM_VTABLE
      _impl_(::_pbi::ConstantInitialized()) {
}
struct DMesgPbDefaultTypeInternal {
  PROTOBUF_CONSTEXPR DMesgPbDefaultTypeInternal() : _instance(::_pbi::ConstantInitialized{}) {}
  ~DMesgPbDefaultTypeInternal() {}
  union {
    DMesgPb _instance;
  };
};

PROTOBUF_ATTRIBUTE_NO_DESTROY PROTOBUF_CONSTINIT
    PROTOBUF_ATTRIBUTE_INIT_PRIORITY1 DMesgPbDefaultTypeInternal _DMesgPb_default_instance_;
}  // namespace Dmn
static constexpr const ::_pb::EnumDescriptor *PROTOBUF_NONNULL *PROTOBUF_NULLABLE
    file_level_enum_descriptors_proto_2fdmn_2ddmesg_2eproto = nullptr;
static constexpr const ::_pb::ServiceDescriptor *PROTOBUF_NONNULL *PROTOBUF_NULLABLE
    file_level_service_descriptors_proto_2fdmn_2ddmesg_2eproto = nullptr;
const ::uint32_t
    TableStruct_proto_2fdmn_2ddmesg_2eproto::offsets[] ABSL_ATTRIBUTE_SECTION_VARIABLE(
        protodesc_cold) = {
        PROTOBUF_FIELD_OFFSET(::Dmn::DMesgPb, _impl_._has_bits_),
        PROTOBUF_FIELD_OFFSET(::Dmn::DMesgPb, _internal_metadata_),
        ~0u,  // no _extensions_
        ~0u,  // no _oneof_case_
        ~0u,  // no _weak_field_map_
        ~0u,  // no _inlined_string_donated_
        ~0u,  // no _split_
        ~0u,  // no sizeof(Split)
        PROTOBUF_FIELD_OFFSET(::Dmn::DMesgPb, _impl_.timestamp_),
        PROTOBUF_FIELD_OFFSET(::Dmn::DMesgPb, _impl_.topic_),
        PROTOBUF_FIELD_OFFSET(::Dmn::DMesgPb, _impl_.runningcounter_),
        PROTOBUF_FIELD_OFFSET(::Dmn::DMesgPb, _impl_.sourceidentifier_),
        PROTOBUF_FIELD_OFFSET(::Dmn::DMesgPb, _impl_.type_),
        PROTOBUF_FIELD_OFFSET(::Dmn::DMesgPb, _impl_.body_),
        2,
        0,
        4,
        1,
        5,
        3,
};

static const ::_pbi::MigrationSchema
    schemas[] ABSL_ATTRIBUTE_SECTION_VARIABLE(protodesc_cold) = {
        {0, 14, -1, sizeof(::Dmn::DMesgPb)},
};
static const ::_pb::Message* PROTOBUF_NONNULL const file_default_instances[] = {
    &::Dmn::_DMesgPb_default_instance_._instance,
};
const char descriptor_table_protodef_proto_2fdmn_2ddmesg_2eproto[] ABSL_ATTRIBUTE_SECTION_VARIABLE(
    protodesc_cold) = {
    "\n\025proto/dmn-dmesg.proto\022\003Dmn\032\032proto/dmn-"
    "dmesg-type.proto\032\032proto/dmn-dmesg-body.p"
    "roto\032\037google/protobuf/timestamp.proto\"\271\001"
    "\n\007DMesgPb\022-\n\ttimestamp\030\001 \001(\0132\032.google.pr"
    "otobuf.Timestamp\022\r\n\005topic\030\002 \001(\t\022\026\n\016runni"
    "ngCounter\030\003 \001(\004\022\030\n\020sourceIdentifier\030\004 \001("
    "\t\022\036\n\004type\030\005 \001(\0162\020.Dmn.DMesgTypePb\022\036\n\004bod"
    "y\030\n \001(\0132\020.Dmn.DMesgBodyPbb\006proto3"
};
static const ::_pbi::DescriptorTable* PROTOBUF_NONNULL const
    descriptor_table_proto_2fdmn_2ddmesg_2eproto_deps[3] = {
        &::descriptor_table_google_2fprotobuf_2ftimestamp_2eproto,
        &::descriptor_table_proto_2fdmn_2ddmesg_2dbody_2eproto,
        &::descriptor_table_proto_2fdmn_2ddmesg_2dtype_2eproto,
};
static ::absl::once_flag descriptor_table_proto_2fdmn_2ddmesg_2eproto_once;
PROTOBUF_CONSTINIT const ::_pbi::DescriptorTable descriptor_table_proto_2fdmn_2ddmesg_2eproto = {
    false,
    false,
    313,
    descriptor_table_protodef_proto_2fdmn_2ddmesg_2eproto,
    "proto/dmn-dmesg.proto",
    &descriptor_table_proto_2fdmn_2ddmesg_2eproto_once,
    descriptor_table_proto_2fdmn_2ddmesg_2eproto_deps,
    3,
    1,
    schemas,
    file_default_instances,
    TableStruct_proto_2fdmn_2ddmesg_2eproto::offsets,
    file_level_enum_descriptors_proto_2fdmn_2ddmesg_2eproto,
    file_level_service_descriptors_proto_2fdmn_2ddmesg_2eproto,
};
namespace Dmn {
// ===================================================================

class DMesgPb::_Internal {
 public:
  using HasBits =
      decltype(std::declval<DMesgPb>()._impl_._has_bits_);
  static constexpr ::int32_t kHasBitsOffset =
      8 * PROTOBUF_FIELD_OFFSET(DMesgPb, _impl_._has_bits_);
};

void DMesgPb::clear_timestamp() {
  ::google::protobuf::internal::TSanWrite(&_impl_);
  if (_impl_.timestamp_ != nullptr) _impl_.timestamp_->Clear();
  _impl_._has_bits_[0] &= ~0x00000004u;
}
void DMesgPb::clear_body() {
  ::google::protobuf::internal::TSanWrite(&_impl_);
  if (_impl_.body_ != nullptr) _impl_.body_->Clear();
  _impl_._has_bits_[0] &= ~0x00000008u;
}
DMesgPb::DMesgPb(::google::protobuf::Arena* PROTOBUF_NULLABLE arena)
#if defined(PROTOBUF_CUSTOM_VTABLE)
    : ::google::protobuf::Message(arena, DMesgPb_class_data_.base()) {
#else   // PROTOBUF_CUSTOM_VTABLE
    : ::google::protobuf::Message(arena) {
#endif  // PROTOBUF_CUSTOM_VTABLE
  SharedCtor(arena);
  // @@protoc_insertion_point(arena_constructor:Dmn.DMesgPb)
}
PROTOBUF_NDEBUG_INLINE DMesgPb::Impl_::Impl_(
    ::google::protobuf::internal::InternalVisibility visibility,
    ::google::protobuf::Arena* PROTOBUF_NULLABLE arena, const Impl_& from,
    const ::Dmn::DMesgPb& from_msg)
      : _has_bits_{from._has_bits_},
        _cached_size_{0},
        topic_(arena, from.topic_),
        sourceidentifier_(arena, from.sourceidentifier_) {}

DMesgPb::DMesgPb(
    ::google::protobuf::Arena* PROTOBUF_NULLABLE arena,
    const DMesgPb& from)
#if defined(PROTOBUF_CUSTOM_VTABLE)
    : ::google::protobuf::Message(arena, DMesgPb_class_data_.base()) {
#else   // PROTOBUF_CUSTOM_VTABLE
    : ::google::protobuf::Message(arena) {
#endif  // PROTOBUF_CUSTOM_VTABLE
  DMesgPb* const _this = this;
  (void)_this;
  _internal_metadata_.MergeFrom<::google::protobuf::UnknownFieldSet>(
      from._internal_metadata_);
  new (&_impl_) Impl_(internal_visibility(), arena, from._impl_, from);
  ::uint32_t cached_has_bits = _impl_._has_bits_[0];
  _impl_.timestamp_ = ((cached_has_bits & 0x00000004u) != 0)
                ? ::google::protobuf::Message::CopyConstruct(arena, *from._impl_.timestamp_)
                : nullptr;
  _impl_.body_ = ((cached_has_bits & 0x00000008u) != 0)
                ? ::google::protobuf::Message::CopyConstruct(arena, *from._impl_.body_)
                : nullptr;
  ::memcpy(reinterpret_cast<char *>(&_impl_) +
               offsetof(Impl_, runningcounter_),
           reinterpret_cast<const char *>(&from._impl_) +
               offsetof(Impl_, runningcounter_),
           offsetof(Impl_, type_) -
               offsetof(Impl_, runningcounter_) +
               sizeof(Impl_::type_));

  // @@protoc_insertion_point(copy_constructor:Dmn.DMesgPb)
}
PROTOBUF_NDEBUG_INLINE DMesgPb::Impl_::Impl_(
    ::google::protobuf::internal::InternalVisibility visibility,
    ::google::protobuf::Arena* PROTOBUF_NULLABLE arena)
      : _cached_size_{0},
        topic_(arena),
        sourceidentifier_(arena) {}

inline void DMesgPb::SharedCtor(::_pb::Arena* PROTOBUF_NULLABLE arena) {
  new (&_impl_) Impl_(internal_visibility(), arena);
  ::memset(reinterpret_cast<char *>(&_impl_) +
               offsetof(Impl_, timestamp_),
           0,
           offsetof(Impl_, type_) -
               offsetof(Impl_, timestamp_) +
               sizeof(Impl_::type_));
}
DMesgPb::~DMesgPb() {
  // @@protoc_insertion_point(destructor:Dmn.DMesgPb)
  SharedDtor(*this);
}
inline void DMesgPb::SharedDtor(MessageLite& self) {
  DMesgPb& this_ = static_cast<DMesgPb&>(self);
  this_._internal_metadata_.Delete<::google::protobuf::UnknownFieldSet>();
  ABSL_DCHECK(this_.GetArena() == nullptr);
  this_._impl_.topic_.Destroy();
  this_._impl_.sourceidentifier_.Destroy();
  delete this_._impl_.timestamp_;
  delete this_._impl_.body_;
  this_._impl_.~Impl_();
}

inline void* PROTOBUF_NONNULL DMesgPb::PlacementNew_(
    const void* PROTOBUF_NONNULL, void* PROTOBUF_NONNULL mem,
    ::google::protobuf::Arena* PROTOBUF_NULLABLE arena) {
  return ::new (mem) DMesgPb(arena);
}
constexpr auto DMesgPb::InternalNewImpl_() {
  return ::google::protobuf::internal::MessageCreator::CopyInit(sizeof(DMesgPb),
                                            alignof(DMesgPb));
}
constexpr auto DMesgPb::InternalGenerateClassData_() {
  return ::google::protobuf::internal::ClassDataFull{
      ::google::protobuf::internal::ClassData{
          &_DMesgPb_default_instance_._instance,
          &_table_.header,
          nullptr,  // OnDemandRegisterArenaDtor
          nullptr,  // IsInitialized
          &DMesgPb::MergeImpl,
          ::google::protobuf::Message::GetNewImpl<DMesgPb>(),
#if defined(PROTOBUF_CUSTOM_VTABLE)
          &DMesgPb::SharedDtor,
          ::google::protobuf::Message::GetClearImpl<DMesgPb>(), &DMesgPb::ByteSizeLong,
              &DMesgPb::_InternalSerialize,
#endif  // PROTOBUF_CUSTOM_VTABLE
          PROTOBUF_FIELD_OFFSET(DMesgPb, _impl_._cached_size_),
          false,
      },
      &DMesgPb::kDescriptorMethods,
      &descriptor_table_proto_2fdmn_2ddmesg_2eproto,
      nullptr,  // tracker
  };
}

PROTOBUF_CONSTINIT
    PROTOBUF_ATTRIBUTE_INIT_PRIORITY1 const ::google::protobuf::internal::ClassDataFull
        DMesgPb_class_data_ =
            DMesgPb::InternalGenerateClassData_();

const ::google::protobuf::internal::ClassData* PROTOBUF_NONNULL DMesgPb::GetClassData() const {
  ::google::protobuf::internal::PrefetchToLocalCache(&DMesgPb_class_data_);
  ::google::protobuf::internal::PrefetchToLocalCache(DMesgPb_class_data_.tc_table);
  return DMesgPb_class_data_.base();
}
PROTOBUF_CONSTINIT PROTOBUF_ATTRIBUTE_INIT_PRIORITY1
const ::_pbi::TcParseTable<3, 6, 2, 41, 2>
DMesgPb::_table_ = {
  {
    PROTOBUF_FIELD_OFFSET(DMesgPb, _impl_._has_bits_),
    0, // no _extensions_
    10, 56,  // max_field_number, fast_idx_mask
    offsetof(decltype(_table_), field_lookup_table),
    4294966752,  // skipmap
    offsetof(decltype(_table_), field_entries),
    6,  // num_field_entries
    2,  // num_aux_entries
    offsetof(decltype(_table_), aux_entries),
    DMesgPb_class_data_.base(),
    nullptr,  // post_loop_handler
    ::_pbi::TcParser::GenericFallback,  // fallback
    #ifdef PROTOBUF_PREFETCH_PARSE_TABLE
    ::_pbi::TcParser::GetTable<::Dmn::DMesgPb>(),  // to_prefetch
    #endif  // PROTOBUF_PREFETCH_PARSE_TABLE
  }, {{
    {::_pbi::TcParser::MiniParse, {}},
    // .google.protobuf.Timestamp timestamp = 1;
    {::_pbi::TcParser::FastMtS1,
     {10, 2, 0, PROTOBUF_FIELD_OFFSET(DMesgPb, _impl_.timestamp_)}},
    // string topic = 2;
    {::_pbi::TcParser::FastUS1,
     {18, 0, 0, PROTOBUF_FIELD_OFFSET(DMesgPb, _impl_.topic_)}},
    // uint64 runningCounter = 3;
    {::_pbi::TcParser::SingularVarintNoZag1<::uint64_t, offsetof(DMesgPb, _impl_.runningcounter_), 4>(),
     {24, 4, 0, PROTOBUF_FIELD_OFFSET(DMesgPb, _impl_.runningcounter_)}},
    // string sourceIdentifier = 4;
    {::_pbi::TcParser::FastUS1,
     {34, 1, 0, PROTOBUF_FIELD_OFFSET(DMesgPb, _impl_.sourceidentifier_)}},
    // .Dmn.DMesgTypePb type = 5;
    {::_pbi::TcParser::SingularVarintNoZag1<::uint32_t, offsetof(DMesgPb, _impl_.type_), 5>(),
     {40, 5, 0, PROTOBUF_FIELD_OFFSET(DMesgPb, _impl_.type_)}},
    {::_pbi::TcParser::MiniParse, {}},
    {::_pbi::TcParser::MiniParse, {}},
  }}, {{
    65535, 65535
  }}, {{
    // .google.protobuf.Timestamp timestamp = 1;
    {PROTOBUF_FIELD_OFFSET(DMesgPb, _impl_.timestamp_), _Internal::kHasBitsOffset + 2, 0,
    (0 | ::_fl::kFcOptional | ::_fl::kMessage | ::_fl::kTvTable)},
    // string topic = 2;
    {PROTOBUF_FIELD_OFFSET(DMesgPb, _impl_.topic_), _Internal::kHasBitsOffset + 0, 0,
    (0 | ::_fl::kFcOptional | ::_fl::kUtf8String | ::_fl::kRepAString)},
    // uint64 runningCounter = 3;
    {PROTOBUF_FIELD_OFFSET(DMesgPb, _impl_.runningcounter_), _Internal::kHasBitsOffset + 4, 0,
    (0 | ::_fl::kFcOptional | ::_fl::kUInt64)},
    // string sourceIdentifier = 4;
    {PROTOBUF_FIELD_OFFSET(DMesgPb, _impl_.sourceidentifier_), _Internal::kHasBitsOffset + 1, 0,
    (0 | ::_fl::kFcOptional | ::_fl::kUtf8String | ::_fl::kRepAString)},
    // .Dmn.DMesgTypePb type = 5;
    {PROTOBUF_FIELD_OFFSET(DMesgPb, _impl_.type_), _Internal::kHasBitsOffset + 5, 0,
    (0 | ::_fl::kFcOptional | ::_fl::kOpenEnum)},
    // .Dmn.DMesgBodyPb body = 10;
    {PROTOBUF_FIELD_OFFSET(DMesgPb, _impl_.body_), _Internal::kHasBitsOffset + 3, 1,
    (0 | ::_fl::kFcOptional | ::_fl::kMessage | ::_fl::kTvTable)},
  }},
  {{
      {::_pbi::TcParser::GetTable<::google::protobuf::Timestamp>()},
      {::_pbi::TcParser::GetTable<::Dmn::DMesgBodyPb>()},
  }},
  {{
    "\13\0\5\0\20\0\0\0"
    "Dmn.DMesgPb"
    "topic"
    "sourceIdentifier"
  }},
};
PROTOBUF_NOINLINE void DMesgPb::Clear() {
// @@protoc_insertion_point(message_clear_start:Dmn.DMesgPb)
  ::google::protobuf::internal::TSanWrite(&_impl_);
  ::uint32_t cached_has_bits = 0;
  // Prevent compiler warnings about cached_has_bits being unused
  (void) cached_has_bits;

  cached_has_bits = _impl_._has_bits_[0];
  if ((cached_has_bits & 0x0000000fu) != 0) {
    if ((cached_has_bits & 0x00000001u) != 0) {
      _impl_.topic_.ClearNonDefaultToEmpty();
    }
    if ((cached_has_bits & 0x00000002u) != 0) {
      _impl_.sourceidentifier_.ClearNonDefaultToEmpty();
    }
    if ((cached_has_bits & 0x00000004u) != 0) {
      ABSL_DCHECK(_impl_.timestamp_ != nullptr);
      _impl_.timestamp_->Clear();
    }
    if ((cached_has_bits & 0x00000008u) != 0) {
      ABSL_DCHECK(_impl_.body_ != nullptr);
      _impl_.body_->Clear();
    }
  }
  if ((cached_has_bits & 0x00000030u) != 0) {
    ::memset(&_impl_.runningcounter_, 0, static_cast<::size_t>(
        reinterpret_cast<char*>(&_impl_.type_) -
        reinterpret_cast<char*>(&_impl_.runningcounter_)) + sizeof(_impl_.type_));
  }
  _impl_._has_bits_.Clear();
  _internal_metadata_.Clear<::google::protobuf::UnknownFieldSet>();
}

#if defined(PROTOBUF_CUSTOM_VTABLE)
::uint8_t* PROTOBUF_NONNULL DMesgPb::_InternalSerialize(
    const ::google::protobuf::MessageLite& base, ::uint8_t* PROTOBUF_NONNULL target,
    ::google::protobuf::io::EpsCopyOutputStream* PROTOBUF_NONNULL stream) {
  const DMesgPb& this_ = static_cast<const DMesgPb&>(base);
#else   // PROTOBUF_CUSTOM_VTABLE
::uint8_t* PROTOBUF_NONNULL DMesgPb::_InternalSerialize(
    ::uint8_t* PROTOBUF_NONNULL target,
    ::google::protobuf::io::EpsCopyOutputStream* PROTOBUF_NONNULL stream) const {
  const DMesgPb& this_ = *this;
#endif  // PROTOBUF_CUSTOM_VTABLE
  // @@protoc_insertion_point(serialize_to_array_start:Dmn.DMesgPb)
  ::uint32_t cached_has_bits = 0;
  (void)cached_has_bits;

  cached_has_bits = this_._impl_._has_bits_[0];
  // .google.protobuf.Timestamp timestamp = 1;
  if ((cached_has_bits & 0x00000004u) != 0) {
    target = ::google::protobuf::internal::WireFormatLite::InternalWriteMessage(
        1, *this_._impl_.timestamp_, this_._impl_.timestamp_->GetCachedSize(), target,
        stream);
  }

  // string topic = 2;
  if ((cached_has_bits & 0x00000001u) != 0) {
    if (!this_._internal_topic().empty()) {
      const std::string& _s = this_._internal_topic();
      ::google::protobuf::internal::WireFormatLite::VerifyUtf8String(
          _s.data(), static_cast<int>(_s.length()), ::google::protobuf::internal::WireFormatLite::SERIALIZE, "Dmn.DMesgPb.topic");
      target = stream->WriteStringMaybeAliased(2, _s, target);
    }
  }

  // uint64 runningCounter = 3;
  if ((cached_has_bits & 0x00000010u) != 0) {
    if (this_._internal_runningcounter() != 0) {
      target = stream->EnsureSpace(target);
      target = ::_pbi::WireFormatLite::WriteUInt64ToArray(
          3, this_._internal_runningcounter(), target);
    }
  }

  // string sourceIdentifier = 4;
  if ((cached_has_bits & 0x00000002u) != 0) {
    if (!this_._internal_sourceidentifier().empty()) {
      const std::string& _s = this_._internal_sourceidentifier();
      ::google::protobuf::internal::WireFormatLite::VerifyUtf8String(
          _s.data(), static_cast<int>(_s.length()), ::google::protobuf::internal::WireFormatLite::SERIALIZE, "Dmn.DMesgPb.sourceIdentifier");
      target = stream->WriteStringMaybeAliased(4, _s, target);
    }
  }

  // .Dmn.DMesgTypePb type = 5;
  if ((cached_has_bits & 0x00000020u) != 0) {
    if (this_._internal_type() != 0) {
      target = stream->EnsureSpace(target);
      target = ::_pbi::WireFormatLite::WriteEnumToArray(
          5, this_._internal_type(), target);
    }
  }

  // .Dmn.DMesgBodyPb body = 10;
  if ((cached_has_bits & 0x00000008u) != 0) {
    target = ::google::protobuf::internal::WireFormatLite::InternalWriteMessage(
        10, *this_._impl_.body_, this_._impl_.body_->GetCachedSize(), target,
        stream);
  }

  if (ABSL_PREDICT_FALSE(this_._internal_metadata_.have_unknown_fields())) {
    target =
        ::_pbi::WireFormat::InternalSerializeUnknownFieldsToArray(
            this_._internal_metadata_.unknown_fields<::google::protobuf::UnknownFieldSet>(::google::protobuf::UnknownFieldSet::default_instance), target, stream);
  }
  // @@protoc_insertion_point(serialize_to_array_end:Dmn.DMesgPb)
  return target;
}

#if defined(PROTOBUF_CUSTOM_VTABLE)
::size_t DMesgPb::ByteSizeLong(const MessageLite& base) {
  const DMesgPb& this_ = static_cast<const DMesgPb&>(base);
#else   // PROTOBUF_CUSTOM_VTABLE
::size_t DMesgPb::ByteSizeLong() const {
  const DMesgPb& this_ = *this;
#endif  // PROTOBUF_CUSTOM_VTABLE
  // @@protoc_insertion_point(message_byte_size_start:Dmn.DMesgPb)
  ::size_t total_size = 0;

  ::uint32_t cached_has_bits = 0;
  // Prevent compiler warnings about cached_has_bits being unused
  (void)cached_has_bits;

  ::_pbi::Prefetch5LinesFrom7Lines(&this_);
  cached_has_bits = this_._impl_._has_bits_[0];
  if ((cached_has_bits & 0x0000003fu) != 0) {
    // string topic = 2;
    if ((cached_has_bits & 0x00000001u) != 0) {
      if (!this_._internal_topic().empty()) {
        total_size += 1 + ::google::protobuf::internal::WireFormatLite::StringSize(
                                        this_._internal_topic());
      }
    }
    // string sourceIdentifier = 4;
    if ((cached_has_bits & 0x00000002u) != 0) {
      if (!this_._internal_sourceidentifier().empty()) {
        total_size += 1 + ::google::protobuf::internal::WireFormatLite::StringSize(
                                        this_._internal_sourceidentifier());
      }
    }
    // .google.protobuf.Timestamp timestamp = 1;
    if ((cached_has_bits & 0x00000004u) != 0) {
      total_size += 1 +
                    ::google::protobuf::internal::WireFormatLite::MessageSize(*this_._impl_.timestamp_);
    }
    // .Dmn.DMesgBodyPb body = 10;
    if ((cached_has_bits & 0x00000008u) != 0) {
      total_size += 1 +
                    ::google::protobuf::internal::WireFormatLite::MessageSize(*this_._impl_.body_);
    }
    // uint64 runningCounter = 3;
    if ((cached_has_bits & 0x00000010u) != 0) {
      if (this_._internal_runningcounter() != 0) {
        total_size += ::_pbi::WireFormatLite::UInt64SizePlusOne(
            this_._internal_runningcounter());
      }
    }
    // .Dmn.DMesgTypePb type = 5;
    if ((cached_has_bits & 0x00000020u) != 0) {
      if (this_._internal_type() != 0) {
        total_size += 1 +
                      ::_pbi::WireFormatLite::EnumSize(this_._internal_type());
      }
    }
  }
  return this_.MaybeComputeUnknownFieldsSize(total_size,
                                             &this_._impl_._cached_size_);
}

void DMesgPb::MergeImpl(::google::protobuf::MessageLite& to_msg, const ::google::protobuf::MessageLite& from_msg) {
  auto* const _this = static_cast<DMesgPb*>(&to_msg);
  auto& from = static_cast<const DMesgPb&>(from_msg);
  ::google::protobuf::Arena* arena = _this->GetArena();
  // @@protoc_insertion_point(class_specific_merge_from_start:Dmn.DMesgPb)
  ABSL_DCHECK_NE(&from, _this);
  ::uint32_t cached_has_bits = 0;
  (void) cached_has_bits;

  cached_has_bits = from._impl_._has_bits_[0];
  if ((cached_has_bits & 0x0000003fu) != 0) {
    if ((cached_has_bits & 0x00000001u) != 0) {
      if (!from._internal_topic().empty()) {
        _this->_internal_set_topic(from._internal_topic());
      } else {
        if (_this->_impl_.topic_.IsDefault()) {
          _this->_internal_set_topic("");
        }
      }
    }
    if ((cached_has_bits & 0x00000002u) != 0) {
      if (!from._internal_sourceidentifier().empty()) {
        _this->_internal_set_sourceidentifier(from._internal_sourceidentifier());
      } else {
        if (_this->_impl_.sourceidentifier_.IsDefault()) {
          _this->_internal_set_sourceidentifier("");
        }
      }
    }
    if ((cached_has_bits & 0x00000004u) != 0) {
      ABSL_DCHECK(from._impl_.timestamp_ != nullptr);
      if (_this->_impl_.timestamp_ == nullptr) {
        _this->_impl_.timestamp_ = ::google::protobuf::Message::CopyConstruct(arena, *from._impl_.timestamp_);
      } else {
        _this->_impl_.timestamp_->MergeFrom(*from._impl_.timestamp_);
      }
    }
    if ((cached_has_bits & 0x00000008u) != 0) {
      ABSL_DCHECK(from._impl_.body_ != nullptr);
      if (_this->_impl_.body_ == nullptr) {
        _this->_impl_.body_ = ::google::protobuf::Message::CopyConstruct(arena, *from._impl_.body_);
      } else {
        _this->_impl_.body_->MergeFrom(*from._impl_.body_);
      }
    }
    if ((cached_has_bits & 0x00000010u) != 0) {
      if (from._internal_runningcounter() != 0) {
        _this->_impl_.runningcounter_ = from._impl_.runningcounter_;
      }
    }
    if ((cached_has_bits & 0x00000020u) != 0) {
      if (from._internal_type() != 0) {
        _this->_impl_.type_ = from._impl_.type_;
      }
    }
  }
  _this->_impl_._has_bits_[0] |= cached_has_bits;
  _this->_internal_metadata_.MergeFrom<::google::protobuf::UnknownFieldSet>(from._internal_metadata_);
}

void DMesgPb::CopyFrom(const DMesgPb& from) {
// @@protoc_insertion_point(class_specific_copy_from_start:Dmn.DMesgPb)
  if (&from == this) return;
  Clear();
  MergeFrom(from);
}


void DMesgPb::InternalSwap(DMesgPb* PROTOBUF_RESTRICT PROTOBUF_NONNULL other) {
  using std::swap;
  auto* arena = GetArena();
  ABSL_DCHECK_EQ(arena, other->GetArena());
  _internal_metadata_.InternalSwap(&other->_internal_metadata_);
  swap(_impl_._has_bits_[0], other->_impl_._has_bits_[0]);
  ::_pbi::ArenaStringPtr::InternalSwap(&_impl_.topic_, &other->_impl_.topic_, arena);
  ::_pbi::ArenaStringPtr::InternalSwap(&_impl_.sourceidentifier_, &other->_impl_.sourceidentifier_, arena);
  ::google::protobuf::internal::memswap<
      PROTOBUF_FIELD_OFFSET(DMesgPb, _impl_.type_)
      + sizeof(DMesgPb::_impl_.type_)
      - PROTOBUF_FIELD_OFFSET(DMesgPb, _impl_.timestamp_)>(
          reinterpret_cast<char*>(&_impl_.timestamp_),
          reinterpret_cast<char*>(&other->_impl_.timestamp_));
}

::google::protobuf::Metadata DMesgPb::GetMetadata() const {
  return ::google::protobuf::Message::GetMetadataImpl(GetClassData()->full());
}
// @@protoc_insertion_point(namespace_scope)
}  // namespace Dmn
namespace google {
namespace protobuf {
}  // namespace protobuf
}  // namespace google
// @@protoc_insertion_point(global_scope)
PROTOBUF_ATTRIBUTE_INIT_PRIORITY2 static ::std::false_type
    _static_init2_ [[maybe_unused]] =
        (::_pbi::AddDescriptors(&descriptor_table_proto_2fdmn_2ddmesg_2eproto),
         ::std::false_type{});
#include "google/protobuf/port_undef.inc"
