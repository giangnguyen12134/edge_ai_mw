// Copyright 2026 Genio Middleware Authors.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

#include "mw/common/json.h"
#include "mw/robotic/jpeg_encoder.h"
#include "mw/robotic/result_json.h"
#include "mw/robotic/robotic_config.h"
#include "mw/robotic/tcp_sender.h"
#include "tests/test_framework.h"

namespace mw {

MW_TEST(ResultJson, EscapesControlAndQuoteCharacters) {
  MW_EXPECT_EQ(JsonEscape("a\"b\\c\n\t"), std::string("a\\\"b\\\\c\\n\\t"));
  MW_EXPECT_EQ(JsonEscape(std::string("\x01", 1)), std::string("\\u0001"));
  MW_EXPECT_EQ(JsonEscape("plain"), std::string("plain"));
}

MW_TEST(ResultJson, OutputParsesBackWithStrictParser) {
  InferenceResult r;
  r.model = "object";
  r.kind = "detection";
  r.sequence = 42;
  r.timestamp_ns = 123456789012ull;
  r.items.push_back(ResultItem("per\"son", 0.91, 0.1, 0.2, 0.3, 0.4));
  r.items.push_back(ResultItem("wave", 0.5, 0, 0, 0, 0));
  const std::string line = InferenceResultToJson(r);
  MW_EXPECT_TRUE(line.find('\n') == std::string::npos);  // One line.

  JsonValue v;
  MW_ASSERT_OK(JsonValue::Parse(line, &v));
  MW_EXPECT_EQ(v.GetString("model", ""), std::string("object"));
  MW_EXPECT_EQ(v.GetInt("seq", 0), 42);
  const JsonValue* items = v.Find("items");
  MW_ASSERT_TRUE(items != nullptr && items->array().size() == 2);
  MW_EXPECT_EQ(items->array()[0].GetString("label", ""),
               std::string("per\"son"));
  MW_EXPECT_TRUE(items->array()[0].Find("box") != nullptr);
  MW_EXPECT_TRUE(items->array()[1].Find("box") == nullptr);  // Non-spatial.
}

MW_TEST(ResultJson, NonFiniteScoresStayValidJson) {
  InferenceResult r;
  r.model = "m";
  r.kind = "speaker";
  r.items.push_back(ResultItem("x", 1.0 / 0.0, 0, 0, 0, 0));
  JsonValue v;
  MW_EXPECT_OK(JsonValue::Parse(InferenceResultToJson(r), &v));
}

MW_TEST(ResultJson, EmptyResultIsValid) {
  InferenceResult r;
  r.model = "face";
  r.kind = "detection";
  JsonValue v;
  MW_ASSERT_OK(JsonValue::Parse(InferenceResultToJson(r), &v));
  MW_EXPECT_EQ(v.Find("items")->array().size(), static_cast<size_t>(0));
}

MW_TEST(RoboticConfig, OverridesApply) {
  RoboticConfig config;
  MW_ASSERT_OK(ApplyRoboticConfigOverride("jetson.host=192.168.55.1", &config));
  MW_EXPECT_OK(ValidateRoboticConfig(config));
}

MW_TEST(RoboticConfig, RejectsBadValues) {
  RoboticConfig config;
  MW_ASSERT_OK(ApplyRoboticConfigOverride("jetson.host=10.0.0.2", &config));
  config.jpeg_quality = 0;
  MW_EXPECT_FALSE(ValidateRoboticConfig(config).ok());
  config.jpeg_quality = 101;
  MW_EXPECT_FALSE(ValidateRoboticConfig(config).ok());
  MW_EXPECT_FALSE(ApplyRoboticConfigOverride("nonsense=1", &config).ok());
}

MW_TEST(RoboticConfig, ShippedConfigsLoad) {
  RoboticConfig config;
  MW_EXPECT_OK(
      LoadRoboticConfig(MW_SOURCE_DIR "/config/dev/robotic.json", &config));
  MW_EXPECT_OK(ValidateRoboticConfig(config));
}

namespace {

// Listens on 127.0.0.1 with an ephemeral port.
int ListenLocal(uint16_t* port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (fd < 0 ||
      bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      listen(fd, 1) != 0) {
    return -1;
  }
  socklen_t len = sizeof(addr);
  getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  *port = ntohs(addr.sin_port);
  return fd;
}

}  // namespace

MW_TEST(TcpSender, DeliversBytesToLocalServer) {
  uint16_t port = 0;
  const int server = ListenLocal(&port);
  MW_ASSERT_TRUE(server >= 0);
  TcpSender sender;
  MW_ASSERT_OK(sender.Connect("127.0.0.1", port, 1000));
  MW_EXPECT_TRUE(sender.connected());
  const int peer = accept(server, nullptr, nullptr);
  MW_ASSERT_TRUE(peer >= 0);

  const std::string text = "{\"hello\":1}\n";
  MW_ASSERT_OK(sender.SendAll(reinterpret_cast<const uint8_t*>(text.data()),
                              text.size(), 1000));
  char buffer[64];
  const ssize_t n = read(peer, buffer, sizeof(buffer));
  MW_EXPECT_EQ(static_cast<size_t>(n), text.size());
  MW_EXPECT_EQ(std::string(buffer, static_cast<size_t>(n)), text);
  close(peer);
  close(server);
}

MW_TEST(TcpSender, ConnectFailureLeavesItDisconnected) {
  uint16_t port = 0;
  const int server = ListenLocal(&port);
  MW_ASSERT_TRUE(server >= 0);
  close(server);  // Nothing listens on the port any more.
  TcpSender sender;
  MW_EXPECT_FALSE(sender.Connect("127.0.0.1", port, 500).ok());
  MW_EXPECT_FALSE(sender.connected());
  const uint8_t byte = 0;
  MW_EXPECT_FALSE(sender.SendAll(&byte, 1, 100).ok());
}

MW_TEST(TcpSender, SendAfterPeerCloseFailsAndDisconnects) {
  uint16_t port = 0;
  const int server = ListenLocal(&port);
  MW_ASSERT_TRUE(server >= 0);
  TcpSender sender;
  MW_ASSERT_OK(sender.Connect("127.0.0.1", port, 1000));
  const int peer = accept(server, nullptr, nullptr);
  close(peer);
  close(server);
  std::vector<uint8_t> big(1 << 20, 1);
  Status status = Status::Ok();
  for (int i = 0; i < 20 && status.ok(); ++i) {
    status = sender.SendAll(big.data(), big.size(), 500);
  }
  MW_EXPECT_FALSE(status.ok());  // Must not raise SIGPIPE either.
  MW_EXPECT_FALSE(sender.connected());
}

#ifdef MW_HAVE_JPEG
MW_TEST(JpegEncoder, ProducesJpegStream) {
  std::unique_ptr<JpegEncoder> encoder;
  MW_ASSERT_OK(CreateJpegEncoder("libjpeg", &encoder));
  const FrameFormat format = FrameFormat::Nv12(64, 48);
  std::vector<uint8_t> nv12(format.buffer_size, 128);
  std::vector<uint8_t> jpeg;
  MW_ASSERT_OK(encoder->EncodeNv12(nv12.data(), format, 80, &jpeg));
  MW_ASSERT_TRUE(jpeg.size() > 4);
  MW_EXPECT_EQ(jpeg[0], static_cast<uint8_t>(0xFF));
  MW_EXPECT_EQ(jpeg[1], static_cast<uint8_t>(0xD8));
  MW_EXPECT_EQ(jpeg[jpeg.size() - 2], static_cast<uint8_t>(0xFF));
  MW_EXPECT_EQ(jpeg[jpeg.size() - 1], static_cast<uint8_t>(0xD9));
}
#endif

MW_TEST(JpegEncoder, UnknownEncoderIsRejected) {
  std::unique_ptr<JpegEncoder> encoder;
  MW_EXPECT_FALSE(CreateJpegEncoder("nvjpeg", &encoder).ok());
}

}  // namespace mw
