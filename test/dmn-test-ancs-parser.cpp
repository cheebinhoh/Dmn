/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-ancs-parser.cpp
 * @brief Protocol-level tests for bounded ANCS event and response parsing.
 */

#include <gtest/gtest.h>

#include "dmn-ancs-client.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

void appendUint16(Bytes &bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void appendUint32(Bytes &bytes, std::uint32_t value) {
  for (unsigned int shift = 0; shift < 32; shift += 8) {
    bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }
}

void appendAttribute(Bytes &bytes, std::uint8_t id, std::string_view value) {
  bytes.push_back(id);
  appendUint16(bytes, static_cast<std::uint16_t>(value.size()));
  for (const auto character : value) {
    bytes.push_back(static_cast<std::uint8_t>(character));
  }
}

auto makeAttributeResponse(std::uint32_t uid) -> Bytes {
  Bytes response{0};
  appendUint32(response, uid);
  appendAttribute(response, 0, "com.example.mail");
  appendAttribute(response, 1, "Example title");
  appendAttribute(response, 2, "");
  appendAttribute(response, 3, "Message \xcf\x80");
  appendAttribute(response, 5, "2026-10-16T21:00:00Z");

  return response;
}

auto consumeCompleteResponse(const Bytes &response, std::uint32_t uid)
    -> dmn::Dmn_AncsValueResult<
        std::optional<dmn::detail::Dmn_AncsAttributeSet>> {
  dmn::detail::Dmn_AncsDataSourceReassembler parser{uid};

  return parser.consume(response);
}

TEST(DmnAncsParserTest, RejectsNotificationSourceValuesWithWrongLength) {
  const std::array<std::uint8_t, 7> shortValue{};
  const std::array<std::uint8_t, 9> longValue{};

  for (const auto value : {std::span<const std::uint8_t>{},
                           std::span<const std::uint8_t>{shortValue},
                           std::span<const std::uint8_t>{longValue}}) {
    const auto parsed = dmn::detail::parseAncsNotificationSource(value);
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().kind, dmn::Dmn_AncsErrorKind::Protocol);
    EXPECT_TRUE(parsed.error().fatal);
  }
}

TEST(DmnAncsParserTest, ParsesEventFieldsAndPreservesUnknownRawValues) {
  const std::array<std::uint8_t, 8> bytes{1,    0xa5, 0xfe, 7,
                                          0x78, 0x56, 0x34, 0x12};

  const auto parsed = dmn::detail::parseAncsNotificationSource(bytes);

  ASSERT_TRUE(parsed) << parsed.error().message;
  EXPECT_EQ(parsed->kind, dmn::Dmn_AncsNotificationKind::Updated);
  EXPECT_EQ(parsed->flags, 0xa5);
  EXPECT_EQ(parsed->category, 0xfe);
  EXPECT_EQ(parsed->category_count, 7);
  EXPECT_EQ(parsed->uid, 0x12345678U);
}

TEST(DmnAncsParserTest, ReportsReservedEventAsNonfatalUnsupportedInput) {
  const std::array<std::uint8_t, 8> bytes{3, 0, 0, 0, 1, 0, 0, 0};

  const auto parsed = dmn::detail::parseAncsNotificationSource(bytes);

  ASSERT_FALSE(parsed);
  EXPECT_EQ(parsed.error().kind, dmn::Dmn_AncsErrorKind::Unsupported);
  EXPECT_FALSE(parsed.error().fatal);
}

TEST(DmnAncsParserTest, ParsesEveryKnownNotificationEventKind) {
  const std::array<dmn::Dmn_AncsNotificationKind, 3> expectedKinds{
      dmn::Dmn_AncsNotificationKind::Added,
      dmn::Dmn_AncsNotificationKind::Updated,
      dmn::Dmn_AncsNotificationKind::Removed};

  for (std::uint8_t eventId = 0; eventId < expectedKinds.size(); ++eventId) {
    const std::array<std::uint8_t, 8> bytes{eventId, 0, 0, 0, 0, 0, 0, 0};
    const auto parsed = dmn::detail::parseAncsNotificationSource(bytes);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->kind, expectedKinds[eventId]);
  }
}

TEST(DmnAncsParserTest, ReassemblesResponseAtEveryTwoFragmentBoundary) {
  constexpr std::uint32_t kUid = 0x78563412U;
  const auto response = makeAttributeResponse(kUid);

  for (std::size_t split = 1; split < response.size(); ++split) {
    dmn::detail::Dmn_AncsDataSourceReassembler parser{kUid};
    const auto first =
        parser.consume(std::span<const std::uint8_t>{response}.first(split));
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_FALSE(first->has_value()) << "split=" << split;

    const auto second =
        parser.consume(std::span<const std::uint8_t>{response}.subspan(split));
    ASSERT_TRUE(second) << second.error().message;
    ASSERT_TRUE(second->has_value()) << "split=" << split;
    EXPECT_EQ((*second)->app_identifier, "com.example.mail");
    EXPECT_EQ((*second)->title, "Example title");
    ASSERT_TRUE((*second)->subtitle.has_value());
    EXPECT_TRUE((*second)->subtitle->empty());
    EXPECT_EQ((*second)->message, "Message \xcf\x80");
    EXPECT_EQ((*second)->date, "2026-10-16T21:00:00Z");
  }
}

TEST(DmnAncsParserTest, ReassemblesOneByteFragments) {
  constexpr std::uint32_t kUid = 0x10203040U;
  const auto response = makeAttributeResponse(kUid);
  dmn::detail::Dmn_AncsDataSourceReassembler parser{kUid};
  std::optional<dmn::detail::Dmn_AncsAttributeSet> completed;

  for (const auto byte : response) {
    const std::array<std::uint8_t, 1> fragment{byte};
    const auto result = parser.consume(fragment);
    ASSERT_TRUE(result) << result.error().message;
    if (result->has_value()) {
      completed = std::move(**result);
    }
  }

  ASSERT_TRUE(completed.has_value());
  EXPECT_EQ(completed->app_identifier, "com.example.mail");
  EXPECT_EQ(completed->message, "Message \xcf\x80");
}

TEST(DmnAncsParserTest,
     SkipsUnknownBoundedAttributesWithoutReorderingKnownOnes) {
  constexpr std::uint32_t kUid = 99;
  Bytes response{0};
  appendUint32(response, kUid);
  appendAttribute(response, 200, "future");
  appendAttribute(response, 0, "app");
  appendAttribute(response, 1, "title");
  appendAttribute(response, 2, "subtitle");
  appendAttribute(response, 3, "message");
  appendAttribute(response, 5, "date");

  const auto parsed = consumeCompleteResponse(response, kUid);

  ASSERT_TRUE(parsed) << parsed.error().message;
  ASSERT_TRUE(parsed->has_value());
  EXPECT_EQ((*parsed)->app_identifier, "app");
  EXPECT_EQ((*parsed)->title, "title");
  EXPECT_EQ((*parsed)->date, "date");
}

TEST(DmnAncsParserTest, RejectsWrongCommandAndMismatchedUid) {
  Bytes wrongCommand{1};
  appendUint32(wrongCommand, 1);
  const auto commandResult = consumeCompleteResponse(wrongCommand, 1);
  ASSERT_FALSE(commandResult);
  EXPECT_EQ(commandResult.error().kind, dmn::Dmn_AncsErrorKind::Protocol);

  Bytes wrongUid{0};
  appendUint32(wrongUid, 2);
  const auto uidResult = consumeCompleteResponse(wrongUid, 1);
  ASSERT_FALSE(uidResult);
  EXPECT_EQ(uidResult.error().kind, dmn::Dmn_AncsErrorKind::Protocol);
}

TEST(DmnAncsParserTest, RejectsKnownAttributesOutOfRequestedOrder) {
  Bytes response{0};
  appendUint32(response, 7);
  appendAttribute(response, 1, "title");

  const auto parsed = consumeCompleteResponse(response, 7);

  ASSERT_FALSE(parsed);
  EXPECT_EQ(parsed.error().kind, dmn::Dmn_AncsErrorKind::Protocol);
}

TEST(DmnAncsParserTest, RejectsOversizedResponseAndAttributeValues) {
  const Bytes tooLarge(8193, 0);
  dmn::detail::Dmn_AncsDataSourceReassembler largeParser{1};
  const auto largeResult = largeParser.consume(tooLarge);
  ASSERT_FALSE(largeResult);
  EXPECT_EQ(largeResult.error().kind, dmn::Dmn_AncsErrorKind::ResourceLimit);

  Bytes response{0};
  appendUint32(response, 5);
  appendAttribute(response, 0, "app");
  appendAttribute(response, 1, std::string(257, 'x'));
  dmn::detail::Dmn_AncsDataSourceReassembler attributeParser{5};
  const auto attributeResult = attributeParser.consume(response);
  ASSERT_FALSE(attributeResult);
  EXPECT_EQ(attributeResult.error().kind,
            dmn::Dmn_AncsErrorKind::ResourceLimit);
}

TEST(DmnAncsParserTest, InvalidUtf8IsNonfatalAndRawBytesAreNotReturned) {
  Bytes response{0};
  appendUint32(response, 6);
  response.push_back(0);
  appendUint16(response, 1);
  response.push_back(0xff);
  dmn::detail::Dmn_AncsDataSourceReassembler parser{6};

  const auto parsed = parser.consume(response);

  ASSERT_FALSE(parsed);
  EXPECT_EQ(parsed.error().kind, dmn::Dmn_AncsErrorKind::Protocol);
  EXPECT_FALSE(parsed.error().fatal);
  EXPECT_EQ(parsed.error().message, "ANCS attribute is not valid UTF-8");
}

TEST(DmnAncsParserTest,
     IncompleteResponseRemainsIncompleteUntilAllTuplesArrive) {
  constexpr std::uint32_t kUid = 10;
  Bytes response{0};
  appendUint32(response, kUid);
  appendAttribute(response, 0, "app");
  appendAttribute(response, 1, "title");
  appendAttribute(response, 2, "subtitle");
  appendAttribute(response, 3, "message");
  dmn::detail::Dmn_AncsDataSourceReassembler parser{kUid};

  const auto partial = parser.consume(response);
  ASSERT_TRUE(partial) << partial.error().message;
  EXPECT_FALSE(partial->has_value());

  Bytes finalAttribute{};
  appendAttribute(finalAttribute, 5, "date");
  const auto complete = parser.consume(finalAttribute);
  ASSERT_TRUE(complete) << complete.error().message;
  ASSERT_TRUE(complete->has_value());
  EXPECT_EQ((*complete)->date, "date");
}

} // namespace
